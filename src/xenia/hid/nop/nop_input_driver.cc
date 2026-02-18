/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/nop/nop_input_driver.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "xenia/base/logging.h"
#include "xenia/hid/hid_flags.h"
#include "xenia/hid/input.h"

namespace xe {
namespace hid {
namespace nop {

NopInputDriver::NopInputDriver(xe::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

NopInputDriver::~NopInputDriver() = default;

X_STATUS NopInputDriver::Setup() { return X_STATUS_SUCCESS; }

static uint16_t ParseButtonName(const std::string& name) {
  if (name == "A") return X_INPUT_GAMEPAD_A;
  if (name == "B") return X_INPUT_GAMEPAD_B;
  if (name == "X") return X_INPUT_GAMEPAD_X;
  if (name == "Y") return X_INPUT_GAMEPAD_Y;
  if (name == "START") return X_INPUT_GAMEPAD_START;
  if (name == "BACK") return X_INPUT_GAMEPAD_BACK;
  if (name == "UP") return X_INPUT_GAMEPAD_DPAD_UP;
  if (name == "DOWN") return X_INPUT_GAMEPAD_DPAD_DOWN;
  if (name == "LEFT") return X_INPUT_GAMEPAD_DPAD_LEFT;
  if (name == "RIGHT") return X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (name == "LB") return X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (name == "RB") return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (name == "LS") return X_INPUT_GAMEPAD_LEFT_THUMB;
  if (name == "RS") return X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (name == "GUIDE") return X_INPUT_GAMEPAD_GUIDE;
  return 0;
}

void NopInputDriver::SetScriptedInput(const std::string& script) {
  // Parse script format: "5s:A,7s:START,10s:A" or "5000ms:A,7000ms:START"
  // Each event: <time><unit>:<button>[:<duration><unit>]
  // Default duration: 200ms
  scripted_events_.clear();

  std::istringstream ss(script);
  std::string token;
  while (std::getline(ss, token, ',')) {
    // Trim whitespace
    size_t start = token.find_first_not_of(" \t");
    if (start == std::string::npos) continue;
    token = token.substr(start);

    // Parse time
    size_t colon = token.find(':');
    if (colon == std::string::npos) continue;

    std::string time_str = token.substr(0, colon);
    std::string rest = token.substr(colon + 1);

    uint64_t time_ms = 0;
    if (time_str.size() > 2 && time_str.substr(time_str.size() - 2) == "ms") {
      time_ms = std::stoull(time_str.substr(0, time_str.size() - 2));
    } else if (time_str.size() > 1 && time_str.back() == 's') {
      time_ms = std::stoull(time_str.substr(0, time_str.size() - 1)) * 1000;
    } else {
      time_ms = std::stoull(time_str);  // Assume milliseconds
    }

    // Parse button and optional duration
    uint64_t duration_ms = 200;  // Default hold time
    std::string button_str = rest;
    size_t colon2 = rest.find(':');
    if (colon2 != std::string::npos) {
      button_str = rest.substr(0, colon2);
      std::string dur_str = rest.substr(colon2 + 1);
      if (dur_str.size() > 2 && dur_str.substr(dur_str.size() - 2) == "ms") {
        duration_ms = std::stoull(dur_str.substr(0, dur_str.size() - 2));
      } else if (dur_str.size() > 1 && dur_str.back() == 's') {
        duration_ms =
            std::stoull(dur_str.substr(0, dur_str.size() - 1)) * 1000;
      } else {
        duration_ms = std::stoull(dur_str);
      }
    }

    // Parse button (support + for combos like "A+START")
    uint16_t buttons = 0;
    std::istringstream btn_ss(button_str);
    std::string btn;
    while (std::getline(btn_ss, btn, '+')) {
      buttons |= ParseButtonName(btn);
    }

    if (buttons) {
      scripted_events_.push_back({time_ms, buttons, duration_ms});
      XELOGI("Scripted input: {}ms button=0x{:04X} hold={}ms", time_ms,
             buttons, duration_ms);
    }
  }

  // Sort events by time
  std::sort(scripted_events_.begin(), scripted_events_.end(),
            [](const ScriptedEvent& a, const ScriptedEvent& b) {
              return a.time_ms < b.time_ms;
            });

  scripted_mode_ = !scripted_events_.empty();
  if (scripted_mode_) {
    start_time_ = std::chrono::steady_clock::now();
    XELOGI("Scripted input enabled with {} events", scripted_events_.size());
  }
}

X_RESULT NopInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!scripted_mode_ || user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Report a standard wired gamepad on port 0
  std::memset(reinterpret_cast<void*>(out_caps), 0, sizeof(*out_caps));
  out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  out_caps->flags = 0;
  out_caps->gamepad.buttons = 0xFFFF;  // All buttons supported
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = (int16_t)0x7FFF;
  out_caps->gamepad.thumb_ly = (int16_t)0x7FFF;
  out_caps->gamepad.thumb_rx = (int16_t)0x7FFF;
  out_caps->gamepad.thumb_ry = (int16_t)0x7FFF;
  out_caps->vibration.left_motor_speed = 0xFFFF;
  out_caps->vibration.right_motor_speed = 0xFFFF;

  return X_ERROR_SUCCESS;
}

X_RESULT NopInputDriver::GetState(uint32_t user_index,
                                  X_INPUT_STATE* out_state) {
  if (!scripted_mode_ || user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  auto now = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - start_time_)
                        .count();

  // Compute combined button state from all active events
  uint16_t active_buttons = 0;
  for (const auto& event : scripted_events_) {
    if (elapsed_ms >= (int64_t)event.time_ms &&
        elapsed_ms < (int64_t)(event.time_ms + event.duration_ms)) {
      active_buttons |= event.buttons;
    }
  }

  std::memset(reinterpret_cast<void*>(out_state), 0, sizeof(*out_state));
  out_state->packet_number = packet_number_++;
  out_state->gamepad.buttons = active_buttons;

  return X_ERROR_SUCCESS;
}

X_RESULT NopInputDriver::SetState(uint32_t user_index,
                                  X_INPUT_VIBRATION* vibration) {
  if (!scripted_mode_ || user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT NopInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  if (!scripted_mode_ || user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Return empty keystroke (no key events). Games primarily use GetState.
  std::memset(reinterpret_cast<void*>(out_keystroke), 0, sizeof(*out_keystroke));
  return X_ERROR_EMPTY;
}

}  // namespace nop
}  // namespace hid
}  // namespace xe
