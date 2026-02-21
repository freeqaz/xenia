/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/emulator.h"

#include <algorithm>
#include <cinttypes>

#include "config.h"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/apu/audio_system.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/backend/null_backend.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/gameinfo_utils.h"
#include "xenia/kernel/util/xdbf_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xbdm/xbdm_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/memory.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/devices/stfs_container_device.h"
#include "xenia/vfs/virtual_file_system.h"

#ifndef XE_HEADLESS_BUILD
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#endif

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#endif  // XE_ARCH

DECLARE_int32(user_language);

DEFINE_double(time_scalar, 1.0,
              "Scalar used to speed or slow time (1x, 2x, 1/2x, etc).",
              "General");
DEFINE_string(
    launch_module, "",
    "Executable to launch from the .iso or the package instead of default.xex "
    "or the module specified by the game. Leave blank to launch the default "
    "module.",
    "General");

namespace xe {

using namespace xe::literals;

Emulator::GameConfigLoadCallback::GameConfigLoadCallback(Emulator& emulator)
    : emulator_(emulator) {
  emulator_.AddGameConfigLoadCallback(this);
}

Emulator::GameConfigLoadCallback::~GameConfigLoadCallback() {
  emulator_.RemoveGameConfigLoadCallback(this);
}

Emulator::Emulator(const std::filesystem::path& command_line,
                   const std::filesystem::path& storage_root,
                   const std::filesystem::path& content_root,
                   const std::filesystem::path& cache_root)
    : on_launch(),
      on_terminate(),
      on_exit(),
      command_line_(command_line),
      storage_root_(storage_root),
      content_root_(content_root),
      cache_root_(cache_root),
      title_name_(),
      title_version_(),
      display_window_(nullptr),
      memory_(),
      audio_system_(),
      graphics_system_(),
      input_system_(),
      export_resolver_(),
      file_system_(),
      kernel_state_(),
      main_thread_(),
      title_id_(std::nullopt),
      paused_(false),
      restoring_(false),
      restore_fence_() {}

Emulator::~Emulator() {
  // Note that we delete things in the reverse order they were initialized.

  // Give the systems time to shutdown before we delete them.
  if (graphics_system_) {
    graphics_system_->Shutdown();
  }
  if (audio_system_) {
    audio_system_->Shutdown();
  }

  input_system_.reset();
  graphics_system_.reset();
  audio_system_.reset();

  kernel_state_.reset();
  file_system_.reset();

  processor_.reset();

  export_resolver_.reset();

  ExceptionHandler::Uninstall(Emulator::ExceptionCallbackThunk, this);
}

X_STATUS Emulator::Setup(
    ui::Window* display_window, ui::ImGuiDrawer* imgui_drawer,
    bool require_cpu_backend,
    std::function<std::unique_ptr<apu::AudioSystem>(cpu::Processor*)>
        audio_system_factory,
    std::function<std::unique_ptr<gpu::GraphicsSystem>()>
        graphics_system_factory,
    std::function<std::vector<std::unique_ptr<hid::InputDriver>>(ui::Window*)>
        input_driver_factory) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  display_window_ = display_window;
  imgui_drawer_ = imgui_drawer;

  // Initialize clock.
  // 360 uses a 50MHz clock.
  Clock::set_guest_tick_frequency(50000000);
  // We could reset this with save state data/constant value to help replays.
  Clock::set_guest_system_time_base(Clock::QueryHostSystemTime());
  // This can be adjusted dynamically, as well.
  Clock::set_guest_time_scalar(cvars::time_scalar);

  // Before we can set thread affinity we must enable the process to use all
  // logical processors.
  xe::threading::EnableAffinityConfiguration();

  // Create memory system first, as it is required for other systems.
  memory_ = std::make_unique<Memory>();
  if (!memory_->Initialize()) {
    return false;
  }

  // Shared export resolver used to attach and query for HLE exports.
  export_resolver_ = std::make_unique<xe::cpu::ExportResolver>();

  std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
  if (cvars::cpu == "x64") {
    backend.reset(new xe::cpu::backend::x64::X64Backend());
  }
#endif  // XE_ARCH
  if (cvars::cpu == "any") {
    if (!backend) {
#if XE_ARCH_AMD64
      backend.reset(new xe::cpu::backend::x64::X64Backend());
#endif  // XE_ARCH
    }
  }
  if (!backend && !require_cpu_backend) {
    backend.reset(new xe::cpu::backend::NullBackend());
  }

  // Initialize the CPU.
  processor_ = std::make_unique<xe::cpu::Processor>(memory_.get(),
                                                    export_resolver_.get());
  if (!processor_->Setup(std::move(backend))) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Initialize the APU.
  if (audio_system_factory) {
    audio_system_ = audio_system_factory(processor_.get());
    if (!audio_system_) {
      return X_STATUS_NOT_IMPLEMENTED;
    }
  }

  // Initialize the GPU.
  graphics_system_ = graphics_system_factory();
  if (!graphics_system_) {
    return X_STATUS_NOT_IMPLEMENTED;
  }

  // Initialize the HID.
  input_system_ = std::make_unique<xe::hid::InputSystem>(display_window_);
  if (!input_system_) {
    return X_STATUS_NOT_IMPLEMENTED;
  }
  if (input_driver_factory) {
    auto input_drivers = input_driver_factory(display_window_);
    for (size_t i = 0; i < input_drivers.size(); ++i) {
      auto& input_driver = input_drivers[i];
      input_driver->set_is_active_callback(
          []() -> bool { return !xe::kernel::xam::xeXamIsUIActive(); });
      input_system_->AddDriver(std::move(input_driver));
    }
  }

  result = input_system_->Setup();
  if (result) {
    return result;
  }

  // Bring up the virtual filesystem used by the kernel.
  file_system_ = std::make_unique<xe::vfs::VirtualFileSystem>();

  // Shared kernel state.
  kernel_state_ = std::make_unique<xe::kernel::KernelState>(this);

  // Setup the core components.
  result = graphics_system_->Setup(
      processor_.get(), kernel_state_.get(),
      display_window_ ? &display_window_->app_context() : nullptr,
      display_window_ != nullptr);
  if (result) {
    return result;
  }

  if (audio_system_) {
    result = audio_system_->Setup(kernel_state_.get());
    if (result) {
      return result;
    }
  }

#define LOAD_KERNEL_MODULE(t) \
  static_cast<void>(kernel_state_->LoadKernelModule<kernel::t>())
  // HLE kernel modules.
  LOAD_KERNEL_MODULE(xboxkrnl::XboxkrnlModule);
  LOAD_KERNEL_MODULE(xam::XamModule);
  LOAD_KERNEL_MODULE(xbdm::XbdmModule);
#undef LOAD_KERNEL_MODULE

  // Initialize emulator fallback exception handling last.
  ExceptionHandler::Install(Emulator::ExceptionCallbackThunk, this);

  return result;
}

X_STATUS Emulator::TerminateTitle() {
  if (!is_title_open()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  kernel_state_->TerminateTitle();
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  on_terminate();
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::LaunchPath(const std::filesystem::path& path) {
  // Launch based on file type.
  // This is a silly guess based on file extension.
  if (!path.has_extension()) {
    // Likely an STFS container.
    return LaunchStfsContainer(path);
  };
  auto extension = xe::utf8::lower_ascii(xe::path_to_utf8(path.extension()));
  if (extension == ".xex" || extension == ".elf" || extension == ".exe") {
    // Treat as a naked xex file.
    return LaunchXexFile(path);
  } else {
    // Assume a disc image.
    return LaunchDiscImage(path);
  }
}

X_STATUS Emulator::LaunchXexFile(const std::filesystem::path& path) {
  // We create a virtual filesystem pointing to its directory and symlink
  // that to the game filesystem.
  // e.g., /my/files/foo.xex will get a local fs at:
  // \\Device\\Harddisk0\\Partition1
  // and then get that symlinked to game:\, so
  // -> game:\foo.xex

  auto mount_path = "\\Device\\Harddisk0\\Partition1";

  // Register the local directory in the virtual filesystem.
  auto parent_path = path.parent_path();
  auto device =
      std::make_unique<vfs::HostPathDevice>(mount_path, parent_path, true);
  if (!device->Initialize()) {
    XELOGE("Unable to scan host path");
    return X_STATUS_NO_SUCH_FILE;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    XELOGE("Unable to register host path");
    return X_STATUS_NO_SUCH_FILE;
  }

  // Create symlinks to the device.
  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);

  // Get just the filename (foo.xex).
  auto file_name = path.filename();

  // Launch the game.
  auto fs_path = "game:\\" + xe::path_to_utf8(file_name);
  return CompleteLaunch(path, fs_path);
}

X_STATUS Emulator::LaunchDiscImage(const std::filesystem::path& path) {
  auto mount_path = "\\Device\\Cdrom0";

  // Register the disc image in the virtual filesystem.
  auto device = std::make_unique<vfs::DiscImageDevice>(mount_path, path);
  if (!device->Initialize()) {
    xe::FatalError("Unable to mount disc image; file not found or corrupt.");
    return X_STATUS_NO_SUCH_FILE;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    xe::FatalError("Unable to register disc image.");
    return X_STATUS_NO_SUCH_FILE;
  }

  // Create symlinks to the device.
  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);

  // Launch the game.
  auto module_path(FindLaunchModule());
  return CompleteLaunch(path, module_path);
}

X_STATUS Emulator::LaunchStfsContainer(const std::filesystem::path& path) {
  auto mount_path = "\\Device\\Cdrom0";

  // Register the container in the virtual filesystem.
  auto device = std::make_unique<vfs::StfsContainerDevice>(mount_path, path);
  if (!device->Initialize()) {
    xe::FatalError(
        "Unable to mount STFS container; file not found or corrupt.");
    return X_STATUS_NO_SUCH_FILE;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    xe::FatalError("Unable to register STFS container.");
    return X_STATUS_NO_SUCH_FILE;
  }

  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);

  // Launch the game.
  auto module_path(FindLaunchModule());
  return CompleteLaunch(path, module_path);
}

void Emulator::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Don't hold the lock on this (so any waits follow through)
  graphics_system_->Pause();
  audio_system_->Pause();

  auto lock = global_critical_region::AcquireDirect();
  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  auto current_thread = kernel::XThread::IsInThread()
                            ? kernel::XThread::GetCurrentThread()
                            : nullptr;
  for (auto thread : threads) {
    // Don't pause ourself or host threads.
    if (thread == current_thread || !thread->can_debugger_suspend()) {
      continue;
    }

    if (thread->is_running()) {
      thread->thread()->Suspend(nullptr);
    }
  }

  XELOGD("! EMULATOR PAUSED !");
}

void Emulator::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;
  XELOGD("! EMULATOR RESUMED !");

  graphics_system_->Resume();
  audio_system_->Resume();

  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  for (auto thread : threads) {
    if (!thread->can_debugger_suspend()) {
      // Don't pause host threads.
      continue;
    }

    if (thread->is_running()) {
      thread->thread()->Resume(nullptr);
    }
  }
}

bool Emulator::SaveToFile(const std::filesystem::path& path) {
  Pause();

  filesystem::CreateEmptyFile(path);
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite, 0, 2_GiB);
  if (!map) {
    return false;
  }

  // Save the emulator state to a file
  ByteStream stream(map->data(), map->size());
  stream.Write(kEmulatorSaveSignature);
  stream.Write(title_id_.has_value());
  if (title_id_.has_value()) {
    stream.Write(title_id_.value());
  }

  // It's important we don't hold the global lock here! XThreads need to step
  // forward (possibly through guarded regions) without worry!
  processor_->Save(&stream);
  graphics_system_->Save(&stream);
  audio_system_->Save(&stream);
  kernel_state_->Save(&stream);
  memory_->Save(&stream);
  map->Close(stream.offset());

  Resume();
  return true;
}

bool Emulator::RestoreFromFile(const std::filesystem::path& path) {
  // Restore the emulator state from a file
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite);
  if (!map) {
    return false;
  }

  restoring_ = true;

  // Terminate any loaded titles.
  Pause();
  kernel_state_->TerminateTitle();

  auto lock = global_critical_region::AcquireDirect();
  ByteStream stream(map->data(), map->size());
  if (stream.Read<uint32_t>() != kEmulatorSaveSignature) {
    return false;
  }

  auto has_title_id = stream.Read<bool>();
  std::optional<uint32_t> title_id;
  if (!has_title_id) {
    title_id = {};
  } else {
    title_id = stream.Read<uint32_t>();
  }
  if (title_id_.has_value() != title_id.has_value() ||
      title_id_.value() != title_id.value()) {
    // Swapping between titles is unsupported at the moment.
    assert_always();
    return false;
  }

  if (!processor_->Restore(&stream)) {
    XELOGE("Could not restore processor!");
    return false;
  }
  if (!graphics_system_->Restore(&stream)) {
    XELOGE("Could not restore graphics system!");
    return false;
  }
  if (!audio_system_->Restore(&stream)) {
    XELOGE("Could not restore audio system!");
    return false;
  }
  if (!kernel_state_->Restore(&stream)) {
    XELOGE("Could not restore kernel state!");
    return false;
  }
  if (!memory_->Restore(&stream)) {
    XELOGE("Could not restore memory!");
    return false;
  }

  // Update the main thread.
  auto threads =
      kernel_state_->object_table()->GetObjectsByType<kernel::XThread>();
  for (auto thread : threads) {
    if (thread->main_thread()) {
      main_thread_ = thread;
      break;
    }
  }

  Resume();

  restore_fence_.Signal();
  restoring_ = false;

  return true;
}

bool Emulator::TitleRequested() {
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");
  return xam->loader_data().launch_data_present;
}

void Emulator::LaunchNextTitle() {
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");
  auto next_title = xam->loader_data().launch_path;

  CompleteLaunch("", next_title);
}

bool Emulator::ExceptionCallbackThunk(Exception* ex, void* data) {
  return reinterpret_cast<Emulator*>(data)->ExceptionCallback(ex);
}

bool Emulator::ExceptionCallback(Exception* ex) {
  // Check to see if the exception occurred in guest code.
  auto code_cache = processor()->backend()->code_cache();
  auto code_base = code_cache->execute_base_address();
  auto code_end = code_base + code_cache->total_size();

  if (!processor()->is_debugger_attached() && debugging::IsDebuggerAttached()) {
    // If Xenia's debugger isn't attached but another one is, pass it to that
    // debugger.
    return false;
  } else if (processor()->is_debugger_attached()) {
    // Let the debugger handle this exception. It may decide to continue past it
    // (if it was a stepping breakpoint, etc).
    return processor()->OnUnhandledException(ex);
  }

  if (!(ex->pc() >= code_base && ex->pc() < code_end)) {
    // Didn't occur in guest code. Let it pass.
    return false;
  }

  // Within range. Pause the emulator and eat the exception.
  Pause();

  // Dump information into the log.
  auto current_thread = kernel::XThread::GetCurrentThread();
  assert_not_null(current_thread);

  auto guest_function = code_cache->LookupFunction(ex->pc());
  assert_not_null(guest_function);

  auto context = current_thread->thread_state()->context();

  XELOGE("==== CRASH DUMP ====");
  XELOGE("Thread ID (Host: 0x{:08X} / Guest: 0x{:08X})",
         current_thread->thread()->system_id(), current_thread->thread_id());
  XELOGE("Thread Handle: 0x{:08X}", current_thread->handle());
  XELOGE("PC: 0x{:08X}",
         guest_function->MapMachineCodeToGuestAddress(ex->pc()));
  XELOGE("Registers:");
  for (int i = 0; i < 32; i++) {
    XELOGE(" r{:<3} = {:016X}", i, context->r[i]);
  }
  for (int i = 0; i < 32; i++) {
    XELOGE(" f{:<3} = {:016X} = (double){} = (float){}", i,
           *reinterpret_cast<uint64_t*>(&context->f[i]), context->f[i],
           *(float*)&context->f[i]);
  }
  for (int i = 0; i < 128; i++) {
    XELOGE(" v{:<3} = [0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}]", i,
           context->v[i].u32[0], context->v[i].u32[1], context->v[i].u32[2],
           context->v[i].u32[3]);
  }

  // Display a dialog telling the user the guest has crashed.
#ifndef XE_HEADLESS_BUILD
  if (display_window_ && imgui_drawer_) {
    display_window_->app_context().CallInUIThreadSynchronous([this]() {
      xe::ui::ImGuiDialog::ShowMessageBox(
          imgui_drawer_, "Uh-oh!",
          "The guest has crashed.\n\n"
          ""
          "Xenia has now paused itself.\n"
          "A crash dump has been written into the log.");
    });
  }
#else
  XELOGE("Guest crashed! PC: 0x{:08X}",
         guest_function->MapMachineCodeToGuestAddress(ex->pc()));
#endif

  // Now suspend ourself (we should be a guest thread).
  current_thread->Suspend(nullptr);

  // We should not arrive here!
  assert_always();
  return false;
}

void Emulator::WaitUntilExit() {
  while (true) {
    if (main_thread_) {
      xe::threading::Wait(main_thread_->thread(), false);
    }

    if (restoring_) {
      restore_fence_.Wait();
    } else {
      // Not restoring and the thread exited. We're finished.
      break;
    }
  }

  on_exit();
}

void Emulator::AddGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  // Check if already added.
  if (std::find(game_config_load_callbacks_.cbegin(),
                game_config_load_callbacks_.cend(),
                callback) != game_config_load_callbacks_.cend()) {
    return;
  }
  game_config_load_callbacks_.push_back(callback);
}

void Emulator::RemoveGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  auto it = std::find(game_config_load_callbacks_.cbegin(),
                      game_config_load_callbacks_.cend(), callback);
  if (it == game_config_load_callbacks_.cend()) {
    return;
  }
  if (game_config_load_callback_loop_next_index_ != SIZE_MAX) {
    // Actualize the next callback index after the erasure from the vector.
    size_t existing_index =
        size_t(std::distance(game_config_load_callbacks_.cbegin(), it));
    if (game_config_load_callback_loop_next_index_ > existing_index) {
      --game_config_load_callback_loop_next_index_;
    }
  }
  game_config_load_callbacks_.erase(it);
}

std::string Emulator::FindLaunchModule() {
  std::string path("game:\\");

  if (!cvars::launch_module.empty()) {
    return path + cvars::launch_module;
  }

  std::string default_module("default.xex");

  auto gameinfo_entry(file_system_->ResolvePath(path + "GameInfo.bin"));
  if (gameinfo_entry) {
    vfs::File* file = nullptr;
    X_STATUS result =
        gameinfo_entry->Open(vfs::FileAccess::kGenericRead, &file);
    if (XSUCCEEDED(result)) {
      std::vector<uint8_t> buffer(gameinfo_entry->size());
      size_t bytes_read = 0;
      result = file->ReadSync(buffer.data(), buffer.size(), 0, &bytes_read);
      if (XSUCCEEDED(result)) {
        kernel::util::GameInfo info(buffer);
        if (info.is_valid()) {
          XELOGI("Found virtual title {}", info.virtual_title_id());

          const std::string xna_id("584E07D1");
          auto xna_id_entry(file_system_->ResolvePath(path + xna_id));
          if (xna_id_entry) {
            default_module = xna_id + "\\" + info.module_name();
          } else {
            XELOGE("Could not find fixed XNA path {}", xna_id);
          }
        }
      }
    }
  }

  return path + default_module;
}

static std::string format_version(xex2_version version) {
  // fmt::format doesn't like bit fields
  uint32_t major, minor, build, qfe;
  major = version.major;
  minor = version.minor;
  build = version.build;
  qfe = version.qfe;
  if (qfe) {
    return fmt::format("{}.{}.{}.{}", major, minor, build, qfe);
  }
  if (build) {
    return fmt::format("{}.{}.{}", major, minor, build);
  }
  return fmt::format("{}.{}", major, minor);
}

X_STATUS Emulator::CompleteLaunch(const std::filesystem::path& path,
                                  const std::string_view module_path) {
#ifndef XE_HEADLESS_BUILD
  // Making changes to the UI (setting the icon) and executing game config load
  // callbacks which expect to be called from the UI thread.
  assert_true(display_window_->app_context().IsInUIThread());
#else
  // Headless mode: no display window
  assert_true(display_window_ == nullptr);
#endif

  // Setup NullDevices for raw HDD partition accesses
  // Cache/STFC code baked into games tries reading/writing to these
  // By using a NullDevice that just returns success to all IO requests it
  // should allow games to believe cache/raw disk was accessed successfully

  // NOTE: this should probably be moved to xenia_main.cc, but right now we need
  // to register the \Device\Harddisk0\ NullDevice _after_ the
  // \Device\Harddisk0\Partition1 HostPathDevice, otherwise requests to
  // Partition1 will go to this. Registering during CompleteLaunch allows us to
  // make sure any HostPathDevices are ready beforehand.
  // (see comment above cache:\ device registration for more info about why)
  auto null_paths = {std::string("\\Partition0"), std::string("\\Cache0"),
                     std::string("\\Cache1")};
  auto null_device =
      std::make_unique<vfs::NullDevice>("\\Device\\Harddisk0", null_paths);
  if (null_device->Initialize()) {
    file_system_->RegisterDevice(std::move(null_device));
  }

  // Reset state.
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
#ifndef XE_HEADLESS_BUILD
  display_window_->SetIcon(nullptr, 0);
#endif

  // Allow xam to request module loads.
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");

  XELOGI("Launching module {}", module_path);
  auto module = kernel_state_->LoadUserModule(module_path);
  if (!module) {
    XELOGE("Failed to load user module {}", xe::path_to_utf8(path));
    return X_STATUS_NOT_FOUND;
  }

  // Grab the current title ID.
  xex2_opt_execution_info* info = nullptr;
  module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &info);

  if (!info) {
    title_id_ = 0;
  } else {
    title_id_ = info->title_id;
    auto title_version = info->version();
    if (title_version.value != 0) {
      title_version_ = format_version(title_version);
    }
  }

  // Try and load the resource database (xex only).
  if (module->title_id()) {
    auto title_id = fmt::format("{:08X}", module->title_id());

    // Load the per-game configuration file and make sure updates are handled by
    // the callbacks.
    config::LoadGameConfig(title_id);
    assert_true(game_config_load_callback_loop_next_index_ == SIZE_MAX);
    game_config_load_callback_loop_next_index_ = 0;
    while (game_config_load_callback_loop_next_index_ <
           game_config_load_callbacks_.size()) {
      game_config_load_callbacks_[game_config_load_callback_loop_next_index_++]
          ->PostGameConfigLoad();
    }
    game_config_load_callback_loop_next_index_ = SIZE_MAX;

    const kernel::util::XdbfGameData db = kernel_state_->module_xdbf(module);
    if (db.is_valid()) {
      XLanguage language =
          db.GetExistingLanguage(static_cast<XLanguage>(cvars::user_language));
      title_name_ = db.title(language);

      XELOGI("-------------------- ACHIEVEMENTS --------------------");
      const std::vector<kernel::util::XdbfAchievementTableEntry>
          achievement_list = db.GetAchievements();
      for (const kernel::util::XdbfAchievementTableEntry& entry :
           achievement_list) {
        std::string label = db.GetStringTableEntry(language, entry.label_id);
        std::string desc =
            db.GetStringTableEntry(language, entry.description_id);

        XELOGI("{} - {} - {} - {}", entry.id, label, desc, entry.gamerscore);
      }
      XELOGI("----------------- END OF ACHIEVEMENTS ----------------");

      auto icon_block = db.icon();
      if (icon_block) {
#ifndef XE_HEADLESS_BUILD
        display_window_->SetIcon(icon_block.buffer, icon_block.size);
#endif
      }
    }
  }

  // Initializing the shader storage in a blocking way so the user doesn't miss
  // the initial seconds - for instance, sound from an intro video may start
  // playing before the video can be seen if doing this in parallel with the
  // main thread.
  on_shader_storage_initialization(true);
  graphics_system_->InitializeShaderStorage(cache_root_, title_id_.value(),
                                            true);
  on_shader_storage_initialization(false);

  // DC3 title-specific guest code patches.
  // DC3 Title ID: 0x373307D9 (Dance Central 3)
  if (title_id_.has_value() && title_id_.value() == 0x373307D9) {
    // Clean stale content cache from previous runs.
    // DC3's SaveLoadManager reads cached song/global options data on boot.
    // If a previous xenia run wrote partial/corrupt data, the game will try
    // to deserialize it and crash with a garbage allocation size.
    // Deleting the content directory forces a fresh start.
    auto dc3_content = content_root_ / "373307D9";
    if (std::filesystem::exists(dc3_content)) {
      XELOGI("DC3: Cleaning stale content cache at {}",
             xe::path_to_utf8(dc3_content));
      std::error_code ec;
      std::filesystem::remove_all(dc3_content, ec);
      if (ec) {
        XELOGI("DC3: Failed to clean content cache: {}", ec.message());
      }
    }
  }

  //
  // The DC3 debug build statically links the Xbox 360 Kinect SDK (NUI).
  // NuiInitialize and related functions are PPC code embedded in the XEX,
  // NOT kernel imports. They fail because Kinect hardware doesn't exist
  // in xenia. The debug build's MILO_ASSERT halts on these failures.
  //
  // We patch the NUI functions in guest memory with PPC stubs before the
  // JIT compiles them. Each stub is 2 instructions (8 bytes):
  //   li r3, 0    (0x38600000) - return S_OK
  //   blr         (0x4E800020) - return
  //
  // This is the standard emulator approach for HLE of statically-linked
  // SDK functions (similar to Dolphin's OS HLE patches).
  if (title_id_.has_value() && title_id_.value() == 0x373307D9 &&
      cvars::stub_nui_functions) {
    XELOGI("DC3: Stubbing NUI (Kinect SDK) functions in guest memory");

    // PPC instructions (big-endian)
    const uint32_t kLiR3_0 = 0x38600000;   // li r3, 0  (return S_OK / 0)
    const uint32_t kLiR3_Neg1 = 0x3860FFFF; // li r3, -1 (return E_UNEXPECTED)
    const uint32_t kBlr = 0x4E800020;       // blr

    // NUI function addresses from the DC3 debug XEX (ham_xbox_r.exe).
    // These are guest virtual addresses for the statically-linked NUI SDK.
    // The functions are PPC code from the Xbox 360 Kinect SDK libraries.
    //
    // Addresses sourced from: dc3-decomp/config/373307D9/symbols.txt
    //
    // TODO: These addresses are specific to the DC3 debug build. A future
    // enhancement could detect the build variant (debug vs retail) by
    // checking function prologues before patching, or use a title-specific
    // patch config file.
    //
    // TODO: Consider implementing proper NUI emulation in xenia's kernel
    // layer instead of guest memory patches. This would involve:
    //   1. Adding NUI kernel module exports (NuiInitialize, NuiShutdown, etc.)
    //      similar to xam_nui.cc's XamNui* exports
    //   2. Registering them in the export resolver so import thunks work
    //   3. But core NUI functions are statically linked, NOT imported, so
    //      this approach would require either:
    //      a) Modifying x64_emitter.cc's Call() to check kExtern behavior
    //         (currently only CallExtern for sc2 checks it), or
    //      b) Adding a guest function override mechanism to Processor that
    //         intercepts calls by address (using ExternHandler + patching
    //         the indirection table entry)
    //   4. A proper implementation could provide skeleton data, depth maps,
    //      etc. for automated testing of gesture/dance gameplay
    struct NuiPatch {
      uint32_t address;
      uint32_t insn0;  // First instruction (usually li r3, <value>)
      uint32_t insn1;  // Second instruction (usually blr)
      const char* name;
    };

    // Functions that MUST return S_OK (game asserts on failure):
    //   - NuiInitialize: LiveCameraInput ctor line 129 MILO_ASSERT_FMT
    //   - NuiSkeletonTrackingEnable: LiveCameraInput ctor line 141
    //   - NuiImageStreamOpen: LiveCameraInput ctor lines 146, 160
    //     Returns S_OK but does NOT write output handle -> handle stays NULL
    //     -> NuiImageStreamGetNextFrame guarded by if(handle) -> never called
    //
    // Functions that can return failure (game handles gracefully):
    //   - NuiAudioCreate: wrapped in if(SUCCEEDED(...)), no assert
    //   - NuiImageStreamGetNextFrame: no assert, just skips frame processing
    //
    // Functions called from destructor (NuiShutdown always, audio only if
    // NuiAudioCreate succeeded):
    //   - NuiShutdown: called unconditionally
    //   - NuiAudioRelease: only if unk11d4 set (NuiAudioCreate success)
    //
    // The NUI SDK has ~50 functions statically linked. We stub all known
    // entry points to return S_OK (0). Functions that produce frame data
    // (GetNextFrame) return E_UNEXPECTED (-1) to signal "no data available".
    //
    // TODO: For more complete Kinect emulation, these stubs could be
    // replaced with C++ handlers that provide:
    //   - Synthetic skeleton data for automated gameplay testing
    //   - Pre-recorded depth/color streams for regression testing
    //   - Scripted gesture sequences for CI/CD integration
    //   This would require the Processor-level ExternHandler approach
    //   described above, since bl-called functions don't go through
    //   the CallExtern codepath.

    NuiPatch patches[] = {
        // Core lifecycle
        {0x829D1200, kLiR3_0, kBlr, "NuiInitialize"},
        {0x829CEDA0, kLiR3_0, kBlr, "NuiShutdown"},

        // Skeleton tracking
        {0x829C25F0, kLiR3_0, kBlr, "NuiSkeletonTrackingEnable"},
        {0x829C1E18, kLiR3_0, kBlr, "NuiSkeletonTrackingDisable"},
        {0x829C1F90, kLiR3_0, kBlr, "NuiSkeletonSetTrackedSkeletons"},
        {0x829C2790, kLiR3_Neg1, kBlr, "NuiSkeletonGetNextFrame"},

        // Image streams - return S_OK but don't write output handle.
        // The handle pointer (r8) stays NULL -> GetNextFrame never called
        // because LiveCameraInput::PollTracking guards with if(curBuf.unk0).
        {0x829C9330, kLiR3_0, kBlr, "NuiImageStreamOpen"},
        {0x829C86F0, kLiR3_Neg1, kBlr, "NuiImageStreamGetNextFrame"},
        {0x829C8A18, kLiR3_0, kBlr, "NuiImageStreamReleaseFrame"},
        {0x829C91C8, kLiR3_0, kBlr, "NuiImageGetColorPixelCoordinatesFromDepthPixel"},

        // Audio - NuiAudioCreate returning failure (E_UNEXPECTED) prevents
        // the game from calling NuiAudioRegisterCallbacks (no assert).
        // LiveCameraInput sets unk11d4=0, destructor skips audio cleanup.
        //
        // TODO: NuiAudioCreate internally accesses global NUI state via
        // RtlEnterCriticalSection on a NUI mutex, allocates buffers with
        // XMemAlloc, calls XamVoiceGetMicArrayAudioEx for Kinect mic array,
        // creates 2 audio threads. Full audio emulation would require
        // implementing the NUIAUDIO subsystem with:
        //   - MEC (Microphone Echo Cancellation) stub
        //   - XamVoiceGetMicArrayAudioEx returning dummy audio streams
        //   - Audio processing thread stubs
        {0x82A0E028, kLiR3_Neg1, kBlr, "NuiAudioCreate"},
        {0x82A0DA48, kLiR3_Neg1, kBlr, "NuiAudioCreatePrivate"},
        {0x82A0D928, kLiR3_0, kBlr, "NuiAudioRegisterCallbacks"},
        {0x82A0D9A0, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacks"},
        {0x82A0C0A0, kLiR3_0, kBlr, "NuiAudioRegisterCallbacksPrivate"},
        {0x82A0C108, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacksPrivate"},
        {0x82A0D440, kLiR3_0, kBlr, "NuiAudioRelease"},

        // Camera properties - called at end of LiveCameraInput ctor
        // (SetColorCameraProperty) and in diagnostic/debug draw code.
        // Must not crash; accessing uninitialized NUI global state would
        // segfault without this stub.
        //
        // TODO: Camera property stubs could track set values in a map
        // and return them from Get calls, enabling camera config testing
        // without Kinect hardware.
        {0x829C7F48, kLiR3_0, kBlr, "NuiCameraSetProperty"},
        {0x829C7058, kLiR3_0, kBlr, "NuiCameraGetProperty"},
        {0x829C7068, kLiR3_0, kBlr, "NuiCameraGetPropertyF"},
        {0x829C7FA0, kLiR3_0, kBlr, "NuiCameraSetExposureRegionOfInterest"},
        {0x829C6868, kLiR3_0, kBlr, "NuiCameraGetExposureRegionOfInterest"},
        {0x829C3FE0, kLiR3_0, kBlr, "NuiCameraElevationSetAngle"},
        {0x829C3EF8, kLiR3_0, kBlr, "NuiCameraElevationGetAngle"},
        {0x829C4940, kLiR3_0, kBlr, "NuiCameraAdjustTilt"},
        {0x829C4E38, kLiR3_0, kBlr, "NuiCameraGetNormalToGravity"},

        // Identity (used by Skeleton.cpp for player identification)
        //
        // TODO: NuiIdentityIdentify takes a tracking ID, flags, callback,
        // and user data. A proper stub could invoke the callback with a
        // "no match" result to simulate identity processing completing.
        {0x829C36B0, kLiR3_0, kBlr, "NuiIdentityEnroll"},
        {0x829C3870, kLiR3_0, kBlr, "NuiIdentityIdentify"},
        {0x829C3998, kLiR3_0, kBlr, "NuiIdentityGetEnrollmentInformation"},
        {0x829C3BB0, kLiR3_0, kBlr, "NuiIdentityAbort"},

        // Fitness tracking (FitnessFilter.cpp)
        // Only called during fitness gameplay mode. All use MILO_NOTIFY
        // on failure (not MILO_ASSERT), so failure is safe.
        {0x829D1B68, kLiR3_Neg1, kBlr, "NuiFitnessStartTracking"},
        {0x829D1E30, kLiR3_Neg1, kBlr, "NuiFitnessPauseTracking"},
        {0x829D1F00, kLiR3_Neg1, kBlr, "NuiFitnessResumeTracking"},
        {0x829D1FD0, kLiR3_Neg1, kBlr, "NuiFitnessStopTracking"},
        {0x82E61690, kLiR3_Neg1, kBlr, "NuiFitnessGetCurrentFitnessData"},

        // Wave gesture (WaveToTurnOnLight.cpp)
        {0x829D1758, kLiR3_Neg1, kBlr, "NuiWaveSetEnabled"},
        {0x829D1668, kLiR3_Neg1, kBlr, "NuiWaveGetGestureOwnerProgress"},

        // Head tracking
        {0x829DA0C8, kLiR3_0, kBlr, "NuiHeadOrientationDisable"},
        {0x829DA598, kLiR3_0, kBlr, "NuiHeadPositionDisable"},

        // Speech recognition (SpeechMgr.cpp) - many have MILO_ASSERT_FMT.
        // SpeechMgr is only created if kinect.speech.enabled=1 in config.
        // If speech IS enabled, these must return S_OK to avoid asserts
        // in NuiSpeechCreateGrammar, NuiSpeechCommitGrammar, etc.
        //
        // TODO: Speech emulation could accept pre-scripted voice commands
        // for automated testing of menu navigation and gameplay triggers.
        // Would need to implement:
        //   - Grammar state management (rule tree in host memory)
        //   - Event queue with synthetic recognition events
        //   - NuiSpeechGetEvents returning scripted results
        {0x82A24B88, kLiR3_0, kBlr, "NuiSpeechEnable"},
        {0x82A23B70, kLiR3_0, kBlr, "NuiSpeechDisable"},
        {0x82A23BB0, kLiR3_0, kBlr, "NuiSpeechCreateGrammar"},
        {0x82A23B80, kLiR3_0, kBlr, "NuiSpeechLoadGrammar"},
        {0x82A23BA0, kLiR3_0, kBlr, "NuiSpeechUnloadGrammar"},
        {0x82A22A48, kLiR3_0, kBlr, "NuiSpeechCommitGrammar"},
        {0x82A21068, kLiR3_0, kBlr, "NuiSpeechStartRecognition"},
        {0x82A22978, kLiR3_0, kBlr, "NuiSpeechStopRecognition"},
        {0x82A21090, kLiR3_0, kBlr, "NuiSpeechSetEventInterest"},
        {0x82A21078, kLiR3_0, kBlr, "NuiSpeechSetGrammarState"},
        {0x82A22998, kLiR3_0, kBlr, "NuiSpeechSetRuleState"},
        {0x82A229B8, kLiR3_0, kBlr, "NuiSpeechCreateRule"},
        {0x82A229E0, kLiR3_0, kBlr, "NuiSpeechCreateState"},
        {0x82A22A00, kLiR3_0, kBlr, "NuiSpeechAddWordTransition"},
        {0x82A210A0, kLiR3_Neg1, kBlr, "NuiSpeechGetEvents"},
        {0x82A22988, kLiR3_0, kBlr, "NuiSpeechDestroyEvent"},
        {0x82A24AB0, kLiR3_0, kBlr, "NuiSpeechEmulateRecognition"},

        // Misc
        {0x82B57560, kLiR3_0, kBlr, "NuiMetaCpuEvent"},

        // Xbox SmartGlass (XBC) SDK - statically linked from XBC.lib
        // SmartGlassInit() calls XbcInitialize(), which calls
        // CXbcImpl::Initialize(). If Initialize returns failure, the game
        // prints "Failed to initialize Xbox SmartGlass library." and crashes.
        // The thunk functions (XbcInitialize, XbcDoWork, XbcSendJSON) are
        // only 4 bytes each (branch instructions), so we stub the real
        // CXbcImpl implementations instead.
        //
        // TODO: SmartGlass could be used for controller input automation
        // (e.g., sending menu selections from a test harness). Would need:
        //   - JSON message parsing/generation
        //   - Client connection state management
        //   - XLRC (Xbox Live Real-time Communication) stub layer
        {0x82606078, kLiR3_0, kBlr, "CXbcImpl::Initialize"},
        {0x82605960, kLiR3_0, kBlr, "CXbcImpl::DoWork"},
        {0x82605DF8, kLiR3_0, kBlr, "CXbcImpl::SendJSON"},
    };

    int patched = 0;
    for (const auto& patch : patches) {
      // Make the guest code page writable before patching.
      // XEX code pages are loaded as read-only, so we need to temporarily
      // allow writes. We leave pages as RW since the JIT reads instructions
      // once and translates to x64 — the PPC pages aren't executed directly.
      auto* heap = memory_->LookupHeap(patch.address);
      if (!heap) {
        XELOGW("  Skip {:08X}: {} (no heap)", patch.address, patch.name);
        continue;
      }
      heap->Protect(patch.address, 8,
                    kMemoryProtectRead | kMemoryProtectWrite);

      auto* mem = memory_->TranslateVirtual<uint8_t*>(patch.address);
      xe::store_and_swap<uint32_t>(mem + 0, patch.insn0);
      xe::store_and_swap<uint32_t>(mem + 4, patch.insn1);
      patched++;

      XELOGI("  Patched {:08X}: {} -> [{:08X} {:08X}]",
             patch.address, patch.name, patch.insn0, patch.insn1);
    }
    XELOGI("DC3: Patched {}/{} NUI functions", patched,
           (int)(sizeof(patches) / sizeof(patches[0])));

    // Fake Kinect skeleton data injection
    // Replaces the NuiSkeletonGetNextFrame stub with a PPC routine that
    // copies pre-built T-pose skeleton data to the caller's output buffer
    // with incrementing timestamps. Also patches SkeletonUpdate's thread
    // to loop every 33ms instead of blocking on sNewSkeletonEvent, and
    // NOPs the IsOverride check so SkeletonFrame::Create() always runs.
    if (cvars::fake_kinect_data) {
      const uint32_t kGetNextFrameAddr = 0x829C2790;
      const uint32_t kSkeletonFrameSize = 0xAB0;  // 2736 bytes

      // Allocate guest heap memory for skeleton template + frame counter.
      // We MUST NOT place this data at kGetNextFrameAddr+N because the NUI
      // library has internal functions (NuipConvertSTSkeletons at 0x829C2A10,
      // NuipAccumulateSkeletonFrameStats at 0x829C2C50) adjacent to
      // NuiSkeletonGetNextFrame that would be overwritten.
      const uint32_t kDataSize = kSkeletonFrameSize + 4;  // +4 for counter
      uint32_t data_guest_addr =
          memory_->SystemHeapAlloc(kDataSize, 0x10);
      if (!data_guest_addr) {
        XELOGW("DC3: Failed to allocate guest memory for fake skeleton data");
      }
      const uint32_t kSkeletonDataAddr = data_guest_addr;
      const uint32_t kCounterAddr = data_guest_addr + kSkeletonFrameSize;

      // Make the stub location writable (just the 19-instruction stub)
      auto* heap = memory_->LookupHeap(kGetNextFrameAddr);
      if (heap && data_guest_addr) {
        heap->Protect(kGetNextFrameAddr, 0x4C,
                      kMemoryProtectRead | kMemoryProtectWrite);

        auto* stub_mem = memory_->TranslateVirtual<uint8_t*>(kGetNextFrameAddr);

        // Write PPC stub code:
        // NuiSkeletonGetNextFrame(DWORD timeout /*r3*/, NUI_SKELETON_FRAME* out /*r4*/)
        //   mr r8, r4                       ; save output pointer
        //   lis r5, hi(kSkeletonDataAddr)
        //   ori r5, r5, lo(kSkeletonDataAddr)
        //   li r6, word_count
        //   mtctr r6
        // loop:
        //   lwz r7, 0(r5)
        //   stw r7, 0(r4)
        //   addi r5, r5, 4
        //   addi r4, r4, 4
        //   bdnz loop
        //   lis r5, hi(kCounterAddr)        ; load frame counter
        //   ori r5, r5, lo(kCounterAddr)
        //   lwz r6, 0(r5)
        //   addi r6, r6, 1                  ; increment
        //   stw r6, 0(r5)                   ; store back
        //   stw r6, 4(r8)                   ; timestamp low word in output
        //   stw r6, 8(r8)                   ; frame number in output
        //   li r3, 0                        ; return S_OK
        //   blr
        uint32_t ppc_stub[] = {
            0x7C882378,                                       // mr r8, r4
            0x3CA00000 | (kSkeletonDataAddr >> 16),           // lis r5, hi16(data)
            0x60A50000 | (kSkeletonDataAddr & 0xFFFF),        // ori r5, r5, lo16(data)
            0x38C00000 | (kSkeletonFrameSize / 4),            // li r6, word_count
            0x7CC903A6,                                       // mtctr r6
            0x80E50000,                                       // lwz r7, 0(r5)
            0x90E40000,                                       // stw r7, 0(r4)
            0x38A50004,                                       // addi r5, r5, 4
            0x38840004,                                       // addi r4, r4, 4
            0x4200FFF0,                                       // bdnz -16 (to lwz)
            0x3CA00000 | (kCounterAddr >> 16),                // lis r5, hi16(counter)
            0x60A50000 | (kCounterAddr & 0xFFFF),             // ori r5, r5, lo16(counter)
            0x80C50000,                                       // lwz r6, 0(r5)
            0x38C60001,                                       // addi r6, r6, 1
            0x90C50000,                                       // stw r6, 0(r5)
            0x90C80004,                                       // stw r6, 4(r8) (timestamp)
            0x90C80008,                                       // stw r6, 8(r8) (frame num)
            0x38600000,                                       // li r3, 0 (S_OK)
            0x4E800020,                                       // blr
        };
        for (size_t i = 0; i < sizeof(ppc_stub) / sizeof(ppc_stub[0]); i++) {
          xe::store_and_swap<uint32_t>(stub_mem + i * 4, ppc_stub[i]);
        }

        // Initialize frame counter to 0
        xe::store_and_swap<uint32_t>(
            memory_->TranslateVirtual<uint8_t*>(kCounterAddr), 0);

        // Write pre-built NUI_SKELETON_FRAME at kSkeletonDataAddr
        auto* data_mem = memory_->TranslateVirtual<uint8_t*>(kSkeletonDataAddr);
        std::memset(data_mem, 0, kSkeletonFrameSize);

        // Helper to write a big-endian float at an offset
        auto write_float = [data_mem](uint32_t offset, float value) {
          xe::store_and_swap<float>(data_mem + offset, value);
        };
        auto write_u32 = [data_mem](uint32_t offset, uint32_t value) {
          xe::store_and_swap<uint32_t>(data_mem + offset, value);
        };

        // NUI_SKELETON_FRAME layout:
        //   0x0000: LARGE_INTEGER liTimeStamp (8 bytes)
        //   0x0008: DWORD dwFrameNumber
        //   0x000C: DWORD dwFlags
        //   0x0010: XMVECTOR vFloorClipPlane (x,y,z,w)
        //   0x0020: XMVECTOR vNormalToGravity (x,y,z,w)
        //   0x0030: NUI_SKELETON_DATA[6] (each 0x1C0 = 448 bytes)
        write_u32(0x0008, 1);  // dwFrameNumber = 1

        // Floor clip plane: Y-up (0, 1, 0, 0)
        write_float(0x0014, 1.0f);  // vFloorClipPlane.y

        // Normal to gravity: Y-up (0, 1, 0, 0)
        write_float(0x0024, 1.0f);  // vNormalToGravity.y

        // NUI_SKELETON_DATA[0] at offset 0x30:
        //   0x00: eTrackingState (DWORD)
        //   0x04: dwTrackingID
        //   0x08: dwEnrollmentIndex
        //   0x0C: dwUserIndex
        //   0x10: Position (XMVECTOR, 16 bytes)
        //   0x20: SkeletonPositions[20] (20 × 16 bytes = 320 bytes)
        //   0x160: TrackingStates[20] (20 × 4 bytes = 80 bytes)
        //   0x1B0: dwQualityFlags
        const uint32_t skel0 = 0x30;  // offset of SkeletonData[0]

        write_u32(skel0 + 0x00, 2);   // eTrackingState = NUI_SKELETON_TRACKED
        write_u32(skel0 + 0x04, 1);   // dwTrackingID = 1
        write_u32(skel0 + 0x0C, 0);   // dwUserIndex = 0

        // Center of mass position
        write_float(skel0 + 0x10, 0.0f);   // Position.x
        write_float(skel0 + 0x14, 0.9f);   // Position.y
        write_float(skel0 + 0x18, 2.0f);   // Position.z
        write_float(skel0 + 0x1C, 1.0f);   // Position.w

        // T-pose joint positions (20 joints × XMVECTOR)
        // Positions in meters, Kinect camera coordinates (X=left/right, Y=up, Z=depth)
        struct JointPos { float x, y, z; };
        JointPos joints[20] = {
            { 0.00f, 0.90f, 2.0f},  //  0: Hip Center
            { 0.00f, 1.10f, 2.0f},  //  1: Spine
            { 0.00f, 1.35f, 2.0f},  //  2: Shoulder Center
            { 0.00f, 1.60f, 2.0f},  //  3: Head
            {-0.20f, 1.35f, 2.0f},  //  4: Shoulder Left
            {-0.50f, 1.35f, 2.0f},  //  5: Elbow Left
            {-0.75f, 1.35f, 2.0f},  //  6: Wrist Left
            {-0.85f, 1.35f, 2.0f},  //  7: Hand Left
            { 0.20f, 1.35f, 2.0f},  //  8: Shoulder Right
            { 0.50f, 1.35f, 2.0f},  //  9: Elbow Right
            { 0.75f, 1.35f, 2.0f},  // 10: Wrist Right
            { 0.85f, 1.35f, 2.0f},  // 11: Hand Right
            {-0.15f, 0.90f, 2.0f},  // 12: Hip Left
            {-0.15f, 0.50f, 2.0f},  // 13: Knee Left
            {-0.15f, 0.05f, 2.0f},  // 14: Ankle Left
            { 0.15f, 0.90f, 2.0f},  // 15: Hip Right
            { 0.15f, 0.50f, 2.0f},  // 16: Knee Right
            { 0.15f, 0.05f, 2.0f},  // 17: Ankle Right
            {-0.15f, 0.00f, 2.0f},  // 18: Foot Left
            { 0.15f, 0.00f, 2.0f},  // 19: Foot Right
        };

        const uint32_t joints_offset = skel0 + 0x20;  // SkeletonPositions[0]
        for (int j = 0; j < 20; j++) {
          uint32_t off = joints_offset + j * 16;
          write_float(off + 0, joints[j].x);
          write_float(off + 4, joints[j].y);
          write_float(off + 8, joints[j].z);
          write_float(off + 12, 1.0f);  // w = 1.0
        }

        // Set all 20 joint tracking states to TRACKED (2)
        const uint32_t tracking_offset = skel0 + 0x160;
        for (int j = 0; j < 20; j++) {
          write_u32(tracking_offset + j * 4, 2);  // NUI_SKELETON_POSITION_TRACKED
        }

        // Binary patches for SkeletonUpdate (debug build addresses).
        //
        // Problem: The SkeletonUpdateThread blocks forever on
        // WaitForSingleObject(sNewSkeletonEvent, INFINITE) because the NUI SDK
        // never signals the event. Even if the thread runs, Update() skips
        // SkeletonFrame::Create() when IsOverride() is true (which it is for
        // LiveCameraInput), so the NUI skeleton data never gets converted.
        //
        // Fix: (1) Change the wait timeout from INFINITE to 33ms so the thread
        // polls in a loop. (2) NOP the IsOverride branch so Create() always
        // runs when NuiSkeletonGetNextFrame succeeds.
        struct BinaryPatch {
          uint32_t address;
          uint32_t value;
          const char* name;
        };
        BinaryPatch skel_patches[] = {
            // SkeletonUpdateThread: li r28, -1 -> li r28, 33
            // Makes WaitForSingleObject poll every 33ms instead of blocking
            {0x8242E74C, 0x3B800021,
             "SkeletonUpdateThread: timeout INFINITE -> 33ms"},
            // SkeletonUpdate::Update(): bne (IsOverride skip) -> nop
            // Allows SkeletonFrame::Create() to run with NUI data
            {0x8242E1B0, 0x60000000,
             "SkeletonUpdate::Update: NOP IsOverride branch"},
        };
        for (const auto& p : skel_patches) {
          auto* h = memory_->LookupHeap(p.address);
          if (h) {
            h->Protect(p.address, 4, kMemoryProtectRead | kMemoryProtectWrite);
            auto* m = memory_->TranslateVirtual<uint8_t*>(p.address);
            xe::store_and_swap<uint32_t>(m, p.value);
            XELOGI("  Patched {:08X}: {}", p.address, p.name);
          }
        }

        XELOGI("DC3: Fake Kinect skeleton data written at {:08X} ({} bytes), "
               "PPC stub at {:08X}, counter at {:08X}",
               kSkeletonDataAddr, kSkeletonFrameSize, kGetNextFrameAddr,
               kCounterAddr);
      }
    }
  }

  auto main_thread = kernel_state_->LaunchModule(module);
  if (!main_thread) {
    return X_STATUS_UNSUCCESSFUL;
  }
  main_thread_ = main_thread;
  on_launch(title_id_.value(), title_name_);

  return X_STATUS_SUCCESS;
}

}  // namespace xe
