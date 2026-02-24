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
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "xenia/emulator.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

class Dc3GdbRspHeadlessListener;

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

  // Report crash info to console
  static void ReportCrash(Emulator* emulator, uint32_t pc,
                          cpu::ThreadState* thread_state);

 private:
  void EmulatorThread(std::filesystem::path launch_path);

  Emulator* emulator_;

  std::atomic<bool> emulator_thread_quit_requested_;
  std::unique_ptr<xe::threading::Event> emulator_thread_event_;
  std::thread emulator_thread_;

  int exit_code_ = EXIT_SUCCESS;

  bool boot_reporting_enabled_ = false;
  bool module_reporting_enabled_ = false;
  std::unique_ptr<Dc3GdbRspHeadlessListener> dc3_gdb_rsp_listener_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_EMULATOR_HEADLESS_H_
