/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#ifdef __linux__
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#endif

#include "xenia/app/emulator_headless.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_rtl.h"
#include "xenia/kernel/xthread.h"
#include "third_party/fmt/include/fmt/format.h"

#ifdef __linux__
// Signal-based JIT IP sampler: sends SIGUSR2 to a target thread,
// the handler records RIP from the signal context so the diagnostic
// thread can map it back to a guest PPC address.
static std::atomic<uintptr_t> g_sampled_rip{0};
static std::atomic<bool> g_sampler_installed{false};

static void JitIpSampleHandler(int sig, siginfo_t* info, void* context) {
  auto* uc = static_cast<ucontext_t*>(context);
  g_sampled_rip.store(
      static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]),
      std::memory_order_release);
}

static void InstallJitIpSampler() {
  if (g_sampler_installed.exchange(true)) return;
  struct sigaction sa = {};
  sa.sa_sigaction = JitIpSampleHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGUSR2, &sa, nullptr);
}
#endif  // __linux__

namespace xe {
namespace app {

EmulatorHeadless::EmulatorHeadless(Emulator* emulator)
    : emulator_(emulator),
      emulator_thread_quit_requested_(false),
      emulator_thread_event_(nullptr) {}

EmulatorHeadless::~EmulatorHeadless() {
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

  // Setup emulator with null backends (display_window = nullptr, imgui_drawer = nullptr)
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

  return true;
}

void EmulatorHeadless::EmulatorThread(std::filesystem::path launch_path) {
  xe::threading::set_name("EmulatorHeadless");

  XELOGI("Emulator thread started, launching: {}", xe::path_to_utf8(launch_path));

  // Launch the game from this thread
  X_STATUS result = emulator_->LaunchPath(launch_path);
  if (XFAILED(result)) {
    XELOGE("Failed to launch title: {:08X}", result);
    std::cout << "ERROR: Failed to launch title: 0x" << std::hex << result << std::dec << std::endl;
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

  XELOGI("Emulator thread finished");
}

void EmulatorHeadless::Run() {
  // Run until emulator thread exits (title ends)
  if (emulator_thread_.joinable()) {
    emulator_thread_.join();
  }
}

void EmulatorHeadless::RunWithTimeout(int32_t timeout_ms) {
  // Run until timeout or emulator thread exits
  auto start_time = std::chrono::steady_clock::now();
  int64_t last_report_ms = 0;

  while (!emulator_thread_quit_requested_.load(std::memory_order_relaxed)) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start_time)
                      .count();

    if (elapsed >= timeout_ms) {
      XELOGI("Timeout of {}ms reached, terminating...", timeout_ms);
      std::cout << "TIMEOUT: " << timeout_ms << "ms reached" << std::endl;
      // Use _exit to avoid assertion failures during cleanup
      // The emulator thread is stuck in WaitUntilExit() and can't be cleanly joined
      std::_Exit(0);
    }

    // Periodic thread state report every 3 seconds
    if (elapsed - last_report_ms >= 3000) {
      last_report_ms = elapsed;
      auto* kernel_state = emulator_->kernel_state();
      auto* processor = emulator_->processor();
      if (kernel_state) {
        auto threads = kernel_state->object_table()->GetObjectsByType<kernel::XThread>(
            kernel::XObject::Type::Thread);
        auto segv_count = ExceptionHandler::GetSigsegvCount();
        auto last_fault = ExceptionHandler::GetLastFaultAddress();
        auto last_rip = ExceptionHandler::GetLastFaultRip();
        fprintf(stderr, "=== Thread Status Report (%ldms) === %zu threads, SIGSEGV=%lu last_fault=0x%lX last_rip=0x%lX",
                elapsed, threads.size(), segv_count, last_fault, last_rip);
        // Map last_rip to guest function if it's in the JIT code cache
        if (last_rip != 0 && processor) {
          auto* backend = processor->backend();
          auto* code_cache = backend ? backend->code_cache() : nullptr;
          if (code_cache) {
            auto* jit_fn = code_cache->LookupFunction(last_rip);
            if (jit_fn) {
              uint32_t guest_pc = jit_fn->MapMachineCodeToGuestAddress(last_rip);
              fprintf(stderr, " crash_guest=0x%08X [%s]", guest_pc, jit_fn->name().c_str());
            }
          }
        }
        fprintf(stderr, "\n");
        fflush(stderr);
        for (auto& thread : threads) {
          auto* ppc_ctx = thread->thread_state() ? thread->thread_state()->context() : nullptr;
          if (!ppc_ctx) {
            fprintf(stderr, "  Thread %d: <no context>\n", thread->thread_id());
            fflush(stderr);
            continue;
          }
          uint32_t sp = (uint32_t)ppc_ctx->r[1];
          uint32_t lr = (uint32_t)ppc_ctx->lr;
          // Resolve LR to function name
          std::string lr_name = "?";
          if (processor && lr >= 0x82000000) {
            auto* fn = processor->QueryFunction(lr);
            if (fn) lr_name = fn->name();
          }
          fprintf(stderr, "  Thread %d: LR=0x%08X [%s] SP=0x%08X\n",
                  thread->thread_id(), lr, lr_name.c_str(), sp);
          fflush(stderr);
          // For the main game thread, dump key registers to help debug guest code spins
          if (thread->thread_id() == 6) {
            uint32_t ctr = (uint32_t)ppc_ctx->ctr;
            uint32_t r3 = (uint32_t)ppc_ctx->r[3];
            uint32_t r8 = (uint32_t)ppc_ctx->r[8];
            uint32_t r9 = (uint32_t)ppc_ctx->r[9];
            uint32_t r10 = (uint32_t)ppc_ctx->r[10];
            uint32_t r11 = (uint32_t)ppc_ctx->r[11];
            uint32_t r12 = (uint32_t)ppc_ctx->r[12];
            uint32_t r30 = (uint32_t)ppc_ctx->r[30];
            uint32_t r31 = (uint32_t)ppc_ctx->r[31];
            fprintf(stderr, "    regs: r3=0x%08X r8=0x%08X r9=0x%08X r10=0x%08X\n",
                    r3, r8, r9, r10);
            fprintf(stderr, "          r11=0x%08X r12=0x%08X r30=0x%08X r31=0x%08X CTR=0x%08X\n",
                    r11, r12, r30, r31, ctr);
            fflush(stderr);
#ifdef __linux__
            // Sample the game thread's actual x86 instruction pointer via SIGUSR2.
            // PPCContext is stale during JIT execution, so this is the only way to
            // find where the thread is actually executing.
            {
              InstallJitIpSampler();
              auto* xe_thread = thread->thread();
              if (xe_thread) {
                auto* native = xe_thread->native_handle();
                if (native) {
                  pthread_t pt = reinterpret_cast<pthread_t>(native);
                  g_sampled_rip.store(0, std::memory_order_release);
                  pthread_kill(pt, SIGUSR2);
                  // Brief spin to let the signal handler fire
                  for (int w = 0; w < 100 && g_sampled_rip.load(std::memory_order_acquire) == 0; w++) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                  }
                  uintptr_t rip = g_sampled_rip.load(std::memory_order_acquire);
                  if (rip != 0) {
                    fprintf(stderr, "    JIT IP sample: host RIP=0x%lX", rip);
                    // Map host JIT address to guest PPC address
                    auto* backend = processor->backend();
                    auto* code_cache = backend ? backend->code_cache() : nullptr;
                    if (code_cache) {
                      auto* jit_fn = code_cache->LookupFunction(static_cast<uint64_t>(rip));
                      if (jit_fn) {
                        uint32_t guest_pc = jit_fn->MapMachineCodeToGuestAddress(rip);
                        fprintf(stderr, " → guest 0x%08X [%s @ 0x%08X]",
                                guest_pc, jit_fn->name().c_str(), jit_fn->address());
                      } else {
                        // Check if it's in the code cache range at all
                        if (rip >= 0xA0000000 && rip < 0xB0000000) {
                          fprintf(stderr, " (in JIT range but no function found)");
                        } else {
                          fprintf(stderr, " (NOT in JIT code cache — host/runtime code)");
                        }
                      }
                    }
                    fprintf(stderr, "\n");
                    fflush(stderr);
                  }
                }
              }
            }
#endif  // __linux__
            // Dump IAT and thunk code for RtlEnterCriticalSection
            auto* mem2 = emulator_->memory();
            if (mem2) {
              auto* iat_ptr = mem2->TranslateVirtual(0x823996E8);
              if (iat_ptr) {
                uint32_t iat_val = xe::load_and_swap<uint32_t>(iat_ptr);
                fprintf(stderr, "    IAT[RtlEnterCS @ 0x823996E8] = 0x%08X\n", iat_val);
              }
              // Dump thunk code at 0x83A00964 (should be sc 2; blr after patching)
              auto* thunk_ptr = mem2->TranslateVirtual(0x83A00964);
              if (thunk_ptr) {
                fprintf(stderr, "    Thunk[0x83A00964]:");
                for (int ti = 0; ti < 4; ti++) {
                  uint32_t instr = xe::load_and_swap<uint32_t>(thunk_ptr + ti * 4);
                  fprintf(stderr, " %08X", instr);
                }
                fprintf(stderr, " (expect: 44000042 4E800020 60000000 60000000)\n");
              }
              fflush(stderr);
            }
            // Check import thunk function state via processor
            auto* fn_at_thunk = processor->QueryFunction(0x83A00964);
            if (fn_at_thunk) {
              auto* guest_fn = static_cast<cpu::GuestFunction*>(fn_at_thunk);
              fprintf(stderr, "    Thunk fn: status=%d behavior=%d has_handler=%d machine_code=%p\n",
                      (int)guest_fn->status(), (int)guest_fn->behavior(),
                      guest_fn->extern_handler() != nullptr,
                      guest_fn->machine_code());
            } else {
              fprintf(stderr, "    Thunk fn: NOT FOUND by QueryFunction\n");
            }
            // Read indirection table entry directly (host address = guest address for 0x8xxxxxxx)
            uint32_t* indir_entry = reinterpret_cast<uint32_t*>(
                static_cast<uintptr_t>(0x83A00964));
            fprintf(stderr, "    Indirection[0x83A00964] = 0x%08X\n", *indir_entry);
            // Kernel shim call counters
            fprintf(stderr, "    RtlEnterCS=%u RtlInitCS=%u RtlLeaveCS=%u\n",
                    kernel::xboxkrnl::GetRtlEnterCsCount(),
                    kernel::xboxkrnl::GetRtlInitCsCount(),
                    kernel::xboxkrnl::GetRtlLeaveCsCount());
            // Check page state at XEX base (0x82000000) for SIGSEGV diagnosis
            {
              auto* heap82 = mem2->LookupHeap(0x82000000);
              if (heap82) {
                uint32_t alloc_len = 0;
                uint32_t prot = 0;
                uint32_t state = 0;
                heap82->QuerySize(0x82000000, &alloc_len);
                fprintf(stderr, "    Heap[0x82000000]: type=%d alloc_len=0x%X\n",
                        (int)heap82->heap_type(), alloc_len);
              }
              // Try reading from host address (diagnostic thread context)
              auto* host82 = mem2->TranslateVirtual(0x82000000);
              if (host82) {
                fprintf(stderr, "    Host ptr for 0x82000000: %p\n", host82);
                // Read first 4 bytes (safe from diagnostic thread)
                uint32_t val = xe::load_and_swap<uint32_t>(host82);
                fprintf(stderr, "    Read 0x82000000: 0x%08X (%c%c)\n",
                        val, (char)(val >> 24), (char)((val >> 16) & 0xFF));
              }
            }
            // Dump guest code near current LR to identify hang site
            if (lr >= 0x82000000 && lr < 0x84000000) {
              uint32_t dump_start = (lr > 0x60) ? (lr - 0x60) : lr;
              fprintf(stderr, "    Guest code near LR=0x%08X:\n", lr);
              for (int ci = 0; ci < 40; ci++) {
                uint32_t addr = dump_start + ci * 4;
                auto* cip = mem2->TranslateVirtual(addr);
                if (!cip) break;
                uint32_t instr = xe::load_and_swap<uint32_t>(cip);
                fprintf(stderr, "      0x%08X: %08X%s\n",
                        addr, instr, (addr == lr) ? "  <-- LR" : "");
              }
            }
            // Dump linked list at XapiThreadNotifyRoutineList (from MAP)
            // The list sentinel is the list head address itself
            // Address regenerated from MAP: 2026-02-22 (Session 9)
            {
              uint32_t sentinel = 0x83B14C3C;
              auto* head_ptr = mem2->TranslateVirtual(sentinel);
              if (head_ptr) {
                uint32_t head_val = xe::load_and_swap<uint32_t>(head_ptr);
                fprintf(stderr, "    NotifyList[0x%08X] head=0x%08X %s\n",
                        sentinel, head_val,
                        (head_val == sentinel) ? "(EMPTY)" : "(NON-EMPTY)");
                // Walk the list (max 8 entries)
                if (head_val != sentinel && head_val != 0) {
                  uint32_t node_addr = head_val;
                  for (int ni = 0; ni < 8 && node_addr != sentinel && node_addr != 0; ni++) {
                    auto* node_ptr = mem2->TranslateVirtual(node_addr);
                    if (!node_ptr) break;
                    uint32_t next = xe::load_and_swap<uint32_t>(node_ptr);
                    uint32_t field4 = xe::load_and_swap<uint32_t>(node_ptr + 4);
                    uint32_t callback = xe::load_and_swap<uint32_t>(node_ptr + 8);
                    fprintf(stderr, "      node[%d] @0x%08X: next=0x%08X +4=0x%08X callback=0x%08X\n",
                            ni, node_addr, next, field4, callback);
                    node_addr = next;
                  }
                }
              }
            }
            fflush(stderr);
          }
          // Walk PPC stack frames - wide range for all guest stacks
          auto* mem = emulator_->memory();
          if (sp >= 0x70000010 && sp < 0x78000000 && (sp & 3) == 0 && (sp & 0xFFFF) != 0) {
            for (int frame = 0; frame < 15; frame++) {
              if (sp < 0x70000010 || sp >= 0x78000000 || (sp & 3) != 0) break;
              auto* host_ptr = mem->TranslateVirtual(sp);
              if (!host_ptr) break;
              uint32_t back_chain = xe::load_and_swap<uint32_t>(host_ptr);
              uint32_t saved_lr = xe::load_and_swap<uint32_t>(host_ptr + 4);
              // Resolve saved LR to function name
              std::string fn_name = "";
              if (processor && saved_lr >= 0x82000000) {
                auto* fn = processor->QueryFunction(saved_lr);
                if (fn) fn_name = " [" + fn->name() + "]";
              }
              fprintf(stderr, "    [%d] sp=0x%08X back=0x%08X lr=0x%08X%s\n",
                      frame, sp, back_chain, saved_lr, fn_name.c_str());
              fflush(stderr);
              if (back_chain == 0 || back_chain == sp || back_chain < 0x70000000 || back_chain >= 0x78000000) break;
              sp = back_chain;
            }
          }
        }
      }
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

void EmulatorHeadless::StartEmulatorThread(std::filesystem::path launch_path) {
  emulator_thread_quit_requested_.store(false, std::memory_order_relaxed);
  emulator_thread_ = std::thread(&EmulatorHeadless::EmulatorThread, this, launch_path);
}

void EmulatorHeadless::SetupBootReporting() {
  boot_reporting_enabled_ = true;

  // Set up launch callback - reports when title is loaded
  // Delegate signature: uint32_t title_id, const std::string_view game_title
  emulator_->on_launch.AddListener([this](uint32_t title_id,
                                      const std::string_view game_title) {
    if (boot_reporting_enabled_) {
      std::cout << "BOOT: Title loaded successfully" << std::endl;
      std::cout << "BOOT: Title ID: 0x" << std::hex << title_id << std::dec
                << std::endl;
      if (!game_title.empty()) {
        std::cout << "BOOT: Title Name: " << game_title << std::endl;
      }

      // Report some additional info
      auto kernel_state = emulator_->kernel_state();
      if (kernel_state) {
        std::cout << "BOOT: Kernel state initialized" << std::endl;
      }
    }

    // Signal the emulator thread event (for synchronization if needed)
    if (emulator_thread_event_) {
      emulator_thread_event_->Set();
    }
  });

  // Set up terminate callback
  emulator_->on_terminate.AddListener([this]() {
    std::cout << "BOOT: Title terminated" << std::endl;
  });

  // Set up exit callback
  emulator_->on_exit.AddListener([this]() {
    std::cout << "BOOT: Emulator exit requested" << std::endl;
  });
}

void EmulatorHeadless::ReportCrash(Emulator* emulator, uint32_t pc,
                                   cpu::ThreadState* thread_state) {
  std::cout << "CRASH: PC = 0x" << std::hex << pc << std::endl;

  if (thread_state && thread_state->context()) {
    auto ctx = thread_state->context();
    std::cout << "CRASH: Registers:" << std::endl;
    for (int i = 0; i < 32; i++) {
      std::cout << "  r" << std::dec << i << " = 0x" << std::hex << ctx->r[i]
                << std::dec;
      if (i % 4 == 3) {
        std::cout << std::endl;
      } else {
        std::cout << "  ";
      }
    }

    std::cout << "  lr = 0x" << std::hex << ctx->lr << std::dec << std::endl;
    std::cout << "  ctr = 0x" << std::hex << ctx->ctr << std::dec << std::endl;
    std::cout << "  cr = 0x" << std::hex << ctx->cr() << std::dec << std::endl;
    std::cout << "  xer(ca,ov,so) = (0x" << std::hex
              << (uint32_t)ctx->xer_ca << "," << (uint32_t)ctx->xer_ov << ","
              << (uint32_t)ctx->xer_so << ")" << std::dec << std::endl;
  }

  auto kernel_state = emulator->kernel_state();
  if (kernel_state) {
    std::cout << "CRASH: Title ID: 0x" << std::hex << emulator->title_id()
              << std::dec << std::endl;
  }
}

}  // namespace app
}  // namespace xe
