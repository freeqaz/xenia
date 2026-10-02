/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Per-title hook registry (NOT upstream).
 *
 * The emulator core calls the functions below at fixed points of a title's
 * lifetime. They do nothing unless a title module registered itself, and only
 * xenia-headless registers any (app/xenia_headless_main.cc), so the windowed
 * xenia-app links no title code. Title modules live in src/xenia/titles/<t>/
 * and are built as their own premake projects.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_TITLE_HOOKS_H_
#define XENIA_TITLES_TITLE_HOOKS_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace xe {
class Memory;
namespace cpu {
class Processor;
}  // namespace cpu
namespace kernel {
class KernelState;
class UserModule;
}  // namespace kernel
namespace vfs {
class VirtualFileSystem;
}  // namespace vfs

namespace titles {

// What Emulator::CompleteLaunch hands a title module: the module is loaded and
// the title ID is known, the shader storage is initialized, and the title's
// main thread has not been created yet (LaunchModule runs right after).
struct TitleLaunchContext {
  Memory* memory = nullptr;
  cpu::Processor* processor = nullptr;
  kernel::KernelState* kernel_state = nullptr;
  kernel::UserModule* module = nullptr;
  std::optional<uint32_t> title_id;
  std::filesystem::path content_root;
  // True in the headless build (XE_HEADLESS_BUILD), where there is no display
  // window.
  bool headless = false;
};

// One title module. Every callback is optional and is called for EVERY title:
// the module checks the title ID itself.
struct TitleHooks {
  const char* name = "";
  // Emulator::CompleteLaunch, before LaunchModule.
  void (*apply_launch_hooks)(const TitleLaunchContext& ctx) = nullptr;
  // Emulator::LaunchXexFile, after game:/d: are linked to mount_path.
  void (*on_xex_mounted)(vfs::VirtualFileSystem* file_system,
                         const std::string& mount_path) = nullptr;
  // Emulator::LaunchPath, before the title is launched.
  void (*on_launch_path)() = nullptr;
  // Emulator::TerminateTitle, before the guest function overrides are cleared
  // and the kernel terminates the title.
  void (*on_terminate_title)() = nullptr;
  // Emulator::~Emulator, before any subsystem is shut down.
  void (*on_shutdown)() = nullptr;
};

// Call before the emulator starts. Hooks run in registration order.
void RegisterTitleHooks(const TitleHooks& hooks);

// Applies the per-title cvar profile (title_profile.h), then every module's
// apply_launch_hooks.
void ApplyLaunchHooks(const TitleLaunchContext& ctx);
void OnXexMounted(vfs::VirtualFileSystem* file_system,
                  const std::string& mount_path);
void OnLaunchPath();
void OnTerminateTitle();
void OnShutdown();

}  // namespace titles
}  // namespace xe

#endif  // XENIA_TITLES_TITLE_HOOKS_H_
