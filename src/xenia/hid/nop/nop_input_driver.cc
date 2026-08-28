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

constexpr uint32_t kDc3OriginalXexNameMin = 0x82000000;
constexpr uint32_t kDc3OriginalXexNameMax = 0x82400000;

bool IsGuestReadable(Memory* memory, uint32_t guest_addr, uint32_t size) {
  if (!memory || !guest_addr || guest_addr >= 0xF0000000 || !size) {
    return false;
  }
  uint32_t guest_end = guest_addr + size - 1;
  if (guest_end < guest_addr) {
    return false;
  }
  auto* heap = memory->LookupHeap(guest_addr);
  if (!heap) {
    return false;
  }
  return heap->QueryRangeAccess(guest_addr, guest_end) !=
         xe::memory::PageAccess::kNoAccess;
}

std::string ReadGuestScreenName(Memory* memory, uint32_t screen_ptr,
                                bool strict_scan_range) {
  if (!memory || !screen_ptr || screen_ptr >= 0xF0000000) {
    return "";
  }
  if (!IsGuestReadable(memory, screen_ptr + 0x20, 4)) {
    return "";
  }
  auto* scr_obj = memory->TranslateVirtual<uint8_t*>(screen_ptr);

  auto read_guest_name = [&](uint32_t name_ptr) -> std::string {
    if (!name_ptr || name_ptr >= 0xF0000000) {
      return "";
    }
    if (strict_scan_range &&
        (name_ptr < kDc3OriginalXexNameMin || name_ptr >= kDc3OriginalXexNameMax)) {
      return "";
    }

    std::string result;
    result.reserve(32);
    for (uint32_t i = 0; i < 64; ++i) {
      if (!IsGuestReadable(memory, name_ptr + i, 1)) {
        return "";
      }
      auto* ch_ptr = memory->TranslateVirtual<uint8_t*>(name_ptr + i);
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

// RB3 (TU5, title 0x45410914) screen-name read. The RB3 UI singleton is a
// BandUI at a FIXED address (TheBandUI = 0x82DFD2B0 -- the object itself, not
// a pointer to it; the frame loop's TheUI pointer global 0x82C721F0 is
// statically initialized to it), with mCurrentScreen @ +0x2C and the screen's
// name pointer @ +0x18. This layout is unrelated to DC3's (a pointer global at
// 0x82F1A8E0, mCurrentScreen @ +0x48, name @ +0x1C/+0x20), so screen-aware
// scripts need both readers. Returns "" if the layout does not read plausibly,
// which is how the caller decides which game it is looking at.
std::string NopInputDriver::ReadRb3ScreenName() const {
  if (!memory_) return "";
  constexpr uint32_t kTheBandUI = 0x82DFD2B0;
  if (!IsGuestReadable(memory_, kTheBandUI + 0x30, 4)) return "";
  auto* ui_obj = memory_->TranslateVirtual<uint8_t*>(kTheBandUI);
  uint32_t cur_screen = xe::load_and_swap<uint32_t>(ui_obj + 0x2C);
  if (!cur_screen || cur_screen >= 0xF0000000) return "";
  if (!IsGuestReadable(memory_, cur_screen + 0x18, 4)) return "";
  auto* scr = memory_->TranslateVirtual<uint8_t*>(cur_screen);
  uint32_t name_ptr = xe::load_and_swap<uint32_t>(scr + 0x18);
  if (!name_ptr || name_ptr >= 0xF0000000) return "";
  std::string result;
  for (uint32_t i = 0; i < 64; ++i) {
    if (!IsGuestReadable(memory_, name_ptr + i, 1)) return "";
    char ch = static_cast<char>(*memory_->TranslateVirtual<uint8_t*>(name_ptr + i));
    if (!ch) return result;
    unsigned char uch = static_cast<unsigned char>(ch);
    if (!(std::isalnum(uch) || ch == '_')) return "";
    result.push_back(ch);
  }
  return "";
}

std::string NopInputDriver::ReadCurrentScreenName() const {
  if (!memory_) return "";

  // RB3 first: its read is strictly validated (fixed object address, name must
  // be a clean identifier), so a hit is unambiguous and a miss costs two loads.
  // Record which layout answered: RB3 and DC3 share screen names ("game_screen"
  // exists in both) but NOT addresses, so the DC3 gameplay poking below must
  // never fire on an RB3 name.
  std::string rb3 = ReadRb3ScreenName();
  if (!rb3.empty()) {
    screen_name_is_rb3_ = true;
    return rb3;
  }
  screen_name_is_rb3_ = false;

  constexpr uint32_t kTheUI = 0x82F1A8E0;
  auto* ui_ptr =
      IsGuestReadable(memory_, kTheUI, 4)
          ? memory_->TranslateVirtual<uint8_t*>(kTheUI)
          : nullptr;
  if (!ui_ptr) return "";

  uint32_t ui_addr = xe::load_and_swap<uint32_t>(ui_ptr);
  if (!IsGuestReadable(memory_, ui_addr, 0x50)) return "";

  auto* ui_obj = memory_->TranslateVirtual<uint8_t*>(ui_addr);

  // mCurrentScreen at offset 0x48
  uint32_t cur_screen = xe::load_and_swap<uint32_t>(ui_obj + 0x48);
  if (!cur_screen || cur_screen >= 0xF0000000) return "";

  // Original debug XEX screen objects have been observed with mName at +0x1C,
  // while some earlier experiments assumed +0x20. Try both to keep
  // screen-aware scripts working across layouts.
  return ReadGuestScreenName(memory_, cur_screen, false);
}

void NopInputDriver::UpdateDc3HostBeatDrive() {
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

  // RB3 also has a screen literally named "game_screen"; everything below this
  // point addresses DC3 singletons by absolute guest address and WRITES to
  // some of them, so it must not run for RB3.
  if (last_screen_name_ != "game_screen" || screen_name_is_rb3_) {
    if (dc3_host_beat_drive_active_) {
      XELOGI("DC3 Script: host beat drive deactivated on '{}'",
             last_screen_name_);
      dc3_host_beat_drive_active_ = false;
    }
    return;
  }

  constexpr uint32_t kTheTaskMgr = 0x82F64A58;
  constexpr uint32_t kTimelineStride = 0x1C;
  constexpr uint32_t kTimeOff = 0x10;
  constexpr uint32_t kLastTimeOff = 0x14;

  auto load_u32 = [&](uint32_t guest_addr) -> uint32_t {
    auto* ptr = IsGuestReadable(memory_, guest_addr, 4)
                    ? memory_->TranslateVirtual<uint8_t*>(guest_addr)
                    : nullptr;
    return ptr ? xe::load_and_swap<uint32_t>(ptr) : 0;
  };
  auto load_float = [&](uint32_t guest_addr) -> float {
    auto* ptr = IsGuestReadable(memory_, guest_addr, 4)
                    ? memory_->TranslateVirtual<uint8_t*>(guest_addr)
                    : nullptr;
    return ptr ? xe::load_and_swap<float>(ptr) : 0.0f;
  };
  auto store_float = [&](uint32_t guest_addr, float value) -> bool {
    if (!IsGuestReadable(memory_, guest_addr, 4)) {
      return false;
    }
    auto* ptr = memory_->TranslateVirtual<uint8_t*>(guest_addr);
    if (!ptr) {
      return false;
    }
    xe::store_and_swap<float>(ptr, value);
    return true;
  };

  // Blocker 2 (gameplay crash): force-advancing the song clock while the Game
  // is still paused/loading (mPaused==1) drives the gameplay pipeline over a
  // not-ready audio stream and crashes (the HamAudio resync / Voice path). Only
  // run the host beat drive once the Game's own load/wait state machine has
  // started playback (Game::PostWaitStart sets mPaused=0).
  // TheGamePanel(0x83117410)->mGame(+0x38)->Game.mPaused(+0x5E).
  {
    constexpr uint32_t kTheGamePanelGate = 0x83117410;
    uint32_t gp_gate = load_u32(kTheGamePanelGate);
    uint32_t game_gate =
        (gp_gate && IsGuestReadable(memory_, gp_gate + 0x38, 4))
            ? load_u32(gp_gate + 0x38)
            : 0;
    bool paused = true;
    if (game_gate && IsGuestReadable(memory_, game_gate + 0x5E, 1)) {
      auto* pp = memory_->TranslateVirtual<uint8_t*>(game_gate + 0x5E);
      paused = pp ? (*pp != 0) : true;
    }
    if (!game_gate || paused) {
      if (dc3_host_beat_drive_active_) {
        dc3_host_beat_drive_active_ = false;
        XELOGI("DC3 Script: beat gate closed (Game paused/not ready)");
      }
      // Keep probing so we can watch the load/wait/paused progression.
      ProbeDc3GameplayState();
      return;
    }
  }

  uint32_t timelines_addr = load_u32(kTheTaskMgr + 0x2C);
  if (!timelines_addr || !IsGuestReadable(memory_, timelines_addr + 0x54, 4) ||
      !IsGuestReadable(memory_, kTheTaskMgr + 0x48, 1)) {
    return;
  }

  auto* auto_ptr = memory_->TranslateVirtual<uint8_t*>(kTheTaskMgr + 0x48);
  if (auto_ptr) {
    *auto_ptr = 0;
  }

  uint32_t seconds_time_addr = timelines_addr + 0 * kTimelineStride + kTimeOff;
  uint32_t seconds_last_addr =
      timelines_addr + 0 * kTimelineStride + kLastTimeOff;
  uint32_t beats_time_addr = timelines_addr + 1 * kTimelineStride + kTimeOff;
  uint32_t beats_last_addr =
      timelines_addr + 1 * kTimelineStride + kLastTimeOff;
  uint32_t ui_time_addr = timelines_addr + 2 * kTimelineStride + kTimeOff;
  uint32_t ui_last_addr =
      timelines_addr + 2 * kTimelineStride + kLastTimeOff;

  float old_seconds = load_float(seconds_time_addr);
  float old_beats = load_float(beats_time_addr);
  float old_ui = load_float(ui_time_addr);

  if (!dc3_host_beat_drive_active_) {
    dc3_host_song_seconds_ = old_seconds;
    dc3_host_song_beat_ = old_beats;
    dc3_host_last_update_time_ = now;
    dc3_host_last_log_time_ = now;
    dc3_host_beat_drive_active_ = true;
    XELOGI(
        "DC3 Script: host beat drive activated taskmgr={:08X} timelines={:08X} "
        "sec={:.3f} beat={:.3f}",
        kTheTaskMgr, timelines_addr, dc3_host_song_seconds_,
        dc3_host_song_beat_);
    ProbeDc3GameplayState();
    return;
  }

  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - dc3_host_last_update_time_)
                        .count();
  if (elapsed_ms <= 0) {
    return;
  }
  if (elapsed_ms > 100) {
    elapsed_ms = 33;
  }
  dc3_host_last_update_time_ = now;

  float delta_seconds = static_cast<float>(elapsed_ms) / 1000.0f;
  float delta_beats = delta_seconds * (120.0f / 60.0f);
  dc3_host_song_seconds_ += delta_seconds;
  dc3_host_song_beat_ += delta_beats;

  store_float(seconds_last_addr, old_seconds);
  store_float(seconds_time_addr, dc3_host_song_seconds_);
  store_float(beats_last_addr, old_beats);
  store_float(beats_time_addr, dc3_host_song_beat_);
  store_float(ui_last_addr, old_ui);
  store_float(ui_time_addr, dc3_host_song_seconds_);

  auto since_last_log = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - dc3_host_last_log_time_)
                            .count();
  if (since_last_log >= 2000) {
    dc3_host_last_log_time_ = now;
    XELOGI("DC3 Script: host beat drive sec={:.3f} beat={:.3f}",
           dc3_host_song_seconds_, dc3_host_song_beat_);
  }

  ProbeDc3GameplayState();
}

void NopInputDriver::ProbeDc3GameplayState() {
  if (!memory_ || last_screen_name_ != "game_screen" || screen_name_is_rb3_) {
    return;
  }

  auto now = std::chrono::steady_clock::now();
  auto load_u32 = [&](uint32_t guest_addr) -> uint32_t {
    auto* ptr = IsGuestReadable(memory_, guest_addr, 4)
                    ? memory_->TranslateVirtual<uint8_t*>(guest_addr)
                    : nullptr;
    return ptr ? xe::load_and_swap<uint32_t>(ptr) : 0;
  };
  auto load_u8 = [&](uint32_t guest_addr) -> uint8_t {
    auto* ptr = IsGuestReadable(memory_, guest_addr, 1)
                    ? memory_->TranslateVirtual<uint8_t*>(guest_addr)
                    : nullptr;
    return ptr ? *ptr : 0;
  };

  constexpr uint32_t kTheGamePanel = 0x83117410;
  uint32_t game_panel_addr = load_u32(kTheGamePanel);
  uint32_t game_addr = 0;
  int game_panel_state = -1;
  int game_load_state = -1;
  int game_wait_state = -1;
  bool game_paused = false;
  bool game_time_paused = false;
  bool game_real_time = false;
  bool game_has_intro = false;

  if (game_panel_addr && IsGuestReadable(memory_, game_panel_addr + 0x83, 1)) {
    game_addr = load_u32(game_panel_addr + 0x38);
    game_panel_state =
        static_cast<int>(load_u32(game_panel_addr + 0x80));
  }

  if (game_addr && IsGuestReadable(memory_, game_addr + 0xA7, 1)) {
    game_paused = load_u8(game_addr + 0x5E) != 0;
    game_time_paused = load_u8(game_addr + 0x5F) != 0;
    game_real_time = load_u8(game_addr + 0x60) != 0;
    game_has_intro = load_u8(game_addr + 0x62) != 0;
    game_load_state = static_cast<int>(load_u32(game_addr + 0x90));
    game_wait_state = static_cast<int>(load_u32(game_addr + 0xA4));
  }

  // Blocker 2 (unpause deadlock): headless, HamAudio never reaches IsReady, so
  // Game::PostWaitStart never fires and mPaused stays 1 forever -- the game
  // cannot self-unpause (the HX_NATIVE audio-fail wall-clock fallback is
  // compiled out of debug.xex). Once the stable stuck state (load=3 wait=3
  // paused=1) is observed on game_screen, force the unpause ourselves: the safe
  // host analogue of the native audio-fail fallback. Verified offsets (DC3
  // Game.h + binary): game+0xA4 mWaitState, gp+0xF8 unkf8 (Game::Poll
  // clock-clobber gate), game+0x60 mRealTime, game+0x5E mPaused. ORDER: clear
  // wait (HandleWait then returns without touching the not-ready audio stream) +
  // unkf8=0 (stop Poll re-clobbering the host-driven TaskMgr clock) + realTime=1
  // FIRST, then mPaused=0 LAST so the host beat-drive gate opens only after the
  // clobbers are disabled. Fires once.
  static bool s_dc3_unpause_nudged = false;
  if (!s_dc3_unpause_nudged && game_addr && game_load_state == 3 &&
      game_wait_state == 3 && game_paused) {
    auto wr_u32 = [&](uint32_t va, uint32_t v) {
      if (IsGuestReadable(memory_, va, 4)) {
        xe::store_and_swap<uint32_t>(memory_->TranslateVirtual<uint8_t*>(va), v);
      }
    };
    auto wr_u8 = [&](uint32_t va, uint8_t v) {
      if (IsGuestReadable(memory_, va, 1)) {
        *memory_->TranslateVirtual<uint8_t*>(va) = v;
      }
    };
    wr_u32(game_addr + 0xA4, 0);                            // mWaitState = 0
    if (game_panel_addr) wr_u8(game_panel_addr + 0xF8, 0);  // unkf8 = 0
    wr_u8(game_addr + 0x60, 1);                             // mRealTime = 1
    wr_u8(game_addr + 0x5E, 0);                             // mPaused = 0 (LAST)
    s_dc3_unpause_nudged = true;
    XELOGI(
        "DC3 Script: UNPAUSE NUDGE applied (game={:08X} gp={:08X}): wait=0 "
        "unkf8=0 realTime=1 paused=0",
        game_addr, game_panel_addr);
  }

  // DIAGNOSTIC (Blocker A auto-pause root cause): when the Game flips back to
  // paused during playing, dump the UIEventMgr dialog-event queue so we can tell
  // whether the auto-pause came from GamePanel::Poll's HasActiveDialogEvent()
  // branch (dialog) or from Game::PauseForSkeletonLoss (fake-Kinect "no player
  // playing"). TheUIEventMgr = *0x83119650; mEventQueue (std::vector<BandEvent*>)
  // @ +0x2C is {begin,end,cap}; BandEvent.mType @ +0x0 (0=dialog,1=transition),
  // BandEvent.mDataArray @ +0x4; DataArray.mNodes @ +0x0, node0.value (Symbol
  // char*) @ +0x0. Read-only.
  static bool s_dc3_pause_diag_logged = false;
  if (game_paused && !dc3_last_game_paused_ && !s_dc3_pause_diag_logged) {
    s_dc3_pause_diag_logged = true;
    constexpr uint32_t kTheUIEventMgr = 0x83119650;
    uint32_t mgr = load_u32(kTheUIEventMgr);
    uint32_t qbegin = mgr ? load_u32(mgr + 0x2C) : 0;
    uint32_t qend = mgr ? load_u32(mgr + 0x30) : 0;
    uint32_t qsize = (qbegin && qend >= qbegin) ? (qend - qbegin) / 4 : 0;
    int front_type = -1;
    uint32_t front_sym = 0;
    if (qsize) {
      uint32_t evt = load_u32(qbegin);
      if (evt) {
        front_type = static_cast<int>(load_u32(evt + 0x0));
        uint32_t arr = load_u32(evt + 0x4);
        if (arr) {
          uint32_t nodes = load_u32(arr + 0x0);
          if (nodes) front_sym = load_u32(nodes + 0x0);
        }
      }
    }
    const char* sym_str = "";
    if (front_sym && IsGuestReadable(memory_, front_sym, 1)) {
      sym_str = reinterpret_cast<const char*>(
          memory_->TranslateVirtual<uint8_t*>(front_sym));
    }
    XELOGI(
        "DC3 Script: PAUSE-ONSET DIAG eventMgr={:08X} qsize={} frontType={} "
        "frontSym='{}' (frontType==0 => dialog auto-pause; empty/!=0 => "
        "skeleton-loss path)",
        mgr, qsize, front_type, sym_str);
  }

  bool state_changed = game_panel_addr != dc3_last_game_panel_addr_ ||
                       game_addr != dc3_last_game_addr_ ||
                       game_panel_state != dc3_last_game_panel_state_ ||
                       game_load_state != dc3_last_game_load_state_ ||
                       game_wait_state != dc3_last_game_wait_state_ ||
                       game_paused != dc3_last_game_paused_ ||
                       game_time_paused != dc3_last_game_time_paused_ ||
                       game_real_time != dc3_last_game_real_time_ ||
                       game_has_intro != dc3_last_game_has_intro_;

  auto since_last_log =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now - dc3_gameplay_probe_last_log_time_)
          .count();
  if (state_changed || since_last_log >= 2000) {
    dc3_gameplay_probe_last_log_time_ = now;
    XELOGI(
        "DC3 Script: gameplay gp={:08X} gpState={} game={:08X} load={} "
        "wait={} paused={} timePaused={} realTime={} hasIntro={}",
        game_panel_addr, game_panel_state, game_addr, game_load_state,
        game_wait_state, game_paused ? 1 : 0, game_time_paused ? 1 : 0,
        game_real_time ? 1 : 0, game_has_intro ? 1 : 0);
  }

  dc3_last_game_panel_addr_ = game_panel_addr;
  dc3_last_game_addr_ = game_addr;
  dc3_last_game_panel_state_ = game_panel_state;
  dc3_last_game_load_state_ = game_load_state;
  dc3_last_game_wait_state_ = game_wait_state;
  dc3_last_game_paused_ = game_paused;
  dc3_last_game_time_paused_ = game_time_paused;
  dc3_last_game_real_time_ = game_real_time;
  dc3_last_game_has_intro_ = game_has_intro;
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
        auto* ui_ptr =
            IsGuestReadable(memory_, kTheUI, 4)
                ? memory_->TranslateVirtual<uint8_t*>(kTheUI)
                : nullptr;
        ui_addr = ui_ptr ? xe::load_and_swap<uint32_t>(ui_ptr) : 0;
        if (IsGuestReadable(memory_, ui_addr, 0x50)) {
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

            // Original-XEX headless can get stuck before the transition target
            // is promoted from mTransitionScreen into mCurrentScreen. This was
            // first observed on the initial attract transition, but the same
            // issue also appears on title -> wait_main_after_saveload_screen
            // once the guest path is using real GotoScreen().
            bool should_force_complete =
                !cur_screen ||
                (name_1c == "title_screen" &&
                 trans_name_1c == "wait_main_after_saveload_screen") ||
                (name_1c == "wait_main_after_saveload_screen" &&
                 trans_name_1c == "main_screen") ||
                (name_1c == "main_screen" &&
                 trans_name_1c == "choose_mode_screen") ||
                (name_1c == "choose_mode_screen" &&
                 trans_name_1c == "song_select_screen") ||
                (name_1c == "song_select_screen" &&
                 trans_name_1c == "multiuser_screen") ||
                (name_1c == "multiuser_screen" &&
                 trans_name_1c == "loading_screen") ||
                (name_1c == "loading_screen" &&
                 trans_name_1c == "preloading_screen") ||
                (name_1c == "preloading_screen" &&
                 trans_name_1c == "real_loading_screen") ||
                (name_1c == "real_loading_screen" &&
                 trans_name_1c == "game_screen");
            if (should_force_complete && trans_screen && trans_state != 0) {
              if (s_last_stuck_transition != trans_screen) {
                s_last_stuck_transition = trans_screen;
                s_stuck_transition_start = now;
              } else {
                auto stuck_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - s_stuck_transition_start)
                        .count();
                if (stuck_ms >= 4000) {
                  XELOGI("DC3 Script: observed stuck UI transition cur={:08X} "
                         "trans={:08X} state={} name='{}' trans='{}' after {}ms",
                         cur_screen, trans_screen, trans_state, name_1c,
                         trans_name_1c, stuck_ms);
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
          auto* ui_ptr =
              IsGuestReadable(memory_, kTheUI, 4)
                  ? memory_->TranslateVirtual<uint8_t*>(kTheUI)
                  : nullptr;
          uint32_t ui_addr = ui_ptr ? xe::load_and_swap<uint32_t>(ui_ptr) : 0;
          auto* ui_obj = IsGuestReadable(memory_, ui_addr, 0x50)
                             ? memory_->TranslateVirtual<uint8_t*>(ui_addr)
                             : nullptr;
          if (ui_obj) {
            if (!s_title_screen_addr) {
              for (int scan_pass = 0; scan_pass < 2 && !s_title_screen_addr;
                   ++scan_pass) {
                bool strict_scan_range = scan_pass == 0;
                if (scan_pass == 1) {
                  XELOGI("DC3 Script: retrying title screen scan without "
                         ".rdata fence");
                }
                for (uint32_t addr = 0x40C00000; addr < 0x41000000;
                     addr += 4) {
                  if (!IsGuestReadable(memory_, addr + 0x20, 4)) {
                    continue;
                  }
                  std::string name =
                      ReadGuestScreenName(memory_, addr, strict_scan_range);
                  if (name == "title_screen" || name == "title") {
                    s_title_screen_addr = addr;
                    XELOGI("DC3 Script: resolved title screen object {:08X} "
                           "via name '{}'",
                           s_title_screen_addr, name);
                    break;
                  }
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
  // DC3 host-beat drive + screen-aware nav + dynamic injection are all global
  // (single-driver) state; only evaluate them once, on the primary pad.
  if (pad == 0) {
    UpdateDc3HostBeatDrive();
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

X_RESULT NopInputDriver::GetState(uint32_t user_index,
                                  X_INPUT_STATE* out_state) {
  if (!scripted_mode_ || user_index >= kMaxPads) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint16_t active_buttons = GetCurrentButtons(user_index);

  // Generate keystroke events for button transitions
  uint16_t pressed = active_buttons & ~prev_buttons_[user_index];
  uint16_t released = prev_buttons_[user_index] & ~active_buttons;

  // Check each button bit for transitions
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

  std::memset(reinterpret_cast<void*>(out_state), 0, sizeof(*out_state));
  out_state->packet_number = packet_number_++;
  out_state->gamepad.buttons = active_buttons;

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
      if (ks.virtual_key) keystroke_queue_[user_index].push_back(ks);
    }
  }
  prev_buttons_[user_index] = active_buttons;

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
