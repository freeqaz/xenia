/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/emulator_headless.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "xenia/base/cvar.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/milo_trace.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/debug/gdb_rsp/gdb_rsp_server.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/titles/dc3/dc3_runtime_telemetry.h"

DEFINE_bool(
    headless_report_real_fault, false,
    "Headless: in the periodic Thread Status Report, also print the last "
    "fault OUTSIDE the XMA register aperture [0x7FEA0000,0x7FEB0000), "
    "resolved to its guest function. The plain last_fault is almost always "
    "an XMA register write once audio starts, which masks a genuine fault. "
    "Read-only.",
    "Headless");
DEFINE_bool(rb3dx_hub_teardown_trace, false,
            "DEPRECATED: use --headless_report_real_fault.", "CPU");

namespace xe {
namespace app {

namespace {

// Hands the async logger's ring buffer to its writer thread before the
// process exits without unwinding. xe::logging has no flush barrier (a
// request for one is open with the core lane); the writer drains a batch at
// most ~50 ms after it is appended, so wait a bounded interval for it.
[[noreturn]] void ExitNow(int rc) {
  std::cout.flush();
  fflush(stdout);
  fflush(stderr);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  fflush(stdout);
  fflush(stderr);
  std::_Exit(rc);
}

// "0x%08X [name]" for a host address inside JIT code, or "" if it is not.
std::string DescribeJitAddress(cpu::Processor* processor, uint64_t host_rip,
                               const char* label) {
  auto* backend = processor ? processor->backend() : nullptr;
  auto* code_cache = backend ? backend->code_cache() : nullptr;
  auto* jit_fn = code_cache && host_rip ? code_cache->LookupFunction(host_rip)
                                        : nullptr;
  if (!jit_fn) {
    return std::string();
  }
  char buf[64];
  snprintf(buf, sizeof(buf), " %s=0x%08X [", label,
           jit_fn->MapMachineCodeToGuestAddress(host_rip));
  return buf + jit_fn->name() + "]";
}

std::string GuestFunctionName(cpu::Processor* processor, uint32_t address) {
  if (!processor || address < 0x82000000) {
    return std::string();
  }
  auto* fn = processor->QueryFunction(address);
  return fn ? fn->name() : std::string();
}

}  // namespace

EmulatorHeadless::EmulatorHeadless(Emulator* emulator)
    : emulator_(emulator),
      emulator_thread_quit_requested_(false),
      emulator_thread_event_(nullptr) {}

EmulatorHeadless::~EmulatorHeadless() {
  if (gdb_rsp_server_ && emulator_ && emulator_->processor() &&
      emulator_->processor()->debug_listener() ==
          debug::gdb_rsp::AsDebugListener(gdb_rsp_server_.get())) {
    emulator_->processor()->set_debug_listener(nullptr);
  }
  gdb_rsp_server_.reset();

  // Shutdown emulator thread
  emulator_thread_quit_requested_.store(true, std::memory_order_relaxed);
  if (emulator_thread_event_) {
    emulator_thread_event_->Set();
  }
  if (emulator_thread_.joinable()) {
    emulator_thread_.join();
  }
}

bool EmulatorHeadless::Initialize(AudioSystemFactory audio_factory,
                                  GraphicsSystemFactory graphics_factory,
                                  InputDriverFactory input_factory) {
  // Create event for emulator thread communication
  emulator_thread_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  if (!emulator_thread_event_) {
    XELOGE("Failed to create emulator thread event");
    return false;
  }

  // Setup emulator with null backends (display_window = nullptr, imgui_drawer
  // = nullptr)
  X_STATUS result = emulator_->Setup(nullptr, nullptr, true, audio_factory,
                                     graphics_factory, input_factory);
  if (XFAILED(result)) {
    XELOGE("Failed to setup emulator: {:08X}", result);
    return false;
  }

  XELOGI("Emulator initialized with headless backends");
  XELOGI("  GPU: null");
  XELOGI("  APU: nop");
  XELOGI("  HID: nop");

  gdb_rsp_server_ =
      debug::gdb_rsp::CreateServerIfEnabled(emulator_->processor());
  if (gdb_rsp_server_) {
    emulator_->processor()->set_debug_listener(
        debug::gdb_rsp::AsDebugListener(gdb_rsp_server_.get()));
  }
  return true;
}

void EmulatorHeadless::EmulatorThread(std::filesystem::path launch_path) {
  xe::threading::set_name("EmulatorHeadless");

  XELOGI("Emulator thread started, launching: {}",
         xe::path_to_utf8(launch_path));

  // Launch the game from this thread
  X_STATUS result = emulator_->LaunchPath(launch_path);
  if (XFAILED(result)) {
    XELOGE("Failed to launch title: {:08X}", result);
    exit_code_ = EXIT_FAILURE;
    std::cout << "ERROR: Failed to launch title: 0x" << std::hex << result
              << std::dec << std::endl;
    return;
  }

  XELOGI("Title launched, entering main loop...");

  // Run until quit requested
  while (!emulator_thread_quit_requested_.load(std::memory_order_relaxed)) {
    // Wait for title to exit
    emulator_->WaitUntilExit();

    // Check if another title was requested
    if (emulator_->TitleRequested()) {
      emulator_->LaunchNextTitle();
    } else {
      // Title has exited, break out of the loop
      break;
    }
  }

  xe::Dc3RuntimeTelemetryRecordBootMilestone("emulator_thread_finished");
  xe::Dc3RuntimeTelemetryEndSession("emulator_thread_finished");
  xe::cpu::MiloTraceEnd("emulator_thread_finished");
  XELOGI("Emulator thread finished");
}

void EmulatorHeadless::Run() {
  // Run with periodic status reporting until emulator thread exits.
  // This enables diagnostics even when using shell `timeout` externally.
  RunWithTimeout(-1);
}

void EmulatorHeadless::RunWithTimeout(int32_t timeout_ms) {
  // Run until timeout or emulator thread exits
  auto start_time = std::chrono::steady_clock::now();
  int64_t last_report_ms = 0;

  while (!emulator_thread_quit_requested_.load(std::memory_order_relaxed)) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start_time)
                       .count();

    if (timeout_ms >= 0 && elapsed >= timeout_ms) {
      XELOGI("Timeout of {}ms reached, terminating...", timeout_ms);
      xe::Dc3RuntimeTelemetryRecordBootMilestone("headless_timeout_reached");
      xe::Dc3RuntimeTelemetryEndSession("headless_timeout", timeout_ms);
      xe::cpu::MiloTraceEnd("headless_timeout");
      std::cout << "TIMEOUT: " << timeout_ms << "ms reached" << std::endl;
      // The emulator thread is stuck in WaitUntilExit() and cannot be cleanly
      // joined; exiting without unwinding also avoids teardown asserts.
      ExitNow(0);
    }

    if (ExceptionHandler::IsLivelockTripped()) {
      ReportFaultLivelock();
      std::cout << "FAULT_LIVELOCK_ABORT" << std::endl;
      ExitNow(70);
    }

    // Periodic thread state report every 3 seconds
    if (elapsed - last_report_ms >= 3000) {
      last_report_ms = elapsed;
      ReportThreadStatus(elapsed);
    }

    // Check if thread is still alive
    if (!emulator_thread_.joinable()) {
      break;
    }

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Wait for thread to finish (only if we exited normally, not via timeout)
  if (emulator_thread_.joinable()) {
    emulator_thread_.join();
  }
}

// The exception handler parked a guest thread that faulted the same way
// fault_spin_limit times in a row and captured its PPCContext* (host rsi).
// Describe where it was, then the caller terminates. Runs on the main thread,
// unlike an exit from the signal handler.
void EmulatorHeadless::ReportFaultLivelock() {
  uint64_t ctx_ptr = ExceptionHandler::GetLastFaultContext();
  auto* memory = emulator_ ? emulator_->memory() : nullptr;
  auto* processor = emulator_ ? emulator_->processor() : nullptr;
  fprintf(stderr, "\n=== FAULT LIVELOCK DIAGNOSIS === ctx(rsi)=0x%lX%s\n",
          static_cast<unsigned long>(ctx_ptr),
          DescribeJitAddress(processor, ExceptionHandler::GetLastFaultRip(),
                             "guest_pc")
              .c_str());
  static const char* kHostRegNames[16] = {
      "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
      "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
  fprintf(stderr, "  host GPRs:");
  for (int i = 0; i < 16; ++i) {
    fprintf(stderr, " %s=%08X", kHostRegNames[i],
            static_cast<uint32_t>(ExceptionHandler::GetLastFaultHostGpr(i)));
  }
  fprintf(stderr, "\n");
  if (ctx_ptr && memory) {
    // r1 is always synced in the context (spilled at the guest bl); most
    // other GPRs may be stale mid-block.
    auto* ctx = reinterpret_cast<cpu::ppc::PPCContext*>(ctx_ptr);
    auto read_u32 = [&](uint32_t addr, uint32_t* out) {
      auto* heap = memory->LookupHeap(addr);
      uint32_t prot = 0;
      if (!heap || !heap->QueryProtect(addr, &prot) ||
          !(prot & kMemoryProtectRead)) {
        return false;
      }
      *out = xe::load_and_swap<uint32_t>(memory->TranslateVirtual(addr));
      return true;
    };
    uint32_t sp = static_cast<uint32_t>(ctx->r[1]);
    fprintf(stderr, "  guest SP(r1)=0x%08X LR=0x%08X\n", sp,
            static_cast<uint32_t>(ctx->lr));
    // MSVC Xenon frames: [sp] = caller's SP, saved LR at [caller_sp - 8].
    for (int depth = 0; depth < 12; ++depth) {
      uint32_t back = 0, lr = 0;
      if (!read_u32(sp, &back) || back <= sp || !read_u32(back - 8, &lr)) {
        break;
      }
      fprintf(stderr, "    [%d] sp=0x%08X LR=0x%08X [%s]\n", depth, back, lr,
              GuestFunctionName(processor, lr).c_str());
      sp = back;
    }
  }
  fprintf(stderr, "=== END FAULT LIVELOCK DIAGNOSIS ===\n");
  fflush(stderr);
}

// The periodic "=== Thread Status Report (<ms>ms) === ... SIGSEGV=<n>" line is
// a harness contract (tools/fork-regress/analyze: dc3_flow.py, rb3_flow.py
// use it as the run clock). Keep its format.
void EmulatorHeadless::ReportThreadStatus(int64_t elapsed) {
  auto* kernel_state = emulator_->kernel_state();
  auto* processor = emulator_->processor();
  if (!kernel_state) {
    return;
  }
  auto threads = kernel_state->object_table()->GetObjectsByType<kernel::XThread>(
      kernel::XObject::Type::Thread);
  auto last_rip = ExceptionHandler::GetLastFaultRip();
  // XMA= counts guest stores to the XMA register aperture [0x7FEA0000,
  // 0x7FEB0000): trapped-and-emulated device register writes, recovered by
  // construction. NON_XMA = SIGSEGV - XMA is the number that means "a guest
  // access faulted".
  // XMA first, SIGSEGV second: the handler bumps SIGSEGV then XMA,
  // so SIGSEGV >= XMA here (exception_handler.h).
  uint64_t xma = ExceptionHandler::GetXmaSoftFaultCount();
  uint64_t segv = ExceptionHandler::GetSigsegvCount();
  fprintf(stderr,
          "=== Thread Status Report (%ldms) === %zu threads, SIGSEGV=%lu "
          "XMA=%lu NON_XMA=%lu last_fault=0x%lX last_rip=0x%lX%s\n",
          static_cast<long>(elapsed), threads.size(),
          static_cast<unsigned long>(segv), static_cast<unsigned long>(xma),
          static_cast<unsigned long>(segv - xma),
          static_cast<unsigned long>(ExceptionHandler::GetLastFaultAddress()),
          static_cast<unsigned long>(last_rip),
          DescribeJitAddress(processor, last_rip, "crash_guest").c_str());
  if (cvars::headless_report_real_fault || cvars::rb3dx_hub_teardown_trace) {
    auto real_rip = ExceptionHandler::GetLastRealFaultRip();
    fprintf(stderr, "  [demask] last_REAL_fault=0x%lX last_REAL_rip=0x%lX%s\n",
            static_cast<unsigned long>(
                ExceptionHandler::GetLastRealFaultAddress()),
            static_cast<unsigned long>(real_rip),
            DescribeJitAddress(processor, real_rip, "real_guest").c_str());
  }
  fflush(stderr);
  auto* memory = emulator_->memory();
  for (auto& thread : threads) {
    auto* ppc_ctx =
        thread->thread_state() ? thread->thread_state()->context() : nullptr;
    if (!ppc_ctx) {
      fprintf(stderr, "  Thread %d: <no context>\n", thread->thread_id());
      continue;
    }
    uint32_t sp = static_cast<uint32_t>(ppc_ctx->r[1]);
    uint32_t lr = static_cast<uint32_t>(ppc_ctx->lr);
    std::string lr_name = GuestFunctionName(processor, lr);
    fprintf(stderr, "  Thread %d: LR=0x%08X [%s] SP=0x%08X\n",
            thread->thread_id(), lr, lr_name.empty() ? "?" : lr_name.c_str(),
            sp);
    if (memory) {
      WalkGuestStack(thread->thread_id(), sp, lr);
    }
  }
  fflush(stderr);
}

// Back-chain walk of one thread's guest stack under the MSVC Xenon
// __savegprlr convention (saved LR at [back_chain - 8]; the generic PPC slot
// [back_chain + 4] is printed too). Reads only pages the guest heap reports
// readable, so a torn context never faults the host.
void EmulatorHeadless::WalkGuestStack(uint32_t thread_id, uint32_t sp,
                                      uint32_t lr) {
  auto* memory = emulator_->memory();
  auto* processor = emulator_->processor();
  auto read_u32 = [&](uint32_t addr, uint32_t* out) {
    if ((addr & 3) || addr < 0x00010000 || addr >= 0x80000000) {
      return false;
    }
    auto* heap = memory->LookupHeap(addr);
    uint32_t prot = 0;
    if (!heap || !heap->QueryProtect(addr, &prot) ||
        !(prot & kMemoryProtectRead)) {
      return false;
    }
    *out = xe::load_and_swap<uint32_t>(memory->TranslateVirtual(addr));
    return true;
  };
  auto is_code = [](uint32_t a) { return a >= 0x82000000 && a < 0x8A000000; };
  if ((sp & 0xFFFF) == 0) {
    return;
  }
  for (int frame = 0; frame < 20; ++frame) {
    uint32_t back_chain = 0, lr_sp4 = 0, lr_bc8 = 0;
    if (!read_u32(sp, &back_chain)) {
      fprintf(stderr, "    [%d] STOP: sp=0x%08X unreadable\n", frame, sp);
      break;
    }
    read_u32(sp + 4, &lr_sp4);
    if (back_chain >= 8) {
      read_u32(back_chain - 8, &lr_bc8);
    }
    uint32_t best_lr = is_code(lr_bc8) ? lr_bc8 : lr_sp4;
    std::string fn_name = is_code(best_lr)
                              ? GuestFunctionName(processor, best_lr)
                              : std::string();
    fprintf(stderr,
            "    [%d] sp=0x%08X back=0x%08X lr_sp4=0x%08X lr_bc8=0x%08X%s%s%s\n",
            frame, sp, back_chain, lr_sp4, lr_bc8, fn_name.empty() ? "" : " [",
            fn_name.c_str(), fn_name.empty() ? "" : "]");
    if (back_chain == 0 || back_chain <= sp) {
      fprintf(stderr, "    [%d] END: back=0x%08X\n", frame, back_chain);
      break;
    }
    sp = back_chain;
  }
}

void EmulatorHeadless::StartEmulatorThread(std::filesystem::path launch_path) {
  emulator_thread_quit_requested_.store(false, std::memory_order_relaxed);
  emulator_thread_ =
      std::thread(&EmulatorHeadless::EmulatorThread, this, launch_path);
}

void EmulatorHeadless::SetupBootReporting() {
  // Set up launch callback - reports when title is loaded
  // Delegate signature: uint32_t title_id, const std::string_view game_title
  emulator_->on_launch.AddListener(
      [this](uint32_t title_id, const std::string_view game_title) {
        std::cout << "BOOT: Title loaded successfully" << std::endl;
        std::cout << "BOOT: Title ID: 0x" << std::hex << title_id << std::dec
                  << std::endl;
        if (!game_title.empty()) {
          std::cout << "BOOT: Title Name: " << game_title << std::endl;
        }
        if (emulator_->kernel_state()) {
          std::cout << "BOOT: Kernel state initialized" << std::endl;
        }
        // Signal the emulator thread event (for synchronization if needed)
        if (emulator_thread_event_) {
          emulator_thread_event_->Set();
        }
      });

  // Set up terminate callback
  emulator_->on_terminate.AddListener(
      []() { std::cout << "BOOT: Title terminated" << std::endl; });

  // Set up exit callback
  emulator_->on_exit.AddListener(
      []() { std::cout << "BOOT: Emulator exit requested" << std::endl; });
}

}  // namespace app
}  // namespace xe
