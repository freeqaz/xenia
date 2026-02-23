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
#include <cctype>
#include <cinttypes>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string_view>
#include <set>
#include <unordered_map>

#if XE_PLATFORM_LINUX
#include <sys/mman.h>
#include <cerrno>
#endif

#include "config.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/error/en.h"
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
#include "xenia/dc3_hack_pack.h"
#include "xenia/dc3_nui_patch_resolver.h"
#include "xenia/dc3_runtime_telemetry.h"
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

DEFINE_int32(dc3_crt_bisect_max, -1,
             "DC3: max CRT constructor index to allow (-1=disabled/all run, "
             "0=only index 0 runs, N=indices 0..N run). "
             "Used for binary search to find heap-corrupting constructor.",
             "DC3");
DEFINE_string(dc3_crt_skip_indices, "",
              "DC3: comma-separated list of CRT constructor indices to "
              "nullify. Supports ranges: '69,75,98-340'. "
              "Default empty = use dc3_crt_skip_nui.",
              "DC3");
DEFINE_bool(dc3_crt_skip_nui, true,
            "DC3: auto-nullify NUI/Kinect SDK CRT constructors (indices "
            "69,75,98-101,210-328). These call unresolved internal NUI "
            "functions that corrupt the heap. Set false to disable.",
            "DC3");
DEFINE_bool(dc3_guest_overrides, true,
            "DC3: use guest extern overrides for eligible simple NUI/XBC "
            "stub-return functions (default cutover path; skips byte patching "
            "for registered entries; preserves fake_kinect_data "
            "NuiSkeletonGetNextFrame path). The legacy NUI/XBC byte-patch "
            "fallback path has been removed; false logs a warning and is "
            "ignored for this path.",
            "DC3");
DEFINE_string(dc3_nui_patch_layout, "auto",
              "DC3: NUI/XBC patch address layout selector "
              "(auto|original|decomp). 'auto' uses the zero-padding heuristic "
              "and logs a .text fingerprint for future resolver matching.",
              "DC3");
DEFINE_string(dc3_nui_layout_fingerprint_original, "",
              "DC3: optional .text FNV1a64 fingerprint (hex) for original "
              "NUI/XBC patch layout selection in auto mode.",
              "DC3");
DEFINE_string(dc3_nui_layout_fingerprint_decomp, "",
              "DC3: optional .text FNV1a64 fingerprint (hex) for decomp "
              "NUI/XBC patch layout selection in auto mode.",
              "DC3");
DEFINE_string(
    dc3_nui_layout_fingerprint_cache_path, "",
    "DC3: optional fingerprint cache file with lines "
    "'original=<hex>' and/or 'decomp=<hex>' for auto layout selection.",
    "DC3");
DEFINE_string(
    dc3_nui_symbol_map_path, "",
    "DC3: optional symbol map manifest used by the NUI/XBC resolver "
    "(symbols.txt-style 'name = .text:0xADDR;'). If unset, a local "
    "dc3-decomp symbols.txt path is auto-probed.",
    "DC3");
DEFINE_string(dc3_nui_patch_resolver_mode, "hybrid",
              "DC3: NUI/XBC patch target resolver mode "
              "(hybrid|strict). hybrid uses manifest/symbol/signature "
              "resolution before catalog fallback; strict disables raw "
              "catalog fallback.",
              "DC3");
DEFINE_string(
    dc3_nui_patch_manifest_path, "",
    "DC3: optional machine-readable DC3 NUI/XBC patch manifest JSON "
    "(xenia_dc3_patch_manifest.json). Preferred over symbols.txt when present.",
    "DC3");
DEFINE_bool(dc3_nui_enable_signature_resolver, true,
            "DC3: enable signature-resolver hook for NUI/XBC patch targets "
            "(default on; used by hybrid/strict resolver modes).",
            "DC3");
DEFINE_bool(dc3_nui_signature_trace, false,
            "DC3: log runtime PPC words for NUI/XBC patch targets "
            "at catalog and resolved addresses (debugging signature resolver).",
            "DC3");

namespace xe {

using namespace xe::literals;

namespace {
using namespace xe::dc3;

void Dc3NuiReturnOkExtern(cpu::ppc::PPCContext* ppc_context,
                          kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }
  ppc_context->r[3] = 0;
}

void Dc3NuiReturnNeg1Extern(cpu::ppc::PPCContext* ppc_context,
                            kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }
  ppc_context->r[3] = UINT64_C(0xFFFFFFFFFFFFFFFF);
}

}  // namespace

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

  if (processor_) {
    processor_->ClearGuestFunctionOverrides();
  }
  Dc3RuntimeTelemetryEndSession("terminate_title");
  kernel_state_->TerminateTitle();
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  on_terminate();
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::LaunchPath(const std::filesystem::path& path) {
  Dc3RuntimeTelemetryEndSession("launch_path_reset");
  if (processor_) {
    processor_->ClearGuestFunctionOverrides();
  }
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

  auto context = current_thread->thread_state()->context();

  XELOGE("==== CRASH DUMP ====");
  XELOGE("Thread ID (Host: 0x{:08X} / Guest: 0x{:08X})",
         current_thread->thread()->system_id(), current_thread->thread_id());
  XELOGE("Thread Handle: 0x{:08X}", current_thread->handle());
  if (guest_function) {
    XELOGE("PC: 0x{:08X}",
           guest_function->MapMachineCodeToGuestAddress(ex->pc()));
  } else {
    XELOGE("PC: <unknown guest function, host PC=0x{:016X}>", ex->pc());
  }
  XELOGE("Guest lr: 0x{:08X}", static_cast<uint32_t>(context->lr));
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

  // Dump fault address details for access violations.
  if (ex->code() == Exception::Code::kAccessViolation) {
    uint64_t host_fault = ex->fault_address();
    uint64_t membase =
        reinterpret_cast<uintptr_t>(memory_->virtual_membase());
    uint32_t guest_fault = static_cast<uint32_t>(host_fault - membase);
    XELOGE("Fault address: host=0x{:016X} guest=0x{:08X} ({})", host_fault,
           guest_fault,
           ex->access_violation_operation() ==
                   Exception::AccessViolationOperation::kWrite
               ? "WRITE"
               : "READ");
  }

  // Dump guest function info and code around crash PC.
  if (guest_function) {
    uint32_t guest_pc = guest_function->MapMachineCodeToGuestAddress(ex->pc());
    XELOGE("Guest function: {} (0x{:08X})", guest_function->name(),
           guest_function->address());
    if (guest_pc >= 0x82000000 && guest_pc < 0x90000000) {
      uint32_t dump_start = (guest_pc > 0x40) ? (guest_pc - 0x40) : guest_pc;
      uint32_t dump_end = guest_pc + 0x40;
      XELOGE("Guest code near PC 0x{:08X}:", guest_pc);
      for (uint32_t addr = dump_start; addr < dump_end; addr += 4) {
        auto* mem_ptr = memory_->TranslateVirtual<uint8_t*>(addr);
        if (!mem_ptr) break;
        uint32_t instr = xe::load_and_swap<uint32_t>(mem_ptr);
        XELOGE("  0x{:08X}: {:08X}{}", addr, instr,
               addr == guest_pc ? "  <-- CRASH PC" : "");
      }
    }
  }

  // Walk the PPC stack to identify call chain (helps diagnose recursion).
  {
    uint32_t sp = static_cast<uint32_t>(context->r[1]);
    XELOGE("==== STACK WALK (SP=0x{:08X}) ====", sp);
    int frame = 0;
    uint32_t last_lr = 0;
    int repeat_count = 0;
    for (; frame < 20000 && sp >= 0x70000000 && sp < 0x78000000; frame++) {
      auto* host_ptr = memory_->TranslateVirtual<uint8_t*>(sp);
      if (!host_ptr) break;
      uint32_t back_chain = xe::load_and_swap<uint32_t>(host_ptr);
      // Try multiple LR save locations:
      uint32_t lr_sp4 = xe::load_and_swap<uint32_t>(host_ptr + 4);
      uint32_t lr_sp8 = xe::load_and_swap<uint32_t>(host_ptr + 8);
      // Also try __savegprlr convention: LR at back_chain - 8
      uint32_t lr_bc8 = 0;
      if (back_chain >= 0x70000000 && back_chain < 0x78000000) {
        auto* bc_ptr = memory_->TranslateVirtual<uint8_t*>(back_chain - 8);
        if (bc_ptr) lr_bc8 = xe::load_and_swap<uint32_t>(bc_ptr);
      }
      // Pick the most likely LR (first non-BEBEBEBE, non-zero, in code range)
      uint32_t best_lr = 0;
      if (lr_bc8 >= 0x82000000 && lr_bc8 < 0x8A000000) best_lr = lr_bc8;
      else if (lr_sp4 >= 0x82000000 && lr_sp4 < 0x8A000000) best_lr = lr_sp4;
      else if (lr_sp8 >= 0x82000000 && lr_sp8 < 0x8A000000) best_lr = lr_sp8;
      else best_lr = lr_sp4;  // fallback
      uint32_t frame_size = (back_chain > sp) ? (back_chain - sp) : 0;
      // Log with sampling: first 30 frames, then every 100th, last 10
      bool should_log = (frame < 30) || (frame % 100 == 0) ||
                         (best_lr != last_lr && best_lr != 0xBEBEBEBE);
      if (should_log) {
        if (repeat_count > 0) {
          XELOGE("  ... ({} identical frames skipped, lr=0x{:08X})",
                 repeat_count, last_lr);
          repeat_count = 0;
        }
        XELOGE("  [{}] sp=0x{:08X} sz={} lr_sp4=0x{:08X} lr_bc8=0x{:08X}",
               frame, sp, frame_size, lr_sp4, lr_bc8);
      } else {
        repeat_count++;
      }
      last_lr = best_lr;
      if (back_chain == 0 || back_chain == sp || back_chain < 0x70000000 ||
          back_chain >= 0x78000000)
        break;
      sp = back_chain;
    }
    if (repeat_count > 0) {
      XELOGE("  ... ({} identical frames skipped, lr=0x{:08X})",
             repeat_count, last_lr);
    }
    XELOGE("==== END STACK WALK ({} frames) ====", frame);
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
  if (guest_function) {
    XELOGE("Guest crashed! PC: 0x{:08X}",
           guest_function->MapMachineCodeToGuestAddress(ex->pc()));
  } else {
    XELOGE("Guest crashed! host PC=0x{:016X} (guest function unknown)", ex->pc());
  }
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
  bool dc3_is_decomp_layout = false;
  if (title_id_.has_value() && title_id_.value() == 0x373307D9) {
    Dc3MaybeCleanStaleContentCache(content_root_);
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

    Dc3NuiPatchSpec patches[] = {
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
        // NOTE: 0x82A24A98 was previously stubbed as "NuiSpeech__E_init"
        // but MAP file reveals it's actually Object::sFactories static
        // initializer (Object.obj) — a critical game engine function.
        // Stubbing it broke the object factory system and caused hangs
        // in downstream initializers (gPropPaths at 0x82A24B28).
        //
        // The original JIT boundary issue (blr in EmulateRecognition stub
        // at 0x82A24AB0 getting scanned into the initializer) is avoided
        // by also not stubbing NuiSpeechEmulateRecognition — the original
        // function prologue doesn't have an early blr, so the JIT
        // boundary detection works correctly with the original code.
        //
        // NuiSpeechEmulateRecognition (0x82A24AB0) is never called
        // because all speech API entry points are already stubbed above.

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

    // Decomp XEX patch table: NUI functions at decomp MAP public symbol
    // addresses. Used when the original-address patches don't match the
    // loaded XEX layout. Addresses sourced from manifest generator
    // (generate_xenia_dc3_patch_manifest.py) which reads the MAP file.
    // When the manifest JSON is available, the resolver prefers manifest
    // addresses over these catalog entries.
    Dc3NuiPatchSpec decomp_patches[] = {
        // Core lifecycle (nuiruntime.obj)
        {0x835D8B0C, kLiR3_0, kBlr, "NuiInitialize"},
        {0x835D66D0, kLiR3_0, kBlr, "NuiShutdown"},
        {0x8373F0E0, kLiR3_0, kBlr, "NuiMetaCpuEvent"},

        // Skeleton tracking (nuiskeleton.obj)
        {0x835CA2D8, kLiR3_0, kBlr, "NuiSkeletonTrackingEnable"},
        {0x835C9B28, kLiR3_0, kBlr, "NuiSkeletonTrackingDisable"},
        {0x835C9C98, kLiR3_0, kBlr, "NuiSkeletonSetTrackedSkeletons"},
        {0x835CA478, kLiR3_Neg1, kBlr, "NuiSkeletonGetNextFrame"},

        // Image streams (nuiimagecamera.obj)
        {0x835D0F7C, kLiR3_0, kBlr, "NuiImageStreamOpen"},
        {0x835D0354, kLiR3_Neg1, kBlr, "NuiImageStreamGetNextFrame"},
        {0x835D067C, kLiR3_0, kBlr, "NuiImageStreamReleaseFrame"},
        {0x835D0E18, kLiR3_0, kBlr,
         "NuiImageGetColorPixelCoordinatesFromDepthPixel"},

        // Audio (nuiaudio.obj)
        {0x8360B778, kLiR3_Neg1, kBlr, "NuiAudioCreate"},
        {0x8360B198, kLiR3_Neg1, kBlr, "NuiAudioCreatePrivate"},
        {0x8360B07C, kLiR3_0, kBlr, "NuiAudioRegisterCallbacks"},
        {0x8360B0F4, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacks"},
        {0x83609818, kLiR3_0, kBlr, "NuiAudioRegisterCallbacksPrivate"},
        {0x83609880, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacksPrivate"},
        {0x8360AB98, kLiR3_0, kBlr, "NuiAudioRelease"},

        // Camera properties (nuidetroit.obj, nuiimagecameraproperties.obj)
        {0x835CFBC4, kLiR3_0, kBlr, "NuiCameraSetProperty"},
        {0x835CBBB4, kLiR3_0, kBlr, "NuiCameraElevationGetAngle"},
        {0x835CBC94, kLiR3_0, kBlr, "NuiCameraElevationSetAngle"},
        {0x835CC5E0, kLiR3_0, kBlr, "NuiCameraAdjustTilt"},
        {0x835CCAD0, kLiR3_0, kBlr, "NuiCameraGetNormalToGravity"},
        {0x835CFC1C, kLiR3_0, kBlr, "NuiCameraSetExposureRegionOfInterest"},
        {0x835CE4DC, kLiR3_0, kBlr, "NuiCameraGetExposureRegionOfInterest"},
        {0x835CECE0, kLiR3_0, kBlr, "NuiCameraGetProperty"},
        {0x835CECF0, kLiR3_0, kBlr, "NuiCameraGetPropertyF"},

        // Identity (identityapi.obj)
        {0x835CB384, kLiR3_0, kBlr, "NuiIdentityEnroll"},
        {0x835CB540, kLiR3_0, kBlr, "NuiIdentityIdentify"},
        {0x835CB668, kLiR3_0, kBlr, "NuiIdentityGetEnrollmentInformation"},
        {0x835CB87C, kLiR3_0, kBlr, "NuiIdentityAbort"},

        // Fitness (nuifitnessapi.obj, nuifitnessxam.obj)
        {0x835D9460, kLiR3_Neg1, kBlr, "NuiFitnessStartTracking"},
        {0x835D9728, kLiR3_Neg1, kBlr, "NuiFitnessPauseTracking"},
        {0x835D97F8, kLiR3_Neg1, kBlr, "NuiFitnessResumeTracking"},
        {0x835D98C8, kLiR3_Neg1, kBlr, "NuiFitnessStopTracking"},
        {0x83901464, kLiR3_Neg1, kBlr, "NuiFitnessGetCurrentFitnessData"},

        // Wave gesture (nuiwave.obj)
        {0x835D905C, kLiR3_Neg1, kBlr, "NuiWaveSetEnabled"},
        {0x835D8F6C, kLiR3_Neg1, kBlr, "NuiWaveGetGestureOwnerProgress"},

        // Head tracking (nuiheadposition.obj, nuiheadorientation.obj)
        {0x83293DE0, kLiR3_0, kBlr, "NuiHeadPositionDisable"},
        {0x835E18FC, kLiR3_0, kBlr, "NuiHeadOrientationDisable"},

        // Speech (xspeechapi.obj)
        {0x832C6B88, kLiR3_0, kBlr, "NuiSpeechEnable"},
        {0x832C5B90, kLiR3_0, kBlr, "NuiSpeechDisable"},
        {0x832C5BCC, kLiR3_0, kBlr, "NuiSpeechCreateGrammar"},
        {0x832C5B9C, kLiR3_0, kBlr, "NuiSpeechLoadGrammar"},
        {0x832C5BBC, kLiR3_0, kBlr, "NuiSpeechUnloadGrammar"},
        {0x832C4A7C, kLiR3_0, kBlr, "NuiSpeechCommitGrammar"},
        {0x832C30E8, kLiR3_0, kBlr, "NuiSpeechStartRecognition"},
        {0x832C49B8, kLiR3_0, kBlr, "NuiSpeechStopRecognition"},
        {0x832C3110, kLiR3_0, kBlr, "NuiSpeechSetEventInterest"},
        {0x832C30F8, kLiR3_0, kBlr, "NuiSpeechSetGrammarState"},
        {0x832C49D8, kLiR3_0, kBlr, "NuiSpeechSetRuleState"},
        {0x832C49F4, kLiR3_0, kBlr, "NuiSpeechCreateRule"},
        {0x832C4A18, kLiR3_0, kBlr, "NuiSpeechCreateState"},
        {0x832C4A34, kLiR3_0, kBlr, "NuiSpeechAddWordTransition"},
        {0x832C3120, kLiR3_Neg1, kBlr, "NuiSpeechGetEvents"},
        {0x832C49C8, kLiR3_0, kBlr, "NuiSpeechDestroyEvent"},
        {0x832C6AB4, kLiR3_0, kBlr, "NuiSpeechEmulateRecognition"},

        // SmartGlass (XBC) - (xbcimpl.obj)
        {0x8352C7A4, kLiR3_0, kBlr, "CXbcImpl::Initialize"},
        {0x8352C0AC, kLiR3_0, kBlr, "CXbcImpl::DoWork"},
        {0x8352C530, kLiR3_0, kBlr, "CXbcImpl::SendJSON"},

        // D3D NUI (nui.obj)
        {0x837948A8, kLiR3_0, kBlr, "D3DDevice_NuiInitialize"},
        {0x8378DC78, kLiR3_0, kBlr, "D3DDevice_NuiMetaData"},
        {0x83794920, kLiR3_0, kBlr, "D3DDevice_NuiStart"},
        {0x83794964, kLiR3_0, kBlr, "D3DDevice_NuiStop"},

        // Internal NUI (Nuip*) from manifest
        {0x835E2FE4, kLiR3_0, kBlr, "NuipBuildXamNuiFrameData"},
        {0x835CDF3C, kLiR3_0, kBlr, "NuipCameraGetExposureRegionOfInterest"},
        {0x835CE548, kLiR3_0, kBlr, "NuipCameraGetProperty"},
        {0x835CE850, kLiR3_0, kBlr, "NuipCameraGetPropertyF"},
        {0x8363A7B4, kLiR3_0, kBlr, "NuipCreateInstance"},
        {0x835D92EC, kLiR3_0, kBlr, "NuipFitnessInitialize"},
        {0x835D9B98, kLiR3_0, kBlr, "NuipFitnessNewSkeletalFrame"},
        {0x835D93C8, kLiR3_0, kBlr, "NuipFitnessShutdown"},
        {0x835D8120, kLiR3_0, kBlr, "NuipInitialize"},
        {0x83641870, kLiR3_0, kBlr, "NuipLoadRegistry"},
        {0x8363A844, kLiR3_0, kBlr, "NuipModuleInit"},
        {0x8363A9C0, kLiR3_0, kBlr, "NuipModuleTerm"},
        {0x83641650, kLiR3_0, kBlr, "NuipRegCreateKeyExW"},
        {0x83640328, kLiR3_0, kBlr, "NuipRegEnumKeyExW"},
        {0x83640420, kLiR3_0, kBlr, "NuipRegEnumValueW"},
        {0x836413E0, kLiR3_0, kBlr, "NuipRegOpenKeyExW"},
        {0x83640A58, kLiR3_0, kBlr, "NuipRegQueryValueExW"},
        {0x83641020, kLiR3_0, kBlr, "NuipRegSetValueExW"},
        {0x83640964, kLiR3_0, kBlr, "NuipUnloadRegistry"},
        {0x835D8DF8, kLiR3_0, kBlr, "NuipWaveInit"},
        {0x835D8E60, kLiR3_0, kBlr, "NuipWaveUpdate"},
    };

    // Log a stable .text fingerprint to support future resolver matching.
    Dc3TextSectionInfo text_info;
    if (auto* xex = module->xex_module()) {
      if (auto* text = xex->GetPESection(".text")) {
        auto* text_mem = memory_->TranslateVirtual<uint8_t*>(text->address);
        text_info.start = text->address;
        text_info.end = text->address + text->size;
        text_info.have_range = text->size != 0;
        if (text_mem && text->size) {
          uint64_t hash = UINT64_C(1469598103934665603);
          for (uint32_t i = 0; i < text->size; ++i) {
            hash ^= text_mem[i];
            hash *= UINT64_C(1099511628211);
          }
          text_info.fingerprint = hash;
          text_info.have_fingerprint = true;
          XELOGI("DC3: .text fingerprint addr={:08X} size=0x{:X} fnv1a64={:016X}",
                 text->address, text->size, hash);
        }
      }
    }

    // Two-pass approach: first check how many patch targets have zero-padding.
    // If many do, we're running a decomp/rebuilt XEX with different function
    // layout, and ALL patches use wrong addresses. In the original retail XEX,
    // no NUI function address would be zero-filled.
    int total_patches = static_cast<int>(sizeof(patches) / sizeof(patches[0]));
    int zero_count = 0;
    for (const auto& patch : patches) {
      auto* mem = memory_->TranslateVirtual<uint8_t*>(patch.address);
      if (mem && xe::load_and_swap<uint32_t>(mem) == 0x00000000) {
        zero_count++;
      }
    }

    bool is_decomp_layout = (zero_count > total_patches / 4);
    std::string_view layout_reason = "zero-padding heuristic";
    std::optional<Dc3NuiPatchManifest> patch_manifest;
    std::filesystem::path patch_manifest_path;
    if (!cvars::dc3_nui_patch_manifest_path.empty()) {
      patch_manifest_path = cvars::dc3_nui_patch_manifest_path;
    } else if (auto auto_manifest_path = Dc3AutoProbePatchManifestPath()) {
      patch_manifest_path = *auto_manifest_path;
    }
    const bool explicit_patch_manifest_path =
        !cvars::dc3_nui_patch_manifest_path.empty();
    if (!patch_manifest_path.empty()) {
      patch_manifest = Dc3LoadNuiPatchManifest(patch_manifest_path);
      if (patch_manifest) {
        XELOGI(
            "DC3: Loaded patch manifest '{}' (build_label={} targets={} crt={})",
            xe::path_to_utf8(patch_manifest_path), patch_manifest->build_label,
            patch_manifest->targets.size(), patch_manifest->crt_sentinels.size());
      } else {
        XELOGW("DC3: Failed to load patch manifest '{}'",
               xe::path_to_utf8(patch_manifest_path));
      }
    }
    std::optional<Dc3FingerprintCache> fingerprint_cache;
    std::filesystem::path fingerprint_cache_path;
    if (!cvars::dc3_nui_layout_fingerprint_cache_path.empty()) {
      fingerprint_cache_path = cvars::dc3_nui_layout_fingerprint_cache_path;
    } else if (auto auto_cache_path = Dc3AutoProbeFingerprintCachePath()) {
      fingerprint_cache_path = *auto_cache_path;
    }
    if (!fingerprint_cache_path.empty()) {
      fingerprint_cache = Dc3LoadFingerprintCacheFile(fingerprint_cache_path);
      if (!fingerprint_cache) {
        XELOGW("DC3: Failed to load fingerprint cache file '{}'",
               xe::path_to_utf8(fingerprint_cache_path));
      } else {
        XELOGI("DC3: Loaded fingerprint cache '{}'",
               xe::path_to_utf8(fingerprint_cache_path));
      }
    }
    if (cvars::dc3_nui_patch_layout == "original") {
      is_decomp_layout = false;
      layout_reason = "forced by --dc3_nui_patch_layout=original";
    } else if (cvars::dc3_nui_patch_layout == "decomp") {
      is_decomp_layout = true;
      layout_reason = "forced by --dc3_nui_patch_layout=decomp";
    } else if (cvars::dc3_nui_patch_layout == "auto") {
      bool matched_manifest_layout = false;
      if (patch_manifest &&
          (patch_manifest->build_label == "decomp" ||
           patch_manifest->build_label == "original")) {
        const std::optional<uint64_t> manifest_runtime_fp =
            patch_manifest->runtime_text_fingerprint;
        const std::optional<uint64_t> manifest_compare_fp =
            manifest_runtime_fp.has_value() ? manifest_runtime_fp
                                            : patch_manifest->text_fingerprint;
        if (text_info.have_fingerprint && manifest_compare_fp.has_value()) {
          if (*manifest_compare_fp == text_info.fingerprint) {
            if (patch_manifest->build_label == "decomp") {
              is_decomp_layout = true;
              layout_reason =
                  "matched patch manifest fingerprint/build_label=decomp";
            } else {
              is_decomp_layout = false;
              layout_reason =
                  "matched patch manifest fingerprint/build_label=original";
            }
            matched_manifest_layout = true;
          } else if (explicit_patch_manifest_path) {
            XELOGW(
                "DC3: Patch manifest fingerprint {:016X} != runtime .text "
                "fingerprint {:016X}; trusting explicit manifest build_label={}",
                *manifest_compare_fp, text_info.fingerprint,
                patch_manifest->build_label);
            is_decomp_layout = patch_manifest->build_label == "decomp";
            layout_reason =
                "trusted explicit patch manifest build_label (fingerprint mismatch)";
            matched_manifest_layout = true;
          }
        } else if (explicit_patch_manifest_path) {
          XELOGW(
              "DC3: Patch manifest missing comparable fingerprint; trusting "
              "explicit manifest build_label={}",
              patch_manifest->build_label);
          is_decomp_layout = patch_manifest->build_label == "decomp";
          layout_reason =
              "trusted explicit patch manifest build_label (no fingerprint)";
          matched_manifest_layout = true;
        }
      }
      uint64_t fp_original = 0;
      uint64_t fp_decomp = 0;
      bool have_fp_original = Dc3TryParseHexU64(
          cvars::dc3_nui_layout_fingerprint_original, &fp_original);
      bool have_fp_decomp = Dc3TryParseHexU64(
          cvars::dc3_nui_layout_fingerprint_decomp, &fp_decomp);
      if (!have_fp_original && fingerprint_cache &&
          fingerprint_cache->original.has_value()) {
        fp_original = *fingerprint_cache->original;
        have_fp_original = true;
      }
      if (!have_fp_decomp && fingerprint_cache &&
          fingerprint_cache->decomp.has_value()) {
        fp_decomp = *fingerprint_cache->decomp;
        have_fp_decomp = true;
      }
      if (!matched_manifest_layout && text_info.have_fingerprint && have_fp_original &&
          text_info.fingerprint == fp_original) {
        is_decomp_layout = false;
        layout_reason = "matched --dc3_nui_layout_fingerprint_original";
      } else if (!matched_manifest_layout && text_info.have_fingerprint &&
                 have_fp_decomp &&
                 text_info.fingerprint == fp_decomp) {
        is_decomp_layout = true;
        layout_reason = "matched --dc3_nui_layout_fingerprint_decomp";
      } else if (!matched_manifest_layout && text_info.have_fingerprint &&
                 (have_fp_original || have_fp_decomp)) {
        const std::string fp_original_str = have_fp_original
                                                ? fmt::format("{:016X}", fp_original)
                                                : std::string("<unset>");
        const std::string fp_decomp_str = have_fp_decomp
                                              ? fmt::format("{:016X}", fp_decomp)
                                              : std::string("<unset>");
        XELOGI(
            "DC3: .text fingerprint {:016X} did not match configured layout "
            "fingerprints (original={} decomp={})",
            text_info.fingerprint, fp_original_str, fp_decomp_str);
      }
    } else {
      XELOGW(
          "DC3: Invalid --dc3_nui_patch_layout='{}' (expected auto|original|decomp); "
          "falling back to auto heuristic",
          cvars::dc3_nui_patch_layout);
    }
    XELOGI(
        "DC3: NUI patch layout={} reason={} (zero-padding {}/{})",
        is_decomp_layout ? "decomp" : "original", layout_reason, zero_count,
        total_patches);
    dc3_is_decomp_layout = is_decomp_layout;

    // Select the appropriate patch table based on XEX layout.
    const Dc3NuiPatchSpec* active_patches =
        is_decomp_layout ? decomp_patches : patches;
    int active_count = is_decomp_layout
                           ? static_cast<int>(sizeof(decomp_patches) /
                                              sizeof(decomp_patches[0]))
                           : total_patches;

    std::optional<Dc3NuiSymbolManifest> symbol_manifest;
    std::filesystem::path symbol_manifest_path;
    if (!cvars::dc3_nui_symbol_map_path.empty()) {
      symbol_manifest_path = cvars::dc3_nui_symbol_map_path;
    } else if (auto auto_path = Dc3AutoProbeNuiSymbolMapPath()) {
      symbol_manifest_path = *auto_path;
    }
    if (!symbol_manifest_path.empty()) {
      symbol_manifest = Dc3LoadNuiSymbolManifest(symbol_manifest_path);
      if (symbol_manifest) {
        XELOGI("DC3: Loaded NUI symbol manifest '{}' ({} .text symbols)",
               xe::path_to_utf8(symbol_manifest_path),
               symbol_manifest->text_symbols.size());
      } else {
        XELOGW("DC3: Failed to load NUI symbol manifest '{}'",
               xe::path_to_utf8(symbol_manifest_path));
      }
    }

    bool use_patch_manifest_targets = patch_manifest.has_value();
    if (patch_manifest && text_info.have_fingerprint) {
      const std::optional<uint64_t> manifest_runtime_fp =
          patch_manifest->runtime_text_fingerprint;
      const std::optional<uint64_t> manifest_compare_fp =
          manifest_runtime_fp.has_value() ? manifest_runtime_fp
                                          : patch_manifest->text_fingerprint;
      if (manifest_compare_fp.has_value() &&
          *manifest_compare_fp != text_info.fingerprint) {
        XELOGW(
            "DC3: Disabling patch manifest target resolution due fingerprint "
            "mismatch (manifest {:016X} != runtime {:016X}); "
            "falling back to symbol/signature/catalog",
            *manifest_compare_fp, text_info.fingerprint);
        use_patch_manifest_targets = false;
      }
    }

    std::string resolver_mode = cvars::dc3_nui_patch_resolver_mode;
    if (resolver_mode == "legacy") {
      XELOGW("DC3: --dc3_nui_patch_resolver_mode=legacy has been removed; "
             "using hybrid");
      resolver_mode = "hybrid";
    } else if (resolver_mode != "hybrid" && resolver_mode != "strict") {
      XELOGW("DC3: Unknown --dc3_nui_patch_resolver_mode='{}'; using hybrid",
             resolver_mode);
      resolver_mode = "hybrid";
    }
    Dc3RuntimeTelemetryConfig telemetry_config;
    telemetry_config.title_id = "373307D9";
    telemetry_config.build_kind = is_decomp_layout ? "decomp" : "original";
    telemetry_config.resolver_mode = resolver_mode;
    telemetry_config.signature_resolver = cvars::dc3_nui_enable_signature_resolver;
    telemetry_config.guest_overrides = true;
    Dc3RuntimeTelemetryBeginSession(telemetry_config);
    Dc3RuntimeTelemetryRecordBootMilestone("dc3_nui_patch_block_begin");

    std::vector<Dc3ResolvedNuiPatch> resolved_patches;
    resolved_patches.reserve(active_count);
    int resolved_by_manifest = 0;
    int resolved_by_symbol = 0;
    int resolved_by_signature = 0;
    int resolved_by_catalog = 0;
    int resolver_strict_rejects = 0;
    for (int i = 0; i < active_count; ++i) {
      auto resolved = Dc3ResolveNuiPatchTarget(active_patches[i], text_info,
                                               use_patch_manifest_targets
                                                   ? &*patch_manifest
                                                   : nullptr,
                                               symbol_manifest ? &*symbol_manifest
                                                               : nullptr,
                                               resolver_mode,
                                               memory_.get(),
                                               cvars::dc3_nui_enable_signature_resolver);
      if (!resolved.resolved && resolved.strict_rejected) {
        resolver_strict_rejects++;
      } else if (resolved.resolved) {
        if (resolved.resolve_method == Dc3PatchResolveMethod::kPatchManifest) {
          resolved_by_manifest++;
        } else if (resolved.resolve_method == Dc3PatchResolveMethod::kSymbolMap) {
          resolved_by_symbol++;
        } else if (resolved.resolve_method ==
                   Dc3PatchResolveMethod::kSignatureStub) {
          resolved_by_signature++;
        } else if (resolved.resolve_method ==
                   Dc3PatchResolveMethod::kCatalogAddress) {
          resolved_by_catalog++;
        }
      }
      resolved_patches.push_back(resolved);
    }
    XELOGI(
        "DC3: NUI resolver summary mode={} manifest_hits={} symbol_hits={} "
        "signature_hits={} catalog_hits={} strict_rejects={} total={}",
        resolver_mode, resolved_by_manifest,
        resolved_by_symbol, resolved_by_signature,
        resolved_by_catalog, resolver_strict_rejects, active_count);
    Dc3RuntimeTelemetryRecordNuiResolverSummary(
        resolver_mode, resolved_by_manifest, resolved_by_symbol,
        resolved_by_signature, resolved_by_catalog, resolver_strict_rejects,
        active_count);
    if (cvars::dc3_nui_signature_trace) {
      auto should_trace_signature_target = [](std::string_view name) {
        return !name.empty();
      };
      auto log_words = [&](const char* label, uint32_t address) {
        if (!Dc3PatchTargetInText(text_info, address, 4)) {
          XELOGI("DC3: SignatureTrace {} {:08X} (outside .text)", label, address);
          return;
        }
        auto* mem = memory_->TranslateVirtual<uint8_t*>(address);
        if (!mem) {
          XELOGI("DC3: SignatureTrace {} {:08X} (unmapped)", label, address);
          return;
        }
        std::string words;
        for (int j = 0; j < 12; ++j) {
          if (!Dc3PatchTargetInText(text_info, address + j * 4, 4)) {
            break;
          }
          const uint32_t w = xe::load_and_swap<uint32_t>(mem + j * 4);
          if (!words.empty()) {
            words.push_back(' ');
          }
          words += fmt::format("{:08X}", w);
        }
        XELOGI("DC3: SignatureTrace {} {:08X}: {}", label, address, words);
      };
      for (const auto& resolved_patch : resolved_patches) {
        const auto& patch = resolved_patch.spec;
        if (!should_trace_signature_target(patch.name)) {
          continue;
        }
        XELOGI("DC3: SignatureTrace target={} resolver={} resolved={} "
               "catalog={:08X} resolved_addr={:08X}",
               patch.name, Dc3PatchResolveMethodName(resolved_patch.resolve_method),
               resolved_patch.resolved ? 1 : 0, patch.address,
               resolved_patch.resolved ? resolved_patch.resolved_address : 0);
        log_words("catalog", patch.address);
        if (resolved_patch.resolved && resolved_patch.resolved_address != patch.address) {
          log_words("resolved", resolved_patch.resolved_address);
        }
      }
    }

    const bool requested_guest_overrides = cvars::dc3_guest_overrides;
    const bool enable_guest_overrides = true;
    if (!requested_guest_overrides) {
      XELOGW(
          "DC3: DC3 NUI/XBC legacy byte-patch path has been removed; "
          "forcing guest overrides on (rollback by reverting commit)");
    }
    XELOGI("DC3: NUI/XBC apply path guest_overrides={} resolver_mode={} "
           "signature_resolver={}",
           enable_guest_overrides ? 1 : 0, resolver_mode,
           cvars::dc3_nui_enable_signature_resolver ? 1 : 0);

    processor_->ClearGuestFunctionOverrides();
    auto guest_extern_handler_for_patch =
        [&](const Dc3NuiPatchSpec& patch) -> cpu::GuestFunction::ExternHandler {
      if (!enable_guest_overrides) {
        return nullptr;
      }
      // Preserve the fake skeleton PPC stub path when enabled.
      if (cvars::fake_kinect_data && !is_decomp_layout &&
          std::string_view(patch.name) == "NuiSkeletonGetNextFrame") {
        return nullptr;
      }
      if (patch.insn1 != kBlr) {
        return nullptr;
      }
      if (patch.insn0 == kLiR3_0) {
        return Dc3NuiReturnOkExtern;
      }
      if (patch.insn0 == kLiR3_Neg1) {
        return Dc3NuiReturnNeg1Extern;
      }
      return nullptr;
    };
    auto patch_target_in_text = [&](uint32_t address) -> bool {
      return Dc3PatchTargetInText(text_info, address);
    };
    int override_registered = 0;
    int override_unsupported = 0;
    int override_register_failed = 0;
    int override_register_non_text = 0;
    int override_register_unresolved = 0;
    for (int i = 0; i < active_count; i++) {
      const auto& resolved_patch = resolved_patches[i];
      const auto& patch = resolved_patch.spec;
      if (!resolved_patch.resolved) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(unresolved target; resolver mode={})",
            patch.address, patch.name, resolver_mode);
        override_register_unresolved++;
        override_register_failed++;
        continue;
      }
      const uint32_t patch_addr = resolved_patch.resolved_address;
      auto handler = guest_extern_handler_for_patch(patch);
      if (!handler) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(unsupported patch shape for override; legacy byte patch path "
            "removed)",
            patch_addr, patch.name);
        override_unsupported++;
        override_register_failed++;
        continue;
      }
      if (!patch_target_in_text(patch_addr)) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(outside .text range {:08X}-{:08X})",
            patch_addr, patch.name, text_info.start, text_info.end);
        override_register_non_text++;
        override_register_failed++;
        continue;
      }
      auto* heap = memory_->LookupHeap(patch_addr);
      auto* mem = memory_->TranslateVirtual<uint8_t*>(patch_addr);
      if (!heap || !mem) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(invalid guest address)",
            patch_addr, patch.name);
        override_register_failed++;
        continue;
      }
      uint32_t existing0 = xe::load_and_swap<uint32_t>(mem + 0);
      if (existing0 == 0x00000000) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(zero-filled target)",
            patch_addr, patch.name);
        override_register_failed++;
        continue;
      }
      processor_->RegisterGuestFunctionOverride(patch_addr, handler,
                                                std::string(patch.name));
      XELOGI("DC3: Registered guest extern override {:08X}: {} (resolver={})",
             patch_addr, patch.name,
             Dc3PatchResolveMethodName(resolved_patch.resolve_method));
      Dc3RuntimeTelemetryRecordNuiOverrideRegistered(
          patch.name, patch_addr,
          Dc3PatchResolveMethodName(resolved_patch.resolve_method));
      override_registered++;
    }
    XELOGI(
        "DC3: Registered {} guest extern overrides from NUI patch table "
        "({} entries not overridden, {} registration failures, "
        "{} outside .text, {} unresolved)",
        override_registered, active_count - override_registered,
        override_register_failed, override_register_non_text,
        override_register_unresolved);

    const int patched = 0;
    const int overridden = override_registered;
    const int skipped = active_count - overridden;
    XELOGI(
        "DC3: NUI patch/override summary: patched={} overridden={} skipped={} "
        "total={} layout={} unsupported_override_entries={} "
        "override_registration_failures={} "
        "override_registration_non_text={} skipped_unresolved={} "
        "legacy_byte_patching_removed=1",
        patched, overridden, skipped, active_count,
        is_decomp_layout ? "decomp" : "original", override_unsupported,
        override_register_failed, override_register_non_text,
        override_register_unresolved);
    Dc3RuntimeTelemetryRecordNuiPatchSummary(
        patched, overridden, skipped, active_count,
        is_decomp_layout ? "decomp" : "original");
    Dc3RuntimeTelemetryRecordBootMilestone("dc3_nui_patch_apply_complete");

    // Fake Kinect skeleton data injection / SkeletonUpdate patches
    // are extracted into dc3_hack_pack for the same reason as the non-NUI
    // workarounds: keep emulator.cc orchestration-focused and preserve an
    // explicit retirement path.
    {
      Dc3HackContext dc3_skeleton_ctx;
      dc3_skeleton_ctx.memory = memory_.get();
      dc3_skeleton_ctx.processor = processor_.get();
      dc3_skeleton_ctx.module = module.get();
      dc3_skeleton_ctx.is_decomp_layout = is_decomp_layout;
#ifdef XE_HEADLESS_BUILD
      dc3_skeleton_ctx.is_headless = true;
#else
      dc3_skeleton_ctx.is_headless = false;
#endif
      auto skel_result = ApplyDc3SkeletonHackPack(dc3_skeleton_ctx);
      XELOGD("DC3: hack-pack category={} applied={} skipped={} failed={}",
             Dc3HackCategoryName(skel_result.category), skel_result.applied,
             skel_result.skipped, skel_result.failed);
    }
  }

  //
  // Fix CRT XapiCallThreadNotifyRoutines hang for the DC3 decomp XEX.
  //
  // The decomp's CRT has an uninitialized LIST_ENTRY at 0x83B14C3C
  // (XapiThreadNotifyRoutineList). On a real Xbox 360, this would be
  // statically initialized to point to itself (empty circular list). In
  // the decomp build, it contains garbage, causing
  // XapiCallThreadNotifyRoutines (0x82F51108) to iterate a corrupt list
  // and spin forever trying to call null callback pointers.
  //
  // Non-NUI DC3 workarounds (CRT/imports/debug/decomp runtime stopgaps) are
  // extracted into the DC3 hack pack module to keep emulator.cc orchestration-
  // only and make retirement tracking manageable.
  if (title_id_.has_value() && title_id_.value() == 0x373307D9) {
    Dc3HackContext dc3_hack_ctx;
    dc3_hack_ctx.memory = memory_.get();
    dc3_hack_ctx.processor = processor_.get();
    dc3_hack_ctx.module = module.get();
    dc3_hack_ctx.is_decomp_layout = dc3_is_decomp_layout;
#ifdef XE_HEADLESS_BUILD
    dc3_hack_ctx.is_headless = true;
#else
    dc3_hack_ctx.is_headless = false;
#endif
    auto dc3_hack_summary = ApplyDc3HackPack(dc3_hack_ctx);
    for (const auto& result : dc3_hack_summary.results) {
      XELOGD("DC3: hack-pack category={} applied={} skipped={} failed={}",
             Dc3HackCategoryName(result.category), result.applied,
             result.skipped, result.failed);
    }
    Dc3RuntimeTelemetryRecordBootMilestone("dc3_hack_pack_apply_complete");
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
