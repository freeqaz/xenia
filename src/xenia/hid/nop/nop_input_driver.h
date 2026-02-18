/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_NOP_NOP_INPUT_DRIVER_H_
#define XENIA_HID_NOP_NOP_INPUT_DRIVER_H_

#include <chrono>
#include <string>
#include <vector>

#include "xenia/hid/input_driver.h"

namespace xe {
namespace hid {
namespace nop {

class NopInputDriver final : public InputDriver {
 public:
  explicit NopInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~NopInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  // Enable scripted input mode: simulate a connected controller with
  // timed button presses. Input script format: "5s:A,7s:START,10s:A"
  void SetScriptedInput(const std::string& script);

 private:
  struct ScriptedEvent {
    uint64_t time_ms;      // Milliseconds after start
    uint16_t buttons;      // Button flags to press
    uint64_t duration_ms;  // How long to hold (default 200ms)
  };

  bool scripted_mode_ = false;
  std::vector<ScriptedEvent> scripted_events_;
  std::chrono::steady_clock::time_point start_time_;
  uint32_t packet_number_ = 0;
};

}  // namespace nop
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_NOP_NOP_INPUT_DRIVER_H_
