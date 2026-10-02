/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 headless autonav (NOT upstream). See dc3_autonav.h.
 *
 * What is left of the host menu automation is one pad input: the attract
 * A-press in the scripted-input adapter (input.attract_press,
 * dc3_scripted_input.cc), armed by --dc3_headless_autonav.
 *
 * (RETIRED 2026-10-02, lane B2) seq.nav_bridge: a main-thread task that
 * called UIManager::GotoScreen(attract -> title -> wait_main -> main ->
 * choose_mode -> song_select -> multiuser -> loading -> game_screen) whenever
 * a screen sat idle, held loading -> game_screen while the song FileMerger
 * merge was busy (merge_busy), and fell back to stomping UIManager's screen
 * pointers (--dc3_game_screen_real_goto=false). Every jump skipped the
 * screen's own select handler: title -> wait_main without title_panel's
 * NAV_SELECT_MSG leaves $post_load_dest_screen unset ("Data 0 is not String
 * (file ui/title/title.dta, line 261)", a latched main-thread FAIL), and
 * multiuser -> loading skipped start_game / enter_gameplay. The flow file now
 * drives every screen with the native port's semantics
 * (docs/fork/dc3/BASELINE.md, "Flow").
 ******************************************************************************
 */

#include "xenia/titles/dc3/dc3_autonav.h"

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DEFINE_bool(dc3_headless_autonav, false,
            "DC3 (original debug.xex): headless harness input. Arms the "
            "scripted-input attract A-press (input.attract_press): A every "
            "3 s while attract_screen blocks a `wait_screen title_screen` "
            "(the attract movie plays for real and has no other way out). "
            "Off: only the flow file and the DTA channel drive the game.",
            "DC3");

namespace xe {
namespace dc3 {

bool AutonavEnabled() { return cvars::dc3_headless_autonav; }

void InstallAutonav(cpu::Processor* processor, Memory* memory,
                    kernel::KernelState* kernel_state) {
  XELOGI("DC3: headless autonav {} (attract A-press only; the nav bridge is "
         "retired)",
         cvars::dc3_headless_autonav ? "on" : "off");
}

}  // namespace dc3
}  // namespace xe
