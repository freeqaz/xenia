/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Per-title hook registry (NOT upstream). See title_hooks.h.
 ******************************************************************************
 */

#include "xenia/titles/title_hooks.h"

#include <vector>

#include "xenia/titles/title_profile.h"

namespace xe {
namespace titles {

namespace {
std::vector<TitleHooks>& Registry() {
  static std::vector<TitleHooks> registry;
  return registry;
}
}  // namespace

void RegisterTitleHooks(const TitleHooks& hooks) {
  Registry().push_back(hooks);
}

void ApplyLaunchHooks(const TitleLaunchContext& ctx) {
  ApplyTitleProfile(ctx);
  for (const auto& hooks : Registry()) {
    if (hooks.apply_launch_hooks) {
      hooks.apply_launch_hooks(ctx);
    }
  }
}

void OnXexMounted(vfs::VirtualFileSystem* file_system,
                  const std::string& mount_path) {
  for (const auto& hooks : Registry()) {
    if (hooks.on_xex_mounted) {
      hooks.on_xex_mounted(file_system, mount_path);
    }
  }
}

void OnLaunchPath() {
  for (const auto& hooks : Registry()) {
    if (hooks.on_launch_path) {
      hooks.on_launch_path();
    }
  }
}

void OnTerminateTitle() {
  for (const auto& hooks : Registry()) {
    if (hooks.on_terminate_title) {
      hooks.on_terminate_title();
    }
  }
}

void OnShutdown() {
  for (const auto& hooks : Registry()) {
    if (hooks.on_shutdown) {
      hooks.on_shutdown();
    }
  }
}

}  // namespace titles
}  // namespace xe
