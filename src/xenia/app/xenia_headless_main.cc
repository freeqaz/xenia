/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "xenia/app/emulator_headless.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/vfs/devices/host_path_device.h"

// Headless backends
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/null/null_graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/hid/nop/nop_hid.h"
#include "xenia/hid/nop/nop_input_driver.h"

// Available CPU backend
#if XE_PLATFORM_WIN32 || XE_PLATFORM_LINUX || XE_PLATFORM_MACOS
#include "xenia/cpu/backend/x64/x64_backend.h"
#endif

DEFINE_path(
    storage_root, "",
    "Root path for persistent internal data storage (config, etc.), or empty "
    "to use the path preferred for the OS, such as the documents folder, or "
    "the emulator executable directory if portable.txt is present in it.",
    "Storage");
DEFINE_path(
    content_root, "",
    "Root path for guest content storage (saves, etc.), or empty to use the "
    "content folder under the storage root.",
    "Storage");
DEFINE_path(
    cache_root, "",
    "Root path for files used to speed up certain parts of the emulator or the "
    "game. These files may be persistent, but they can be deleted without "
    "major side effects such as progress loss. If empty, the cache folder "
    "under the storage root, or, if available, the cache directory preferred "
    "for the OS, will be used.",
    "Storage");
DEFINE_transient_path(target, "",
                  "Specifies the target .xex or .iso to execute.",
                  "General");
DEFINE_transient_bool(portable, false,
                    "Specifies if Xenia should run in portable mode.",
                    "General");
DEFINE_bool(mount_scratch, false, "Enable scratch mount", "Storage");
DEFINE_bool(mount_cache, false, "Enable cache mount", "Storage");
DEFINE_string(gpu, "null",
              "Graphics system. Use: [null, vulkan]", "GPU");
DEFINE_bool(headless_report_boot, true,
            "Report boot status to console", "Headless");
DEFINE_int32(headless_timeout_ms, 0,
            "Timeout in milliseconds before terminating (0 = run indefinitely)",
            "Headless");
DEFINE_string(scripted_input, "",
              "Scripted controller input. Format: '5s:A,7s:START,10s:A'. "
              "Simulates a connected controller with timed button presses.",
              "HID");

namespace xe {
namespace app {

static std::unique_ptr<apu::AudioSystem> CreateHeadlessAudioSystem(
    cpu::Processor* processor) {
  return apu::nop::NopAudioSystem::Create(processor);
}

static std::unique_ptr<gpu::GraphicsSystem> CreateHeadlessGraphicsSystem() {
  if (cvars::gpu == "vulkan") {
    XELOGI("GPU backend: Vulkan");
    return std::make_unique<gpu::vulkan::VulkanGraphicsSystem>();
  }
  XELOGI("GPU backend: null");
  return std::make_unique<gpu::null::NullGraphicsSystem>();
}

static std::vector<std::unique_ptr<hid::InputDriver>> CreateHeadlessInputDrivers(
    ui::Window* window) {
  std::vector<std::unique_ptr<hid::InputDriver>> drivers;
  auto driver = std::make_unique<hid::nop::NopInputDriver>(window, 0);
  if (!cvars::scripted_input.empty()) {
    driver->SetScriptedInput(cvars::scripted_input);
  }
  drivers.emplace_back(std::move(driver));
  return drivers;
}

static int HeadlessMain(const std::vector<std::string>& args) {
  XELOGI("Xenia Headless Mode - Starting");

  // Figure out where internal files and content should go.
  std::filesystem::path storage_root = cvars::storage_root;
  if (storage_root.empty()) {
    storage_root = xe::filesystem::GetExecutableFolder();
    if (!cvars::portable &&
        !std::filesystem::exists(storage_root / "portable.txt")) {
      storage_root = xe::filesystem::GetUserFolder();
#if defined(XE_PLATFORM_WIN32) || defined(XE_PLATFORM_GNU_LINUX)
      storage_root = storage_root / "Xenia";
#else
      storage_root = storage_root / "Xenia";
#endif
    }
  }
  storage_root = std::filesystem::absolute(storage_root);
  XELOGI("Storage root: {}", xe::path_to_utf8(storage_root));

  config::SetupConfig(storage_root);

  std::filesystem::path content_root = cvars::content_root;
  if (content_root.empty()) {
    content_root = storage_root / "content";
  } else {
    if (!content_root.is_absolute()) {
      content_root = storage_root / content_root;
    }
  }
  content_root = std::filesystem::absolute(content_root);
  XELOGI("Content root: {}", xe::path_to_utf8(content_root));

  std::filesystem::path cache_root = cvars::cache_root;
  if (cache_root.empty()) {
    cache_root = storage_root / "cache";
  } else {
    if (!cache_root.is_absolute()) {
      cache_root = storage_root / cache_root;
    }
  }
  cache_root = std::filesystem::absolute(cache_root);
  XELOGI("Cache root: {}", xe::path_to_utf8(cache_root));

  // Create emulator instance
  auto emulator =
      std::make_unique<Emulator>("", storage_root, content_root, cache_root);

  // Create headless app wrapper
  auto app = std::make_unique<EmulatorHeadless>(emulator.get());

  if (!app->Initialize(CreateHeadlessAudioSystem, CreateHeadlessGraphicsSystem,
                      CreateHeadlessInputDrivers)) {
    XELOGE("Failed to initialize headless emulator");
    return EXIT_FAILURE;
  }

  // Setup optional mounts
  if (cvars::mount_scratch) {
    auto scratch_device = std::make_unique<xe::vfs::HostPathDevice>(
        "\\SCRATCH", "scratch", false);
    if (!scratch_device->Initialize()) {
      XELOGE("Unable to scan scratch path");
    } else {
      if (!emulator->file_system()->RegisterDevice(std::move(scratch_device))) {
        XELOGE("Unable to register scratch path");
      } else {
        emulator->file_system()->RegisterSymbolicLink("scratch:", "\\SCRATCH");
      }
    }
  }

  if (cvars::mount_cache) {
    auto cache0_device =
        std::make_unique<xe::vfs::HostPathDevice>("\\CACHE0", "cache0", false);
    if (!cache0_device->Initialize()) {
      XELOGE("Unable to scan cache0 path");
    } else {
      if (!emulator->file_system()->RegisterDevice(std::move(cache0_device))) {
        XELOGE("Unable to register cache0 path");
      } else {
        emulator->file_system()->RegisterSymbolicLink("cache0:", "\\CACHE0");
      }
    }

    auto cache_device =
        std::make_unique<xe::vfs::HostPathDevice>("\\CACHE", "cache", false);
    if (!cache_device->Initialize()) {
      XELOGE("Unable to scan cache path");
    } else {
      if (!emulator->file_system()->RegisterDevice(std::move(cache_device))) {
        XELOGE("Unable to register cache path");
      } else {
        emulator->file_system()->RegisterSymbolicLink("cache:", "\\CACHE");
      }
    }
  }

  // Set up callbacks for boot status reporting
  if (cvars::headless_report_boot) {
    app->SetupBootReporting();
  }

  // Launch target if specified
  std::filesystem::path path;
  if (!cvars::target.empty()) {
    path = cvars::target;
  }

  if (path.empty()) {
    XELOGE("No target specified. Use --target=<path> to specify an XEX or ISO.");
    return EXIT_FAILURE;
  }

  auto abs_path = std::filesystem::absolute(path);
  XELOGI("Launching: {}", xe::path_to_utf8(abs_path));

  // Create frame dump directory if specified
  if (!cvars::dump_frames_path.empty()) {
    std::filesystem::create_directories(cvars::dump_frames_path);
    XELOGI("Frame dump path: {}", cvars::dump_frames_path);
  }

  // Start the emulator thread (this will call LaunchPath internally)
  app->StartEmulatorThread(abs_path);

  // Run emulator until exit or timeout
  if (cvars::headless_timeout_ms > 0) {
    XELOGI("Running with {}ms timeout...", cvars::headless_timeout_ms);
    app->RunWithTimeout(cvars::headless_timeout_ms);
  } else {
    XELOGI("Running (no timeout - use Ctrl+C to stop)...");
    app->Run();
  }

  XELOGI("Xenia Headless Mode - Shutting down");
  return app->exit_code();
}

}  // namespace app
}  // namespace xe

int main(int argc, char** argv) {
  // Parse arguments first
  cvar::ParseLaunchArguments(argc, argv, "", {});

  // Initialize logging (needs parsed cvars)
  xe::InitializeLogging("xenia-headless");

  int result = xe::app::HeadlessMain({});

  xe::ShutdownLogging();

  return result;
}
