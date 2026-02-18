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

#include "xenia/app/emulator_headless.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "third_party/fmt/include/fmt/format.h"

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
    if (elapsed - last_report_ms >= 500) {
      last_report_ms = elapsed;
      auto* kernel_state = emulator_->kernel_state();
      if (kernel_state) {
        auto threads = kernel_state->object_table()->GetObjectsByType<kernel::XThread>(
            kernel::XObject::Type::Thread);
        // Also enumerate via threads_by_id
        auto all_objects = kernel_state->object_table()->GetAllObjects();
        size_t thread_count = 0;
        for (auto& obj : all_objects) {
          if (obj->type() == kernel::XObject::Type::Thread) thread_count++;
        }
        fprintf(stderr, "=== Thread Status Report (%ldms) === %zu threads (object_table total=%zu, thread_objs=%zu)\n",
                elapsed, threads.size(), all_objects.size(), thread_count);
        // Look up threads by ID (game threads 5, 6 are missing from object table)
        for (uint32_t tid = 5; tid <= 8; tid++) {
          auto t = kernel_state->GetThreadByID(tid);
          if (t) {
            auto* ctx = t->thread_state() ? t->thread_state()->context() : nullptr;
            fprintf(stderr, "  [by_id] Thread %d: SP=0x%08X LR=0x%08X\n",
                    tid, ctx ? (uint32_t)ctx->r[1] : 0, ctx ? (uint32_t)ctx->lr : 0);
          }
        }
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
          fprintf(stderr, "  Thread %d: LR=0x%08X SP=0x%08X\n", thread->thread_id(), lr, sp);
          fflush(stderr);
          // Walk PPC stack frames
          auto* mem = emulator_->memory();
          // Skip stack walk for threads at stack top (idle) or outside valid range
          // Stack tops are at 0x10000-aligned boundaries; skip if SP is exactly at one
          if (sp >= 0x70000010 && sp < 0x701F0000 && (sp & 3) == 0 && (sp & 0xFFFF) != 0) {
            for (int frame = 0; frame < 10; frame++) {
              if (sp < 0x70000010 || sp >= 0x701F0000 || (sp & 3) != 0) break;
              auto* host_ptr = mem->TranslateVirtual(sp);
              if (!host_ptr) break;
              uint32_t back_chain = xe::load_and_swap<uint32_t>(host_ptr);
              uint32_t saved_lr = xe::load_and_swap<uint32_t>(host_ptr + 4);
              fprintf(stderr, "    [%d] sp=0x%08X back=0x%08X lr=0x%08X\n", frame, sp, back_chain, saved_lr);
              fflush(stderr);
              if (back_chain == 0 || back_chain == sp || back_chain < 0x70000000 || back_chain >= 0x70200000) break;
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
