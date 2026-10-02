/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 scripted-input title adapter (NOT upstream).
 *
 * The DC3 half of the old hid/nop/nop_input_driver.cc: the UIManager screen
 * reader the `wait_screen` directives use, the read-only gameplay probe whose
 * `gpState=` lines the harness counts, and the harness automation that runs
 * from the pad poll (input.* hacks in dc3_hacks.h).
 ******************************************************************************
 */

#ifndef XENIA_TITLES_DC3_DC3_SCRIPTED_INPUT_H_
#define XENIA_TITLES_DC3_DC3_SCRIPTED_INPUT_H_

namespace xe {
namespace dc3 {

// Installs the DC3 adapter into the nop input driver's scripted-input player.
void InstallScriptedInputAdapter();

}  // namespace dc3
}  // namespace xe

#endif  // XENIA_TITLES_DC3_DC3_SCRIPTED_INPUT_H_
