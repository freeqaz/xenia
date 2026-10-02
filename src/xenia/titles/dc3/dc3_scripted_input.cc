/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 scripted-input title adapter (NOT upstream). See dc3_scripted_input.h.
 *
 * Moved out of hid/nop/nop_input_driver.cc; log texts are unchanged (the
 * fork-regress analyzers read "DC3 Script: ... gpState=" and "wait_screen ...
 * SATISFIED").
 ******************************************************************************
 */

#include "xenia/titles/dc3/dc3_scripted_input.h"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <string>

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/hid/input.h"
#include "xenia/hid/nop/nop_input_driver.h"
#include "xenia/memory.h"
#include "xenia/titles/dc3/dc3_autonav.h"
#include "xenia/titles/dc3/dc3_hacks.h"

namespace xe {
namespace dc3 {

namespace {

constexpr uint32_t kDc3OriginalXexNameMin = 0x82000000;
constexpr uint32_t kDc3OriginalXexNameMax = 0x82400000;
constexpr uint32_t kTheUI = 0x82F1A8E0;

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
    if (strict_scan_range && (name_ptr < kDc3OriginalXexNameMin ||
                              name_ptr >= kDc3OriginalXexNameMax)) {
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

class Dc3ScriptedInputAdapter final
    : public hid::nop::ScriptedInputTitleAdapter {
 public:
  std::string ReadCurrentScreenName(Memory* memory) override;
  void OnPrimaryPadPoll(Memory* memory, const std::string& screen) override;
  uint16_t WhileWaitingForScreen(Memory* memory, const std::string& wanted,
                                 std::string* screen) override;
  void LogWaitStatus(Memory* memory, const std::string& wanted,
                     const std::string& screen) override;

 private:
  void ProbeGameplayState(Memory* memory, const std::string& screen);

  // input.beat_drive (beat drive B).
  bool beat_drive_active_ = false;
  float song_seconds_ = 0.0f;
  float song_beat_ = 0.0f;
  std::chrono::steady_clock::time_point last_update_time_;
  std::chrono::steady_clock::time_point last_log_time_;

  // Read-only gameplay probe.
  std::chrono::steady_clock::time_point probe_last_log_time_;
  uint32_t last_game_panel_addr_ = 0;
  uint32_t last_game_addr_ = 0;
  int last_game_panel_state_ = -1;
  int last_game_load_state_ = -1;
  int last_game_wait_state_ = -1;
  bool last_game_paused_ = false;
  bool last_game_time_paused_ = false;
  bool last_game_real_time_ = false;
  bool last_game_has_intro_ = false;
  bool unpause_nudged_ = false;
  bool pause_diag_logged_ = false;

  // LogWaitStatus / WhileWaitingForScreen.
  uint32_t last_stuck_transition_ = 0;
  std::chrono::steady_clock::time_point stuck_transition_start_;
  std::chrono::steady_clock::time_point attract_seen_since_;
  std::chrono::steady_clock::time_point last_attract_press_;
  uint32_t title_screen_addr_ = 0;
  int attract_force_gate_ = -1;
};

std::string Dc3ScriptedInputAdapter::ReadCurrentScreenName(Memory* memory) {
  if (!memory) return "";
  auto* ui_ptr = IsGuestReadable(memory, kTheUI, 4)
                     ? memory->TranslateVirtual<uint8_t*>(kTheUI)
                     : nullptr;
  if (!ui_ptr) return "";

  uint32_t ui_addr = xe::load_and_swap<uint32_t>(ui_ptr);
  if (!IsGuestReadable(memory, ui_addr, 0x50)) return "";

  auto* ui_obj = memory->TranslateVirtual<uint8_t*>(ui_addr);

  // mCurrentScreen at offset 0x48
  uint32_t cur_screen = xe::load_and_swap<uint32_t>(ui_obj + 0x48);
  if (!cur_screen || cur_screen >= 0xF0000000) return "";

  // Original debug XEX screen objects have been observed with mName at +0x1C,
  // while some earlier experiments assumed +0x20. Try both to keep
  // screen-aware scripts working across layouts.
  return ReadGuestScreenName(memory, cur_screen, false);
}

void Dc3ScriptedInputAdapter::OnPrimaryPadPoll(Memory* memory,
                                               const std::string& screen) {
  if (!memory) {
    return;
  }
  auto now = std::chrono::steady_clock::now();

  if (screen != "game_screen") {
    if (beat_drive_active_) {
      XELOGI("DC3 Script: host beat drive deactivated on '{}'", screen);
      beat_drive_active_ = false;
    }
    return;
  }

  constexpr uint32_t kTheTaskMgr = 0x82F64A58;
  constexpr uint32_t kTimelineStride = 0x1C;
  constexpr uint32_t kTimeOff = 0x10;
  constexpr uint32_t kLastTimeOff = 0x14;

  auto load_u32 = [&](uint32_t guest_addr) -> uint32_t {
    auto* ptr = IsGuestReadable(memory, guest_addr, 4)
                    ? memory->TranslateVirtual<uint8_t*>(guest_addr)
                    : nullptr;
    return ptr ? xe::load_and_swap<uint32_t>(ptr) : 0;
  };
  auto load_float = [&](uint32_t guest_addr) -> float {
    auto* ptr = IsGuestReadable(memory, guest_addr, 4)
                    ? memory->TranslateVirtual<uint8_t*>(guest_addr)
                    : nullptr;
    return ptr ? xe::load_and_swap<float>(ptr) : 0.0f;
  };
  auto store_float = [&](uint32_t guest_addr, float value) -> bool {
    if (!IsGuestReadable(memory, guest_addr, 4)) {
      return false;
    }
    auto* ptr = memory->TranslateVirtual<uint8_t*>(guest_addr);
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
        (gp_gate && IsGuestReadable(memory, gp_gate + 0x38, 4))
            ? load_u32(gp_gate + 0x38)
            : 0;
    bool paused = true;
    if (game_gate && IsGuestReadable(memory, game_gate + 0x5E, 1)) {
      auto* pp = memory->TranslateVirtual<uint8_t*>(game_gate + 0x5E);
      paused = pp ? (*pp != 0) : true;
    }
    if (!game_gate || paused) {
      if (beat_drive_active_) {
        beat_drive_active_ = false;
        XELOGI("DC3 Script: beat gate closed (Game paused/not ready)");
      }
      // Keep probing so we can watch the load/wait/paused progression.
      ProbeGameplayState(memory, screen);
      return;
    }
  }

  uint32_t timelines_addr = load_u32(kTheTaskMgr + 0x2C);
  if (!timelines_addr || !IsGuestReadable(memory, timelines_addr + 0x54, 4) ||
      !IsGuestReadable(memory, kTheTaskMgr + 0x48, 1)) {
    return;
  }

  static const bool kBeatDrive =
      AutonavEnabled() &&
      HackGate("input.beat_drive",
               "second 120 BPM TaskMgr timeline drive from the pad poll "
               "(wall clock)");
  if (!kBeatDrive) {
    ProbeGameplayState(memory, screen);
    return;
  }

  auto* auto_ptr = memory->TranslateVirtual<uint8_t*>(kTheTaskMgr + 0x48);
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
  uint32_t ui_last_addr = timelines_addr + 2 * kTimelineStride + kLastTimeOff;

  float old_seconds = load_float(seconds_time_addr);
  float old_beats = load_float(beats_time_addr);
  float old_ui = load_float(ui_time_addr);

  if (!beat_drive_active_) {
    HackFired("input.beat_drive");
    song_seconds_ = old_seconds;
    song_beat_ = old_beats;
    last_update_time_ = now;
    last_log_time_ = now;
    beat_drive_active_ = true;
    XELOGI(
        "DC3 Script: host beat drive activated taskmgr={:08X} timelines={:08X} "
        "sec={:.3f} beat={:.3f}",
        kTheTaskMgr, timelines_addr, song_seconds_, song_beat_);
    ProbeGameplayState(memory, screen);
    return;
  }

  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_update_time_)
                        .count();
  if (elapsed_ms <= 0) {
    return;
  }
  if (elapsed_ms > 100) {
    elapsed_ms = 33;
  }
  last_update_time_ = now;

  float delta_seconds = static_cast<float>(elapsed_ms) / 1000.0f;
  float delta_beats = delta_seconds * (120.0f / 60.0f);
  song_seconds_ += delta_seconds;
  song_beat_ += delta_beats;

  store_float(seconds_last_addr, old_seconds);
  store_float(seconds_time_addr, song_seconds_);
  store_float(beats_last_addr, old_beats);
  store_float(beats_time_addr, song_beat_);
  store_float(ui_last_addr, old_ui);
  store_float(ui_time_addr, song_seconds_);

  auto since_last_log = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - last_log_time_)
                            .count();
  if (since_last_log >= 2000) {
    last_log_time_ = now;
    XELOGI("DC3 Script: host beat drive sec={:.3f} beat={:.3f}", song_seconds_,
           song_beat_);
  }

  ProbeGameplayState(memory, screen);
}

void Dc3ScriptedInputAdapter::ProbeGameplayState(Memory* memory,
                                                 const std::string& screen) {
  if (!memory || screen != "game_screen") {
    return;
  }

  auto now = std::chrono::steady_clock::now();
  auto load_u32 = [&](uint32_t guest_addr) -> uint32_t {
    auto* ptr = IsGuestReadable(memory, guest_addr, 4)
                    ? memory->TranslateVirtual<uint8_t*>(guest_addr)
                    : nullptr;
    return ptr ? xe::load_and_swap<uint32_t>(ptr) : 0;
  };
  auto load_u8 = [&](uint32_t guest_addr) -> uint8_t {
    auto* ptr = IsGuestReadable(memory, guest_addr, 1)
                    ? memory->TranslateVirtual<uint8_t*>(guest_addr)
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

  if (game_panel_addr && IsGuestReadable(memory, game_panel_addr + 0x83, 1)) {
    game_addr = load_u32(game_panel_addr + 0x38);
    game_panel_state = static_cast<int>(load_u32(game_panel_addr + 0x80));
  }

  if (game_addr && IsGuestReadable(memory, game_addr + 0xA7, 1)) {
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
  if (!unpause_nudged_ && game_addr && game_load_state == 3 &&
      game_wait_state == 3 && game_paused && AutonavEnabled() &&
      HackGate("input.unpause_nudge",
               "Game wait=0 unkf8=0 realTime=1 paused=0 once stuck at "
               "load=3 wait=3")) {
    auto wr_u32 = [&](uint32_t va, uint32_t v) {
      if (IsGuestReadable(memory, va, 4)) {
        xe::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(va), v);
      }
    };
    auto wr_u8 = [&](uint32_t va, uint8_t v) {
      if (IsGuestReadable(memory, va, 1)) {
        *memory->TranslateVirtual<uint8_t*>(va) = v;
      }
    };
    wr_u32(game_addr + 0xA4, 0);                            // mWaitState = 0
    if (game_panel_addr) wr_u8(game_panel_addr + 0xF8, 0);  // unkf8 = 0
    wr_u8(game_addr + 0x60, 1);                             // mRealTime = 1
    wr_u8(game_addr + 0x5E, 0);                             // mPaused = 0 (LAST)
    unpause_nudged_ = true;
    HackFired("input.unpause_nudge");
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
  if (game_paused && !last_game_paused_ && !pause_diag_logged_) {
    pause_diag_logged_ = true;
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
    // Bounded read: the symbol is guest data, never trust a terminator.
    std::string sym_str;
    for (uint32_t i = 0; front_sym && i < 64; ++i) {
      if (!IsGuestReadable(memory, front_sym + i, 1)) break;
      char c = *memory->TranslateVirtual<char*>(front_sym + i);
      if (!c) break;
      sym_str.push_back(c);
    }
    XELOGI(
        "DC3 Script: PAUSE-ONSET DIAG eventMgr={:08X} qsize={} frontType={} "
        "frontSym='{}' (frontType==0 => dialog auto-pause; empty/!=0 => "
        "skeleton-loss path)",
        mgr, qsize, front_type, sym_str);
  }

  bool state_changed = game_panel_addr != last_game_panel_addr_ ||
                       game_addr != last_game_addr_ ||
                       game_panel_state != last_game_panel_state_ ||
                       game_load_state != last_game_load_state_ ||
                       game_wait_state != last_game_wait_state_ ||
                       game_paused != last_game_paused_ ||
                       game_time_paused != last_game_time_paused_ ||
                       game_real_time != last_game_real_time_ ||
                       game_has_intro != last_game_has_intro_;

  auto since_last_log = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - probe_last_log_time_)
                            .count();
  if (state_changed || since_last_log >= 2000) {
    probe_last_log_time_ = now;
    XELOGI(
        "DC3 Script: gameplay gp={:08X} gpState={} game={:08X} load={} "
        "wait={} paused={} timePaused={} realTime={} hasIntro={}",
        game_panel_addr, game_panel_state, game_addr, game_load_state,
        game_wait_state, game_paused ? 1 : 0, game_time_paused ? 1 : 0,
        game_real_time ? 1 : 0, game_has_intro ? 1 : 0);
  }

  last_game_panel_addr_ = game_panel_addr;
  last_game_addr_ = game_addr;
  last_game_panel_state_ = game_panel_state;
  last_game_load_state_ = game_load_state;
  last_game_wait_state_ = game_wait_state;
  last_game_paused_ = game_paused;
  last_game_time_paused_ = game_time_paused;
  last_game_real_time_ = game_real_time;
  last_game_has_intro_ = game_has_intro;
}

void Dc3ScriptedInputAdapter::LogWaitStatus(Memory* memory,
                                            const std::string& wanted,
                                            const std::string& screen) {
  auto now = std::chrono::steady_clock::now();
  uint32_t ui_addr = 0;
  uint32_t cur_screen = 0;
  uint32_t trans_screen = 0;
  uint32_t trans_state = 0;
  std::string name_1c;
  std::string trans_name_1c;
  if (memory) {
    auto* ui_ptr = IsGuestReadable(memory, kTheUI, 4)
                       ? memory->TranslateVirtual<uint8_t*>(kTheUI)
                       : nullptr;
    ui_addr = ui_ptr ? xe::load_and_swap<uint32_t>(ui_ptr) : 0;
    if (IsGuestReadable(memory, ui_addr, 0x50)) {
      auto* ui_obj = memory->TranslateVirtual<uint8_t*>(ui_addr);
      if (ui_obj) {
        trans_state = xe::load_and_swap<uint32_t>(ui_obj + 0x2C);
        cur_screen = xe::load_and_swap<uint32_t>(ui_obj + 0x48);
        trans_screen = xe::load_and_swap<uint32_t>(ui_obj + 0x4C);
        name_1c = ReadGuestScreenName(memory, cur_screen, false);
        if (name_1c.empty() && cur_screen) {
          name_1c = "<unnamed>";
        }
        trans_name_1c = ReadGuestScreenName(memory, trans_screen, false);
        if (trans_name_1c.empty() && trans_screen) {
          trans_name_1c = "<unnamed>";
        }

        // Original-XEX headless can get stuck before the transition target
        // is promoted from mTransitionScreen into mCurrentScreen. This was
        // first observed on the initial attract transition, but the same
        // issue also appears on title -> wait_main_after_saveload_screen
        // once the guest path is using real GotoScreen(). (Log only.)
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
          if (last_stuck_transition_ != trans_screen) {
            last_stuck_transition_ = trans_screen;
            stuck_transition_start_ = now;
          } else {
            auto stuck_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - stuck_transition_start_)
                    .count();
            if (stuck_ms >= 4000) {
              XELOGI("DC3 Script: observed stuck UI transition cur={:08X} "
                     "trans={:08X} state={} name='{}' trans='{}' after {}ms",
                     cur_screen, trans_screen, trans_state, name_1c,
                     trans_name_1c, stuck_ms);
            }
          }
        } else {
          last_stuck_transition_ = 0;
          stuck_transition_start_ = std::chrono::steady_clock::time_point{};
        }
      }
    }
  }
  XELOGI("DC3 Script: waiting for '{}' current='{}' ui={:08X} "
         "cur={:08X} trans={:08X} transState={} "
         "name='{}' trans='{}'",
         wanted, screen, ui_addr, cur_screen, trans_screen, trans_state,
         name_1c, trans_name_1c);
}

uint16_t Dc3ScriptedInputAdapter::WhileWaitingForScreen(
    Memory* memory, const std::string& wanted, std::string* screen) {
  uint16_t active = 0;
  auto now = std::chrono::steady_clock::now();
  if (!AutonavEnabled()) {
    return 0;
  }
  if (wanted == "title_screen" && *screen == "attract_screen") {
    if (attract_seen_since_ == std::chrono::steady_clock::time_point{}) {
      attract_seen_since_ = now;
    }
    auto attract_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - attract_seen_since_)
                          .count();
    auto since_last_press =
        last_attract_press_ == std::chrono::steady_clock::time_point{}
            ? INT64_MAX
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - last_attract_press_)
                  .count();
    if (attract_ms >= 1500 && since_last_press >= 3000) {
      static const bool kAttractPress =
          HackGate("input.attract_press",
                   "press A every 3 s while attract_screen blocks a "
                   "wait_screen title_screen");
      if (kAttractPress) {
        HackFired("input.attract_press");
        active |= hid::X_INPUT_GAMEPAD_A;
        last_attract_press_ = now;
        XELOGI("DC3 Script: attract-screen fallback press A while waiting "
               "for title_screen");
      }
    }
    if (memory && attract_ms >= 5000 && attract_force_gate_ < 0) {
      attract_force_gate_ =
          HackGate("input.attract_force",
                   "4 MiB heap scan + UIManager stomp attract -> title")
              ? 1
              : 0;
    }
    if (memory && attract_ms >= 5000 && attract_force_gate_ == 1) {
      auto* ui_ptr = IsGuestReadable(memory, kTheUI, 4)
                         ? memory->TranslateVirtual<uint8_t*>(kTheUI)
                         : nullptr;
      uint32_t ui_addr = ui_ptr ? xe::load_and_swap<uint32_t>(ui_ptr) : 0;
      auto* ui_obj = IsGuestReadable(memory, ui_addr, 0x50)
                         ? memory->TranslateVirtual<uint8_t*>(ui_addr)
                         : nullptr;
      if (ui_obj) {
        if (!title_screen_addr_) {
          for (int scan_pass = 0; scan_pass < 2 && !title_screen_addr_;
               ++scan_pass) {
            bool strict_scan_range = scan_pass == 0;
            if (scan_pass == 1) {
              XELOGI("DC3 Script: retrying title screen scan without "
                     ".rdata fence");
            }
            for (uint32_t addr = 0x40C00000; addr < 0x41000000; addr += 4) {
              if (!IsGuestReadable(memory, addr + 0x20, 4)) {
                continue;
              }
              std::string name =
                  ReadGuestScreenName(memory, addr, strict_scan_range);
              if (name == "title_screen" || name == "title") {
                title_screen_addr_ = addr;
                XELOGI("DC3 Script: resolved title screen object {:08X} "
                       "via name '{}'",
                       title_screen_addr_, name);
                break;
              }
            }
          }
        }
        if (title_screen_addr_) {
          HackFired("input.attract_force");
          xe::store_and_swap<uint32_t>(ui_obj + 0x48, title_screen_addr_);
          xe::store_and_swap<uint32_t>(ui_obj + 0x4C, 0);
          xe::store_and_swap<uint32_t>(ui_obj + 0x2C, 0);
          *screen = "title_screen";
          XELOGI("DC3 Script: forced UI jump attract_screen -> title_screen "
                 "({:08X})",
                 title_screen_addr_);
        }
      }
    }
  } else {
    attract_seen_since_ = std::chrono::steady_clock::time_point{};
  }
  return active;
}

Dc3ScriptedInputAdapter g_adapter;

}  // namespace

void InstallScriptedInputAdapter() {
  hid::nop::SetScriptedInputTitleAdapter(&g_adapter);
}

}  // namespace dc3
}  // namespace xe
