/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Per-title cvar profile (NOT upstream). See title_profile.h.
 ******************************************************************************
 */

#include "xenia/titles/title_profile.h"

#include <cstdint>
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/titles/title_ids.h"

namespace xe {
namespace titles {

namespace {

// Sets one cvar for this title, below the user's explicit choices: a value
// given on the command line or in the title's own game config
// (config/<TITLEID>.config.toml) wins. The value is stored as the GAME-config
// layer, so it is never written back to the global config file and cannot leak
// into the next title launched with that file.
template <typename T>
void ProfileSet(uint32_t title_id, const char* name, T value) {
  cvar::ConfigVar<T>* var = nullptr;
  if (cvar::ConfigVars) {
    auto it = cvar::ConfigVars->find(name);
    if (it != cvar::ConfigVars->end()) {
      var = dynamic_cast<cvar::ConfigVar<T>*>(it->second);
    }
  }
  if (!var) {
    // A typo or a type mismatch here would silently leave the title on the
    // global default, so say so loudly.
    XELOGE("TITLE-PROFILE {:08X}: unknown cvar '{}' (or wrong type); not set",
           title_id, name);
    return;
  }
  if (var->has_commandline_value()) {
    XELOGI("TITLE-PROFILE {:08X}: {}={} kept (command line)", title_id, name,
           *var->current_value());
    return;
  }
  if (var->has_game_config_value()) {
    XELOGI("TITLE-PROFILE {:08X}: {}={} kept (game config)", title_id, name,
           *var->current_value());
    return;
  }
  var->SetGameConfigValue(value);
  XELOGI("TITLE-PROFILE {:08X}: {}={}", title_id, name, *var->current_value());
}

// The fork's all-title mitigations (FORK_CLEANUP_PLAN.md section 3.4), at the
// values DC3 and RB3 were measured under. Each entry is a title requirement
// until an A/B shows otherwise; see docs/fork/core/TITLE_PROFILE.md.
void ApplyMitigationProfile(uint32_t title_id) {
  ProfileSet<bool>(title_id, "soft_fault_unmapped_reads", true);
  ProfileSet<bool>(title_id, "tolerate_null_guest_calls", true);
  ProfileSet<bool>(title_id, "scanner_stop_on_invalid_run", true);
  ProfileSet<bool>(title_id, "io_force_synchronous_completion", true);
  ProfileSet<bool>(title_id, "autoinit_critical_sections", true);
  ProfileSet<bool>(title_id, "rtl_leave_critical_section_force_release", true);
  ProfileSet<uint64_t>(title_id, "fault_spin_limit", 4096);
  ProfileSet<bool>(title_id, "xam_enum_overlapped_nomorefiles_success", true);
}

}  // namespace

void ApplyTitleProfile(const TitleLaunchContext& ctx) {
  if (!ctx.title_id.has_value()) {
    return;
  }
  const uint32_t title_id = ctx.title_id.value();
  switch (title_id) {
    case kTitleDc3:
      ApplyMitigationProfile(title_id);
      // 4 MiB floor for the SkeletonUpdate worker (was a hardcoded title-ID
      // check in ExCreateThread).
      ProfileSet<uint32_t>(title_id, "min_guest_thread_stack_size",
                           4u * 1024u * 1024u);
      break;
    case kTitleRb3:
      ApplyMitigationProfile(title_id);
      break;
    default:
      break;
  }
}

}  // namespace titles
}  // namespace xe
