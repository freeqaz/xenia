/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914) scripted-input title adapter (NOT upstream).
 *
 * Gives the nop input driver's `wait_screen` directives the RB3 screen name.
 * Replaces the RB3 reader hid/nop used to fall back to for every title.
 ******************************************************************************
 */

#include <string>

#include "xenia/hid/nop/nop_input_driver.h"
#include "xenia/titles/rb3/rb3_guest.h"
#include "xenia/titles/rb3/rb3_internal.h"

namespace xe {
namespace titles {
namespace rb3 {

namespace {

class Rb3ScriptedInputAdapter final
    : public hid::nop::ScriptedInputTitleAdapter {
 public:
  // TheBandUI (0x82DFD2B0, the object itself) -> mCurrentScreen @+0x2C ->
  // name @+0x18; "" unless the name is a clean identifier.
  std::string ReadCurrentScreenName(Memory* memory) override {
    return rb3::ReadCurrentScreenName(memory);
  }
};

Rb3ScriptedInputAdapter g_adapter;

}  // namespace

void InstallScriptedInputAdapter() {
  hid::nop::SetScriptedInputTitleAdapter(&g_adapter);
}

void RemoveScriptedInputAdapter() {
  hid::nop::SetScriptedInputTitleAdapter(nullptr);
}

}  // namespace rb3
}  // namespace titles
}  // namespace xe
