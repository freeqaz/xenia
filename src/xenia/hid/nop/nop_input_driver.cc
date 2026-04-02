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
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/hid/hid_flags.h"
#include "xenia/hid/input.h"
#include "xenia/memory.h"
#include "xenia/ui/virtual_key.h"

namespace xe {
namespace hid {
namespace nop {

namespace {

constexpr uint32_t kDc3OriginalXexNameMin = 0x82000000;
constexpr uint32_t kDc3OriginalXexNameMax = 0x82400000;

std::string ReadGuestScreenName(Memory* memory, uint32_t screen_ptr,
                                bool scan_only) {
  if (!memory || !screen_ptr || screen_ptr >= 0xF0000000) {
    return "";
  }
  auto* scr_obj = memory->TranslateVirtual<uint8_t*>(screen_ptr);
  if (!scr_obj) {
    return "";
  }

  auto read_guest_name = [&](uint32_t name_ptr) -> std::string {
    if (!name_ptr || name_ptr >= 0xF0000000) {
      return "";
    }
    if (scan_only &&
        (name_ptr < kDc3OriginalXexNameMin || name_ptr >= kDc3OriginalXexNameMax)) {
      return "";
    }

    std::string result;
    result.reserve(32);
    for (uint32_t i = 0; i < 64; ++i) {
      auto* ch_ptr = memory->TranslateVirtual<uint8_t*>(name_ptr + i);
      if (!ch_ptr) {
        return "";
      }
      char ch = static_cast<char>(*ch_ptr);
      if (!ch) {
        return result;
      }
      unsigned char uch = static_cast<unsigned char>(ch);
      if (!(std::isalnum(uch) || ch == '_')) {
        return "";
      }
      result.push_back(ch);
    }
    return "";
  };

  for (uint32_t offset : {0x1C, 0x20}) {
    uint32_t name_ptr = xe::load_and_swap<uint32_t>(scr_obj + offset);
    std::string name = read_guest_name(name_ptr);
    if (!name.empty()) {
      return name;
    }
  }
  return "";
}

}  // namespace

NopInputDriver::NopInputDriver(xe::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

NopInputDriver::~NopInputDriver() = default;

X_STATUS NopInputDriver::Setup() { return X_STATUS_SUCCESS; }

static uint16_t ParseButtonName(const std::string& name) {
  // Case-insensitive matching for flexibility
  std::string upper = name;
  for (auto& c : upper) c = static_cast<char>(toupper(c));

  if (upper == "NONE" || upper == "NOOP" || upper == "IDLE") return 0;
  if (upper == "A" || upper == "CONFIRM") return X_INPUT_GAMEPAD_A;
  if (upper == "B") return X_INPUT_GAMEPAD_B;
  if (upper == "X") return X_INPUT_GAMEPAD_X;
  if (upper == "Y") return X_INPUT_GAMEPAD_Y;
  if (upper == "START") return X_INPUT_GAMEPAD_START;
  if (upper == "BACK") return X_INPUT_GAMEPAD_BACK;
  if (upper == "UP") return X_INPUT_GAMEPAD_DPAD_UP;
  if (upper == "DOWN") return X_INPUT_GAMEPAD_DPAD_DOWN;
  if (upper == "LEFT") return X_INPUT_GAMEPAD_DPAD_LEFT;
  if (upper == "RIGHT") return X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (upper == "LB") return X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (upper == "RB") return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (upper == "LS") return X_INPUT_GAMEPAD_LEFT_THUMB;
  if (upper == "RS") return X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (upper == "GUIDE") return X_INPUT_GAMEPAD_GUIDE;
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

void NopInputDriver::LoadScriptFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    XELOGE("Failed to open script file: {}", path);
    return;
  }

  script_directives_.clear();
  script_index_ = 0;
  wait_satisfied_ = false;
  last_screen_read_time_ = std::chrono::steady_clock::now();

  std::string line;
  int line_num = 0;
  while (std::getline(file, line)) {
    line_num++;
    // Trim whitespace
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) continue;
    line = line.substr(start);

    // Skip comments and empty lines
    if (line.empty() || line[0] == '#') continue;

    // Parse "wait_screen <name>" or "wait <name>"
    bool is_wait_screen = (line.size() > 12 && line.substr(0, 12) == "wait_screen ");
    bool is_wait = (!is_wait_screen && line.size() > 5 && line.substr(0, 5) == "wait ");

    if (is_wait_screen || is_wait) {
      std::string screen = is_wait_screen ? line.substr(12) : line.substr(5);
      // Trim trailing whitespace
      size_t end = screen.find_last_not_of(" \t\r\n");
      if (end != std::string::npos) screen = screen.substr(0, end + 1);

      ScriptDirective dir;
      dir.type = ScriptDirective::kWaitScreen;
      dir.screen_name = screen;
      dir.delay_ms = 0;
      dir.buttons = 0;
      script_directives_.push_back(dir);
      XELOGI("Script[{}]: wait_screen '{}'", line_num, screen);
      continue;
    }

    // Parse "+<N> <button>" where N is treated as a delay value.
    // The +N values from native scripts are frame counts at 60fps.
    // Convert to milliseconds: N frames * 16.67ms/frame.
    // For Xenia which runs slower, we use a larger multiplier.
    if (line[0] == '+') {
      size_t space = line.find(' ');
      if (space == std::string::npos) continue;

      int value = std::stoi(line.substr(1, space - 1));
      std::string button_name = line.substr(space + 1);
      // Trim
      size_t end = button_name.find_last_not_of(" \t\r\n");
      if (end != std::string::npos) button_name = button_name.substr(0, end + 1);
      std::string upper_button_name = button_name;
      for (auto& c : upper_button_name) c = static_cast<char>(toupper(c));

      // Convert frame-count-style values to milliseconds.
      // Native port at 60fps: +30 = 500ms. Xenia runs slower, so
      // use a generous multiplier: N * 50ms (allows for ~20fps effective).
      int delay_ms = value * 50;

      uint16_t buttons = ParseButtonName(button_name);
      bool is_idle_hold = upper_button_name == "NONE" ||
                          upper_button_name == "NOOP" ||
                          upper_button_name == "IDLE";
      if (buttons || is_idle_hold) {
        ScriptDirective dir;
        dir.type = ScriptDirective::kDelayedPress;
        dir.delay_ms = delay_ms;
        dir.buttons = buttons;
        script_directives_.push_back(dir);
        if (buttons) {
          XELOGI("Script[{}]: +{}ms {} (0x{:04X})", line_num, delay_ms,
                 button_name, buttons);
        } else {
          XELOGI("Script[{}]: +{}ms {} (idle hold)", line_num, delay_ms,
                 button_name);
        }
      }
      continue;
    }

    XELOGW("Script[{}]: unrecognized directive: {}", line_num, line);
  }

  screen_aware_mode_ = !script_directives_.empty();
  scripted_mode_ = true;  // Enable controller presence
  XELOGI("Screen-aware script loaded: {} directives from {}",
         script_directives_.size(), path);
}

void NopInputDriver::InjectButtonPress(uint16_t buttons, uint64_t duration_ms) {
  std::lock_guard<std::mutex> lock(inject_mutex_);
  InjectedEvent ev;
  ev.start = std::chrono::steady_clock::now();
  ev.duration_ms = duration_ms;
  ev.buttons = buttons;
  injected_events_.push_back(ev);
}

std::string NopInputDriver::ReadCurrentScreenName() const {
  if (!memory_) return "";

  constexpr uint32_t kTheUI = 0x82F1A8E0;
  auto* ui_ptr = memory_->TranslateVirtual<uint8_t*>(kTheUI);
  if (!ui_ptr) return "";

  uint32_t ui_addr = xe::load_and_swap<uint32_t>(ui_ptr);
  if (!ui_addr || ui_addr >= 0xF0000000) return "";

  auto* ui_obj = memory_->TranslateVirtual<uint8_t*>(ui_addr);
  if (!ui_obj) return "";

  // mCurrentScreen at offset 0x48
  uint32_t cur_screen = xe::load_and_swap<uint32_t>(ui_obj + 0x48);
  if (!cur_screen || cur_screen >= 0xF0000000) return "";

  // Original debug XEX screen objects have been observed with mName at +0x1C,
  // while some earlier experiments assumed +0x20. Try both to keep
  // screen-aware scripts working across layouts.
  return ReadGuestScreenName(memory_, cur_screen, false);
}

uint16_t NopInputDriver::GetScreenAwareButtons() {
  if (!screen_aware_mode_ || script_index_ >= script_directives_.size()) {
    return 0;
  }

  auto now = std::chrono::steady_clock::now();

  // Read current screen name at most every 100ms to reduce overhead
  auto since_last_read = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - last_screen_read_time_)
                             .count();
  if (since_last_read >= 100) {
    last_screen_name_ = ReadCurrentScreenName();
    last_screen_read_time_ = now;
  }

  uint16_t active = 0;
  auto& dir = script_directives_[script_index_];

  if (dir.type == ScriptDirective::kWaitScreen) {
    static auto s_last_wait_log = std::chrono::steady_clock::time_point{};
    static uint32_t s_last_stuck_transition = 0;
    static auto s_stuck_transition_start = std::chrono::steady_clock::time_point{};
    static auto s_attract_seen_since = std::chrono::steady_clock::time_point{};
    static auto s_last_attract_press = std::chrono::steady_clock::time_point{};
    static uint32_t s_title_screen_addr = 0;
    auto since_last_wait_log =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - s_last_wait_log)
            .count();
    if (since_last_wait_log >= 2000) {
      constexpr uint32_t kTheUI = 0x82F1A8E0;
      uint32_t ui_addr = 0;
      uint32_t cur_screen = 0;
      uint32_t trans_screen = 0;
      uint32_t trans_state = 0;
      std::string name_1c;
      std::string trans_name_1c;
      if (memory_) {
        auto* ui_ptr = memory_->TranslateVirtual<uint8_t*>(kTheUI);
        ui_addr = ui_ptr ? xe::load_and_swap<uint32_t>(ui_ptr) : 0;
        if (ui_addr && ui_addr < 0xF0000000) {
          auto* ui_obj = memory_->TranslateVirtual<uint8_t*>(ui_addr);
          if (ui_obj) {
            trans_state = xe::load_and_swap<uint32_t>(ui_obj + 0x2C);
            cur_screen = xe::load_and_swap<uint32_t>(ui_obj + 0x48);
            trans_screen = xe::load_and_swap<uint32_t>(ui_obj + 0x4C);
            name_1c = ReadGuestScreenName(memory_, cur_screen, false);
            if (name_1c.empty() && cur_screen) {
              name_1c = "<unnamed>";
            }
            trans_name_1c = ReadGuestScreenName(memory_, trans_screen, false);
            if (trans_name_1c.empty() && trans_screen) {
              trans_name_1c = "<unnamed>";
            }

            // Original-XEX headless can get stuck before the first screen is
            // promoted from mTransitionScreen into mCurrentScreen. If the same
            // transition target sits active for several seconds, force-complete
            // it so screen-aware automation can continue.
            if (!cur_screen && trans_screen && trans_state != 0) {
              if (s_last_stuck_transition != trans_screen) {
                s_last_stuck_transition = trans_screen;
                s_stuck_transition_start = now;
              } else {
                auto stuck_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - s_stuck_transition_start)
                        .count();
                if (stuck_ms >= 4000) {
                  xe::store_and_swap<uint32_t>(ui_obj + 0x48, trans_screen);
                  xe::store_and_swap<uint32_t>(ui_obj + 0x4C, 0);
                  xe::store_and_swap<uint32_t>(ui_obj + 0x2C, 0);
                  cur_screen = trans_screen;
                  trans_screen = 0;
                  trans_state = 0;
                  name_1c = trans_name_1c;
                  trans_name_1c.clear();
                  XELOGI("DC3 Script: force-completed stuck UI transition "
                         "to {:08X} ('{}') after {}ms",
                         cur_screen, name_1c, stuck_ms);
                  s_last_stuck_transition = 0;
                  s_stuck_transition_start = std::chrono::steady_clock::time_point{};
                }
              }
            } else {
              s_last_stuck_transition = 0;
              s_stuck_transition_start = std::chrono::steady_clock::time_point{};
            }
          }
        }
      }
      XELOGI("DC3 Script: waiting for '{}' current='{}' ui={:08X} "
             "cur={:08X} trans={:08X} transState={} "
             "name='{}' trans='{}'",
             dir.screen_name, last_screen_name_, ui_addr, cur_screen,
             trans_screen, trans_state, name_1c, trans_name_1c);
      s_last_wait_log = now;
    }
    if (!wait_satisfied_) {
      if (dir.screen_name == "title_screen" &&
          last_screen_name_ == "attract_screen") {
        if (s_attract_seen_since == std::chrono::steady_clock::time_point{}) {
          s_attract_seen_since = now;
        }
        auto attract_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - s_attract_seen_since)
                .count();
        auto since_last_press =
            s_last_attract_press == std::chrono::steady_clock::time_point{}
                ? INT64_MAX
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - s_last_attract_press)
                      .count();
        if (attract_ms >= 1500 && since_last_press >= 3000) {
          active |= X_INPUT_GAMEPAD_A;
          s_last_attract_press = now;
          XELOGI("DC3 Script: attract-screen fallback press A while waiting "
                 "for title_screen");
        }
        if (memory_ && attract_ms >= 5000) {
          constexpr uint32_t kTheUI = 0x82F1A8E0;
          auto* ui_ptr = memory_->TranslateVirtual<uint8_t*>(kTheUI);
          uint32_t ui_addr = ui_ptr ? xe::load_and_swap<uint32_t>(ui_ptr) : 0;
          auto* ui_obj = (ui_addr && ui_addr < 0xF0000000)
                             ? memory_->TranslateVirtual<uint8_t*>(ui_addr)
                             : nullptr;
          if (ui_obj) {
            if (!s_title_screen_addr) {
              for (uint32_t addr = 0x40C00000; addr < 0x41000000;
                   addr += 4) {
                if (!memory_->TranslateVirtual<uint8_t*>(addr)) {
                  continue;
                }
                std::string name = ReadGuestScreenName(memory_, addr, true);
                if (name == "title_screen" || name == "title") {
                  s_title_screen_addr = addr;
                  XELOGI("DC3 Script: resolved title screen object {:08X} "
                         "via name '{}'",
                         s_title_screen_addr, name);
                  break;
                }
                if (s_title_screen_addr) {
                  break;
                }
              }
            }
            if (s_title_screen_addr) {
              xe::store_and_swap<uint32_t>(ui_obj + 0x48, s_title_screen_addr);
              xe::store_and_swap<uint32_t>(ui_obj + 0x4C, 0);
              xe::store_and_swap<uint32_t>(ui_obj + 0x2C, 0);
              last_screen_name_ = "title_screen";
              XELOGI("DC3 Script: forced UI jump attract_screen -> title_screen "
                     "({:08X})", s_title_screen_addr);
            }
          }
        }
      } else {
        s_attract_seen_since = std::chrono::steady_clock::time_point{};
      }

      // Check if current screen matches
      if (!last_screen_name_.empty() && last_screen_name_ == dir.screen_name) {
        wait_satisfied_ = true;
        wait_satisfied_time_ = now;
        XELOGI("DC3 Script: wait_screen '{}' SATISFIED (current: '{}')",
               dir.screen_name, last_screen_name_);
        // Advance past the wait_screen directive
        script_index_++;
      }
    }
  }

  if (wait_satisfied_ && script_index_ < script_directives_.size()) {
    auto& cur = script_directives_[script_index_];

    if (cur.type == ScriptDirective::kDelayedPress) {
      auto ms_since_wait =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - wait_satisfied_time_)
              .count();

      // Button hold: active for 350ms after the delay fires
      constexpr int64_t kHoldMs = 350;

      if (ms_since_wait >= cur.delay_ms &&
          ms_since_wait < cur.delay_ms + kHoldMs) {
        active |= cur.buttons;

        // Log on first detection of press window
        static int64_t s_last_logged_delay = -1;
        static size_t s_last_logged_index = SIZE_MAX;
        if (s_last_logged_index != script_index_ ||
            s_last_logged_delay != cur.delay_ms) {
          s_last_logged_delay = cur.delay_ms;
          s_last_logged_index = script_index_;
          if (cur.buttons) {
            XELOGI("DC3 Script: pressing 0x{:04X} at +{}ms "
                   "(screen '{}', directive {})",
                   cur.buttons, cur.delay_ms, last_screen_name_,
                   script_index_);
          } else {
            XELOGI("DC3 Script: idle hold at +{}ms (screen '{}', directive {})",
                   cur.delay_ms, last_screen_name_, script_index_);
          }
        }
      }

      // After hold period + gap, advance to next directive
      constexpr int64_t kGapMs = 150;  // Gap between consecutive presses
      if (ms_since_wait >= cur.delay_ms + kHoldMs + kGapMs) {
        script_index_++;

        // If next directive is another wait_screen, reset wait state
        if (script_index_ < script_directives_.size() &&
            script_directives_[script_index_].type ==
                ScriptDirective::kWaitScreen) {
          wait_satisfied_ = false;
          XELOGI("DC3 Script: next wait_screen '{}' (directive {})",
                 script_directives_[script_index_].screen_name,
                 script_index_);
        }
      }
    } else if (cur.type == ScriptDirective::kWaitScreen) {
      // We hit another wait_screen — reset and wait
      wait_satisfied_ = false;
    }
  }

  // Check if script is complete
  if (script_index_ >= script_directives_.size()) {
    XELOGI("DC3 Script: ALL DIRECTIVES COMPLETE");
    screen_aware_mode_ = false;  // Script finished
  }

  return active;
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

uint16_t NopInputDriver::GetCurrentButtons() {
  uint16_t active_buttons = 0;

  // Time-based scripted events
  if (!scripted_events_.empty()) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - start_time_)
                          .count();

    for (const auto& event : scripted_events_) {
      if (elapsed_ms >= (int64_t)event.time_ms &&
          elapsed_ms < (int64_t)(event.time_ms + event.duration_ms)) {
        active_buttons |= event.buttons;
      }
    }
  }

  // Screen-aware scripted events
  if (screen_aware_mode_) {
    active_buttons |= GetScreenAwareButtons();
  }

  // Dynamic injected events
  {
    std::lock_guard<std::mutex> lock(inject_mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = injected_events_.begin(); it != injected_events_.end();) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - it->start)
                         .count();
      if (elapsed < (int64_t)it->duration_ms) {
        active_buttons |= it->buttons;
        ++it;
      } else {
        it = injected_events_.erase(it);
      }
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
