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

DEFINE_string(
    scripted_pad_subtypes, "",
    "Comma-separated XINPUT_DEVSUBTYPE per scripted pad, e.g. \"1,8\" = pad0 "
    "gamepad, pad1 drums. Empty entries / missing pads default to 1 "
    "(XINPUT_DEVSUBTYPE_GAMEPAD). RB3 maps controllers to overshell slots by "
    "subtype (6/11=guitar, 8=drums, 15=keytar, 25=pro guitar), and only ONE "
    "slot accepts plain gamepads -- two scripted pads can only both join a "
    "band if they present as different instrument subtypes.",
    "HID");

namespace xe {
namespace hid {
namespace nop {

namespace {

std::atomic<ScriptedInputTitleAdapter*> s_title_adapter{nullptr};
std::atomic<bool> s_script_loaded{false};

// The native player's wait_screen timeout (Joypad_Native.cpp
// kWaitTimeoutFrames: 30 s at 60 fps).
constexpr int64_t kWaitTimeoutFrames = 30 * 60;

// Script trigger pseudo-buttons (XInput triggers are analog, not bits).
constexpr uint8_t kScriptLeftTrigger = 1;
constexpr uint8_t kScriptRightTrigger = 2;

uint8_t ParseTriggerName(const std::string& name) {
  std::string upper = name;
  for (auto& c : upper) c = static_cast<char>(toupper(c));
  if (upper == "L2" || upper == "LT") return kScriptLeftTrigger;
  if (upper == "R2" || upper == "RT") return kScriptRightTrigger;
  return 0;
}

}  // namespace

bool ScreenAwareScriptLoaded() {
  return s_script_loaded.load(std::memory_order_acquire);
}

void SetScriptedInputTitleAdapter(ScriptedInputTitleAdapter* adapter) {
  s_title_adapter.store(adapter, std::memory_order_release);
}


// Singleton bridge for NopInjectButtonPress (one nop driver per process in
// practice; last-created wins, cleared on destruction).
static std::atomic<NopInputDriver*> s_nop_driver_instance{nullptr};

void NopInjectButtonPress(uint32_t pad, uint16_t buttons,
                          uint64_t duration_ms) {
  NopInputDriver* driver = s_nop_driver_instance.load(std::memory_order_acquire);
  if (driver) {
    driver->InjectButtonPress(buttons, duration_ms, pad);
  }
}

NopInputDriver::NopInputDriver(xe::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {
  s_nop_driver_instance.store(this, std::memory_order_release);
}

NopInputDriver::~NopInputDriver() {
  s_nop_driver_instance.store(nullptr, std::memory_order_release);
}

X_STATUS NopInputDriver::Setup() { return X_STATUS_SUCCESS; }

static uint16_t ParseButtonName(const std::string& name) {
  // Case-insensitive matching for flexibility
  std::string upper = name;
  for (auto& c : upper) c = static_cast<char>(toupper(c));

  if (upper == "NONE" || upper == "NOOP" || upper == "IDLE") return 0;
  // The native port's names (Joypad_Native.cpp ParseButtonName), mapped to
  // the Xbox pad: confirm = A, cancel = B, option/back/select = BACK.
  if (upper == "A" || upper == "CONFIRM") return X_INPUT_GAMEPAD_A;
  if (upper == "B" || upper == "CANCEL") return X_INPUT_GAMEPAD_B;
  if (upper == "X") return X_INPUT_GAMEPAD_X;
  if (upper == "Y") return X_INPUT_GAMEPAD_Y;
  if (upper == "START") return X_INPUT_GAMEPAD_START;
  if (upper == "BACK" || upper == "OPTION" || upper == "SELECT")
    return X_INPUT_GAMEPAD_BACK;
  if (upper == "UP") return X_INPUT_GAMEPAD_DPAD_UP;
  if (upper == "DOWN") return X_INPUT_GAMEPAD_DPAD_DOWN;
  if (upper == "LEFT") return X_INPUT_GAMEPAD_DPAD_LEFT;
  if (upper == "RIGHT") return X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (upper == "LB" || upper == "L1") return X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (upper == "RB" || upper == "R1") return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (upper == "LS" || upper == "L3") return X_INPUT_GAMEPAD_LEFT_THUMB;
  if (upper == "RS" || upper == "R3") return X_INPUT_GAMEPAD_RIGHT_THUMB;
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

    // Parse optional pad-target suffix "@N" (default pad 0), e.g. "A@1".
    uint8_t pad = 0;
    size_t at = button_str.find('@');
    if (at != std::string::npos) {
      std::string pad_str = button_str.substr(at + 1);
      button_str = button_str.substr(0, at);
      try {
        int p = std::stoi(pad_str);
        if (p >= 0 && p < static_cast<int>(kMaxPads)) pad = static_cast<uint8_t>(p);
      } catch (...) {
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
      scripted_events_.push_back({time_ms, buttons, duration_ms, pad});
      XELOGI("Scripted input: {}ms button=0x{:04X} hold={}ms pad={}", time_ms,
             buttons, duration_ms, pad);
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

    // Parse "+<N> <button>" (N frames after the last satisfied wait_screen)
    // or "<N> <button>" (absolute frame N), the native port's format. A
    // trailing "# comment" is dropped, as in the native parser.
    {
      std::string body = line.substr(0, line.find('#'));
      bool relative = !body.empty() && body[0] == '+';
      size_t num_start = relative ? 1 : 0;
      size_t space = body.find_first_of(" \t", num_start);
      if (space != std::string::npos && space > num_start &&
          std::all_of(body.begin() + num_start, body.begin() + space,
                      [](char c) { return std::isdigit(uint8_t(c)) != 0; })) {
        int value = std::stoi(body.substr(num_start, space - num_start));
        std::string button_name = body.substr(space + 1);
        size_t bstart = button_name.find_first_not_of(" \t");
        size_t bend = button_name.find_last_not_of(" \t\r\n");
        button_name = bstart == std::string::npos
                          ? std::string()
                          : button_name.substr(bstart, bend - bstart + 1);
        std::string upper_button_name = button_name;
        for (auto& c : upper_button_name) c = static_cast<char>(toupper(c));

        uint16_t buttons = ParseButtonName(button_name);
        uint8_t triggers = ParseTriggerName(button_name);
        bool is_idle_hold = upper_button_name == "NONE" ||
                            upper_button_name == "NOOP" ||
                            upper_button_name == "IDLE";
        if (buttons || triggers || is_idle_hold) {
          ScriptDirective dir;
          dir.type = ScriptDirective::kDelayedPress;
          // Legacy (no frame clock) timing: N x 50 ms after the wait.
          dir.delay_ms = value * 50;
          dir.buttons = buttons;
          dir.frame = value;
          dir.relative = relative;
          dir.triggers = triggers;
          script_directives_.push_back(dir);
          XELOGI("Script[{}]: {}{} {} (0x{:04X}{})", line_num,
                 relative ? "+" : "@", value, button_name, buttons,
                 is_idle_hold ? ", no press" : "");
        } else {
          XELOGW("Script[{}]: unknown button '{}'", line_num, button_name);
        }
        continue;
      }
    }

    XELOGW("Script[{}]: unrecognized directive: {}", line_num, line);
  }

  screen_aware_mode_ = !script_directives_.empty();
  scripted_mode_ = true;  // Enable controller presence
  frame_mode_ = -1;
  frame_waiting_ = false;
  frame_wait_start_ = -1;
  frame_wait_satisfied_ = -1;
  frame_last_eval_ = -1;
  frame_buttons_ = 0;
  frame_triggers_ = 0;
  s_script_loaded.store(screen_aware_mode_, std::memory_order_release);
  XELOGI("Screen-aware script loaded: {} directives from {}",
         script_directives_.size(), path);
}

void NopInputDriver::InjectButtonPress(uint16_t buttons, uint64_t duration_ms,
                                       uint32_t pad) {
  if (pad >= kMaxPads) {
    return;
  }
  std::lock_guard<std::mutex> lock(inject_mutex_);
  InjectedEvent ev;
  ev.start = std::chrono::steady_clock::now();
  ev.duration_ms = duration_ms;
  ev.buttons = buttons;
  ev.pad = static_cast<uint8_t>(pad);
  injected_events_.push_back(ev);
}

std::string NopInputDriver::ReadCurrentScreenName() const {
  if (!memory_) return "";
  if (auto* adapter = s_title_adapter.load(std::memory_order_acquire)) {
    return adapter->ReadCurrentScreenName(memory_);
  }
  return "";
}

void NopInputDriver::PollTitleAdapter() {
  if (!memory_) {
    return;
  }

  auto now = std::chrono::steady_clock::now();
  auto since_last_read = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - last_screen_read_time_)
                             .count();
  if (since_last_read >= 100 || last_screen_name_.empty()) {
    std::string current_screen = ReadCurrentScreenName();
    if (!current_screen.empty()) {
      last_screen_name_ = current_screen;
    }
    last_screen_read_time_ = now;
  }

  if (auto* adapter = s_title_adapter.load(std::memory_order_acquire)) {
    adapter->OnPrimaryPadPoll(memory_, last_screen_name_);
  }
}

uint16_t NopInputDriver::GetScreenAwareButtons() {
  if (!screen_aware_mode_ || script_index_ >= script_directives_.size()) {
    frame_triggers_ = 0;
    return 0;
  }
  auto* frame_adapter = s_title_adapter.load(std::memory_order_acquire);
  if (frame_mode_ < 0 && frame_adapter) {
    frame_mode_ = frame_adapter->HasFrameClock() ? 1 : 0;
    XELOGI("Script: {} timing", frame_mode_
                                    ? "guest frame clock (native port semantics)"
                                    : "legacy wall-clock (+N = N x 50 ms)");
  }
  if (frame_mode_ == 1 && frame_adapter) {
    return GetFrameScriptButtons(frame_adapter);
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
    auto* adapter = s_title_adapter.load(std::memory_order_acquire);
    auto since_last_wait_log =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - s_last_wait_log)
            .count();
    if (since_last_wait_log >= 2000) {
      if (adapter) {
        adapter->LogWaitStatus(memory_, dir.screen_name, last_screen_name_);
      } else {
        XELOGI("Script: waiting for '{}' current='{}'", dir.screen_name,
               last_screen_name_);
      }
      s_last_wait_log = now;
    }
    if (!wait_satisfied_) {
      if (adapter) {
        active |= adapter->WhileWaitingForScreen(memory_, dir.screen_name,
                                                 &last_screen_name_);
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

uint16_t NopInputDriver::GetFrameScriptButtons(
    ScriptedInputTitleAdapter* adapter) {
  int64_t frame = adapter->FrameNumber();
  if (frame < 0) {
    return 0;  // The clock has not ticked yet.
  }
  if (frame != frame_last_eval_) {
    // Evaluate every frame since the last evaluation, as the native player
    // does once per JoypadPoll. Frames the guest did not poll the pad in are
    // still stepped (waits and timeouts advance); their presses are delivered
    // on this frame instead of being lost.
    int64_t from = frame_last_eval_ < 0 ? frame : frame_last_eval_ + 1;
    if (frame < from) {
      from = frame;
    }
    uint16_t buttons = 0;
    uint8_t triggers = 0;
    for (int64_t f = from; f <= frame; ++f) {
      buttons |= StepFrameScript(adapter, f, &triggers);
    }
    frame_buttons_ = buttons;
    frame_triggers_ = triggers;
    frame_last_eval_ = frame;
  }
  uint16_t active = frame_buttons_;
  if (frame_waiting_ && script_index_ < script_directives_.size()) {
    auto& dir = script_directives_[script_index_];
    // Title automation while a wait is pending (e.g. DC3's attract press,
    // only with --dc3_headless_autonav).
    active |= adapter->WhileWaitingForScreen(memory_, dir.screen_name,
                                             &last_screen_name_);
    auto now = std::chrono::steady_clock::now();
    if (now - frame_last_log_ >= std::chrono::seconds(2)) {
      frame_last_log_ = now;
      adapter->LogWaitStatus(memory_, dir.screen_name, last_screen_name_);
    }
  }
  if (script_index_ >= script_directives_.size()) {
    XELOGI("DC3 Script: ALL DIRECTIVES COMPLETE (frame {})", frame);
    screen_aware_mode_ = false;
  }
  return active;
}

uint16_t NopInputDriver::StepFrameScript(ScriptedInputTitleAdapter* adapter,
                                         int64_t frame, uint8_t* triggers) {
  // Joypad_Native.cpp GetScriptedButtons(currentFrame), line for line.
  uint16_t buttons = 0;
  while (script_index_ < script_directives_.size()) {
    auto& d = script_directives_[script_index_];
    if (d.type == ScriptDirective::kWaitScreen) {
      if (!frame_waiting_) {
        frame_waiting_ = true;
        frame_wait_start_ = frame;
      }
      std::string screen = ReadCurrentScreenName();
      if (!screen.empty()) {
        last_screen_name_ = screen;
      }
      bool satisfied = !screen.empty() && !adapter->InTransition(memory_) &&
                       screen == d.screen_name;
      if (satisfied) {
        frame_wait_satisfied_ = frame;
        frame_waiting_ = false;
        wait_satisfied_ = true;
        XELOGI("DC3 Script: wait_screen '{}' SATISFIED (current: '{}', "
               "frame {})",
               d.screen_name, screen, frame);
        ++script_index_;
        continue;
      }
      if (frame - frame_wait_start_ > kWaitTimeoutFrames) {
        XELOGW("DC3 Script: wait_screen '{}' TIMEOUT (current: '{}', {} "
               "frames)",
               d.screen_name, screen, frame - frame_wait_start_);
        frame_waiting_ = false;
        frame_wait_satisfied_ = frame;
        ++script_index_;
        continue;
      }
      break;  // Still waiting.
    }
    // kDelayedPress
    int64_t target = d.relative ? (frame_wait_satisfied_ >= 0
                                       ? frame_wait_satisfied_ + d.frame
                                       : d.frame)
                                : d.frame;
    if (frame == target) {
      buttons |= d.buttons;
      *triggers |= d.triggers;
      if (d.buttons || d.triggers) {
        XELOGI("DC3 Script: pressing 0x{:04X} at frame {} (+{}, screen "
               "'{}', directive {})",
               d.buttons, frame, frame - frame_wait_satisfied_,
               last_screen_name_, script_index_);
      }
      ++script_index_;
      continue;
    } else if (frame > target) {
      XELOGW("DC3 Script: directive {} (frame {}) already passed at frame {}; "
             "skipped",
             script_index_, target, frame);
      ++script_index_;
      continue;
    }
    break;  // A future press.
  }
  return buttons;
}

X_RESULT NopInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!scripted_mode_ || user_index >= kMaxPads) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Report a wired pad on this port. Subtype defaults to plain gamepad but is
  // overridable per pad via --scripted_pad_subtypes (RB3 slot-maps by subtype;
  // see the cvar help).
  static uint8_t s_pad_subtypes[kMaxPads] = {0};
  static bool s_subtypes_parsed = false;
  if (!s_subtypes_parsed) {
    s_subtypes_parsed = true;
    for (uint32_t i = 0; i < kMaxPads; ++i) s_pad_subtypes[i] = 0x01;
    std::stringstream ss(cvars::scripted_pad_subtypes);
    std::string item;
    for (uint32_t i = 0; i < kMaxPads && std::getline(ss, item, ','); ++i) {
      int v = item.empty() ? 1 : std::atoi(item.c_str());
      if (v > 0 && v < 256) s_pad_subtypes[i] = static_cast<uint8_t>(v);
    }
  }
  std::memset(reinterpret_cast<void*>(out_caps), 0, sizeof(*out_caps));
  out_caps->type = 0x01;  // XINPUT_DEVTYPE_GAMEPAD
  out_caps->sub_type = s_pad_subtypes[user_index];
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

uint16_t NopInputDriver::GetCurrentButtons(uint32_t pad) {
  // The title adapter poll + screen-aware nav + dynamic injection are all
  // global (single-driver) state; only evaluate them once, on the primary pad.
  if (pad == 0) {
    PollTitleAdapter();
  }

  uint16_t active_buttons = 0;

  // Time-based scripted events (filtered by target pad)
  if (!scripted_events_.empty()) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - start_time_)
                          .count();

    for (const auto& event : scripted_events_) {
      if (event.pad != pad) continue;
      if (elapsed_ms >= (int64_t)event.time_ms &&
          elapsed_ms < (int64_t)(event.time_ms + event.duration_ms)) {
        active_buttons |= event.buttons;
      }
    }
  }

  if (pad == 0) {
    // Screen-aware scripted events (DC3 nav, primary pad only)
    if (screen_aware_mode_) {
      active_buttons |= GetScreenAwareButtons();
    }
  }

  {
    // Dynamic injected events (per-pad)
    std::lock_guard<std::mutex> lock(inject_mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = injected_events_.begin(); it != injected_events_.end();) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - it->start)
                         .count();
      if (elapsed >= (int64_t)it->duration_ms) {
        it = injected_events_.erase(it);
        continue;
      }
      if (it->pad == pad) {
        active_buttons |= it->buttons;
      }
      ++it;
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

void NopInputDriver::QueueKeystrokeEdges(uint32_t user_index,
                                         uint16_t active_buttons) {
  // Generate keystroke events for button transitions since the last poll.
  uint16_t pressed = active_buttons & ~prev_buttons_[user_index];
  uint16_t released = prev_buttons_[user_index] & ~active_buttons;
  for (uint16_t bit = 1; bit != 0; bit <<= 1) {
    if (pressed & bit) {
      X_INPUT_KEYSTROKE ks = {};
      ks.virtual_key = ButtonToVK(bit);
      ks.flags = X_INPUT_KEYSTROKE_KEYDOWN;
      ks.user_index = static_cast<uint8_t>(user_index);
      if (ks.virtual_key) {
        keystroke_queue_[user_index].push_back(ks);
        XELOGI("Keystroke KEYDOWN: VK=0x{:04X} button=0x{:04X} pad={}",
               (uint16_t)ks.virtual_key, bit, user_index);
      }
    }
    if (released & bit) {
      X_INPUT_KEYSTROKE ks = {};
      ks.virtual_key = ButtonToVK(bit);
      ks.flags = X_INPUT_KEYSTROKE_KEYUP;
      ks.user_index = static_cast<uint8_t>(user_index);
      if (ks.virtual_key) {
        keystroke_queue_[user_index].push_back(ks);
      }
    }
  }
  prev_buttons_[user_index] = active_buttons;
}

X_RESULT NopInputDriver::GetState(uint32_t user_index,
                                  X_INPUT_STATE* out_state) {
  if (!scripted_mode_ || user_index >= kMaxPads) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint16_t active_buttons = GetCurrentButtons(user_index);
  QueueKeystrokeEdges(user_index, active_buttons);

  std::memset(reinterpret_cast<void*>(out_state), 0, sizeof(*out_state));
  out_state->packet_number = packet_number_++;
  out_state->gamepad.buttons = active_buttons;
  if (user_index == 0 && frame_mode_ == 1) {
    out_state->gamepad.left_trigger =
        (frame_triggers_ & kScriptLeftTrigger) ? 0xFF : 0;
    out_state->gamepad.right_trigger =
        (frame_triggers_ & kScriptRightTrigger) ? 0xFF : 0;
  }

  return X_ERROR_SUCCESS;
}

X_RESULT NopInputDriver::SetState(uint32_t user_index,
                                  X_INPUT_VIBRATION* vibration) {
  if (!scripted_mode_ || user_index >= kMaxPads) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT NopInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  if (!scripted_mode_ || user_index >= kMaxPads) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Poll current state to generate any pending keystroke events
  // (in case GetKeystroke is called without GetState)
  uint16_t active_buttons = GetCurrentButtons(user_index);
  QueueKeystrokeEdges(user_index, active_buttons);

  if (!keystroke_queue_[user_index].empty()) {
    *out_keystroke = keystroke_queue_[user_index].front();
    keystroke_queue_[user_index].pop_front();
    return X_ERROR_SUCCESS;
  }

  std::memset(reinterpret_cast<void*>(out_keystroke), 0, sizeof(*out_keystroke));
  return X_ERROR_EMPTY;
}

}  // namespace nop
}  // namespace hid
}  // namespace xe
