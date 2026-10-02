/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_EMULATOR_HEADLESS_H_
#define XENIA_APP_EMULATOR_HEADLESS_H_

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "xenia/emulator.h"
#include "xenia/xbox.h"

#include "xenia/debug/gdb_rsp/gdb_rsp_server.h"

namespace xe {
namespace app {

// Headless emulator wrapper - runs Xenia without any UI dependencies.
// Uses null/nop backends for GPU/APU/HID.
class EmulatorHeadless {
 public:
  explicit EmulatorHeadless(Emulator* emulator);
  ~EmulatorHeadless();

  // Factory function types for creating subsystems
  using AudioSystemFactory =
      std::function<std::unique_ptr<apu::AudioSystem>(cpu::Processor*)>;
  using GraphicsSystemFactory = std::function<std::unique_ptr<gpu::GraphicsSystem>()>;
  using InputDriverFactory = std::function<std::vector<std::unique_ptr<hid::InputDriver>>(
      ui::Window*)>;

  // Initialize the emulator with the given factory functions
  bool Initialize(AudioSystemFactory audio_factory,
                 GraphicsSystemFactory graphics_factory,
                 InputDriverFactory input_factory);

  // Start the emulator thread with the given path to launch
  void StartEmulatorThread(std::filesystem::path launch_path);

  // Run emulator until exit (call after StartEmulatorThread)
  void Run();

  // Run emulator with a timeout (returns when timeout expires or title exits)
  void RunWithTimeout(int32_t timeout_ms);

  // Get the exit code from the last run
  int exit_code() const { return exit_code_; }

  // Set up callbacks for boot status reporting to console
  void SetupBootReporting();

 private:
  void EmulatorThread(std::filesystem::path launch_path);
  void ReportFaultLivelock();
  void ReportThreadStatus(int64_t elapsed_ms);
  void WalkGuestStack(uint32_t thread_id, uint32_t sp, uint32_t lr);

  Emulator* emulator_;

  std::atomic<bool> emulator_thread_quit_requested_;
  std::unique_ptr<xe::threading::Event> emulator_thread_event_;
  std::thread emulator_thread_;

  // EXIT_FAILURE when the title could not be launched.
  std::atomic<int> exit_code_{EXIT_SUCCESS};

  // The GDB RSP server, when --gdb_rsp_stub is set.
  debug::gdb_rsp::GdbRspServerPtr gdb_rsp_server_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_EMULATOR_HEADLESS_H_
