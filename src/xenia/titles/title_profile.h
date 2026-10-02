/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Per-title cvar profile (NOT upstream).
 *
 * Will set the fork's all-title mitigation cvars for the titles that need them
 * at launch, logging one line per cvar it sets, and never overriding a value
 * given on the command line (FORK_CLEANUP_PLAN.md section 3.4). EMPTY for now:
 * Phase 1 moves code only and changes no default.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_TITLE_PROFILE_H_
#define XENIA_TITLES_TITLE_PROFILE_H_

#include "xenia/titles/title_hooks.h"

namespace xe {
namespace titles {

void ApplyTitleProfile(const TitleLaunchContext& ctx);

}  // namespace titles
}  // namespace xe

#endif  // XENIA_TITLES_TITLE_PROFILE_H_
