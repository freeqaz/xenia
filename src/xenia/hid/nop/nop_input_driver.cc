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
#include "xenia/ui/virtual_key.h"

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

uint16_t NopInputDriver::GetCurrentButtons() const {
  auto now = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - start_time_)
                        .count();

  uint16_t active_buttons = 0;
  for (const auto& event : scripted_events_) {
    if (elapsed_ms >= (int64_t)event.time_ms &&
        elapsed_ms < (int64_t)(event.time_ms + event.duration_ms)) {
      active_buttons |= event.buttons;
    }
  }
  return active_buttons;
}

uint16_t NopInputDriver::ButtonToVK(uint16_t button) const {
  switch (button) {
    case X_INPUT_GAMEPAD_A:
      return uint16_t(ui::VirtualKey::kXInputPadA);
    case X_INPUT_GAMEPAD_B:
      return uint16_t(ui::VirtualKey::kXInputPadB);
    case X_INPUT_GAMEPAD_X:
      return uint16_t(ui::VirtualKey::kXInputPadX);
    case X_INPUT_GAMEPAD_Y:
      return uint16_t(ui::VirtualKey::kXInputPadY);
    case X_INPUT_GAMEPAD_START:
      return uint16_t(ui::VirtualKey::kXInputPadStart);
    case X_INPUT_GAMEPAD_BACK:
      return uint16_t(ui::VirtualKey::kXInputPadBack);
    case X_INPUT_GAMEPAD_DPAD_UP:
      return uint16_t(ui::VirtualKey::kXInputPadDpadUp);
    case X_INPUT_GAMEPAD_DPAD_DOWN:
      return uint16_t(ui::VirtualKey::kXInputPadDpadDown);
    case X_INPUT_GAMEPAD_DPAD_LEFT:
      return uint16_t(ui::VirtualKey::kXInputPadDpadLeft);
    case X_INPUT_GAMEPAD_DPAD_RIGHT:
      return uint16_t(ui::VirtualKey::kXInputPadDpadRight);
    case X_INPUT_GAMEPAD_LEFT_SHOULDER:
      return uint16_t(ui::VirtualKey::kXInputPadLShoulder);
    case X_INPUT_GAMEPAD_RIGHT_SHOULDER:
      return uint16_t(ui::VirtualKey::kXInputPadRShoulder);
    case X_INPUT_GAMEPAD_LEFT_THUMB:
      return uint16_t(ui::VirtualKey::kXInputPadLThumbPress);
    case X_INPUT_GAMEPAD_RIGHT_THUMB:
      return uint16_t(ui::VirtualKey::kXInputPadRThumbPress);
    default:
      return 0;
  }
}

X_RESULT NopInputDriver::GetState(uint32_t user_index,
                                  X_INPUT_STATE* out_state) {
  if (!scripted_mode_ || user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint16_t active_buttons = GetCurrentButtons();

  // Generate keystroke events for button transitions
  uint16_t pressed = active_buttons & ~prev_buttons_;
  uint16_t released = prev_buttons_ & ~active_buttons;

  // Check each button bit for transitions
  for (uint16_t bit = 1; bit != 0; bit <<= 1) {
    if (pressed & bit) {
      X_INPUT_KEYSTROKE ks = {};
      ks.virtual_key = ButtonToVK(bit);
      ks.flags = X_INPUT_KEYSTROKE_KEYDOWN;
      ks.user_index = 0;
      if (ks.virtual_key) {
        keystroke_queue_.push_back(ks);
        XELOGI("Keystroke KEYDOWN: VK=0x{:04X} button=0x{:04X}",
               (uint16_t)ks.virtual_key, bit);
      }
    }
    if (released & bit) {
      X_INPUT_KEYSTROKE ks = {};
      ks.virtual_key = ButtonToVK(bit);
      ks.flags = X_INPUT_KEYSTROKE_KEYUP;
      ks.user_index = 0;
      if (ks.virtual_key) {
        keystroke_queue_.push_back(ks);
      }
    }
  }
  prev_buttons_ = active_buttons;

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

  // Poll current state to generate any pending keystroke events
  // (in case GetKeystroke is called without GetState)
  uint16_t active_buttons = GetCurrentButtons();
  uint16_t pressed = active_buttons & ~prev_buttons_;
  uint16_t released = prev_buttons_ & ~active_buttons;
  for (uint16_t bit = 1; bit != 0; bit <<= 1) {
    if (pressed & bit) {
      X_INPUT_KEYSTROKE ks = {};
      ks.virtual_key = ButtonToVK(bit);
      ks.flags = X_INPUT_KEYSTROKE_KEYDOWN;
      ks.user_index = 0;
      if (ks.virtual_key) {
        keystroke_queue_.push_back(ks);
        XELOGI("Keystroke KEYDOWN: VK=0x{:04X} button=0x{:04X}",
               (uint16_t)ks.virtual_key, bit);
      }
    }
    if (released & bit) {
      X_INPUT_KEYSTROKE ks = {};
      ks.virtual_key = ButtonToVK(bit);
      ks.flags = X_INPUT_KEYSTROKE_KEYUP;
      ks.user_index = 0;
      if (ks.virtual_key) keystroke_queue_.push_back(ks);
    }
  }
  prev_buttons_ = active_buttons;

  if (!keystroke_queue_.empty()) {
    *out_keystroke = keystroke_queue_.front();
    keystroke_queue_.pop_front();
    return X_ERROR_SUCCESS;
  }

  std::memset(reinterpret_cast<void*>(out_keystroke), 0, sizeof(*out_keystroke));
  return X_ERROR_EMPTY;
}

}  // namespace nop
}  // namespace hid
}  // namespace xe
