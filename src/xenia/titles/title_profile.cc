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

// Only the mitigations each title was measured to need (FORK_CLEANUP_PLAN.md
// section 3.4; evidence in docs/fork/core/TITLE_PROFILE.md). Everything else
// runs at the upstream default.

void ApplyDc3Profile(uint32_t title_id) {
  // The decomp-layout image (S3) makes null calls during boot: a defect of
  // the rebuilt image (unresolved /FORCE externs), not an emulator gap.
  ProfileSet<bool>(title_id, "tolerate_null_guest_calls", true);
  // Fires on the original layout (async reads); not yet A/B'd past
  // song_select. Its original rationale was refuted by ac0052e5b.
  ProfileSet<bool>(title_id, "io_force_synchronous_completion", true);
  // 4 MiB floor for the SkeletonUpdate worker, which the fork's automation
  // runs UI code on (was a hardcoded title-ID check in ExCreateThread).
  ProfileSet<uint32_t>(title_id, "min_guest_thread_stack_size",
                       4u * 1024u * 1024u);
  // Kinect title: report the sensor connected.
  ProfileSet<bool>(title_id, "nui_device_present", true);
}

void ApplyRb3Profile(uint32_t title_id) {
  // Needed by the fork's own RB3DX UI probe, which walks guest back chains
  // from host code and reads a fresh thread's stack_base (titles/rb3), not by
  // the game.
  ProfileSet<bool>(title_id, "soft_fault_unmapped_reads", true);
  // Retail TU5 Splash::Show calls Enter() on a null RndDir: a splash milo is
  // missing from the harness content (no TU5 update patch ark), not an
  // emulator gap.
  ProfileSet<bool>(title_id, "tolerate_null_guest_calls", true);
  // RB3Enhanced.dll is loaded without running its CRT, so its .bss critical
  // sections are never constructed. Not covered by any harness scenario.
  ProfileSet<bool>(title_id, "autoinit_critical_sections", true);
  ProfileSet<bool>(title_id, "rtl_leave_critical_section_force_release", true);
  // Turns RB3DX's post-OOM store loop into a diagnosable exit.
  ProfileSet<uint64_t>(title_id, "fault_spin_limit", 4096);
}

}  // namespace

void ApplyTitleProfile(const TitleLaunchContext& ctx) {
  if (!ctx.title_id.has_value()) {
    return;
  }
  const uint32_t title_id = ctx.title_id.value();
  switch (title_id) {
    case kTitleDc3:
      ApplyDc3Profile(title_id);
      break;
    case kTitleRb3:
      ApplyRb3Profile(title_id);
      break;
    default:
      break;
  }
}

}  // namespace titles
}  // namespace xe
