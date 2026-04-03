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
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/hid/input_driver.h"

namespace xe {
class Memory;
}

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

  // Enable screen-aware scripted input mode.
  // Script file format (one directive per line):
  //   wait_screen <screen_name>    — wait until UIManager.mCurrentScreen matches
  //   +<frames> <button>           — after wait is satisfied, wait N frames then press button
  //   +<frames> NONE              — after wait is satisfied, wait N frames with no button press
  //   # comment                    — ignored
  //
  // Requires SetMemory() to be called first so we can read guest memory.
  void LoadScriptFile(const std::string& path);

  // Set guest memory pointer so screen-aware scripting can read TheUI.
  void SetMemory(Memory* memory) { memory_ = memory; }

  // Inject a one-shot button press from external code (e.g., NUI handler).
  // Thread-safe. The press will be active for duration_ms.
  void InjectButtonPress(uint16_t buttons, uint64_t duration_ms = 200);

 private:
  struct ScriptedEvent {
    uint64_t time_ms;      // Milliseconds after start
    uint16_t buttons;      // Button flags to press
    uint64_t duration_ms;  // How long to hold (default 200ms)
  };

  // Screen-aware script directive
  struct ScriptDirective {
    enum Type { kWaitScreen, kDelayedPress };
    Type type;
    std::string screen_name;  // For kWaitScreen
    int delay_ms;             // For kDelayedPress: ms after wait satisfied
    uint16_t buttons;         // For kDelayedPress
  };

  // Dynamic injection event
  struct InjectedEvent {
    std::chrono::steady_clock::time_point start;
    uint64_t duration_ms;
    uint16_t buttons;
  };

  uint16_t GetCurrentButtons();
  uint16_t ButtonToVK(uint16_t button) const;

  // Read the current screen name from guest memory (TheUI->mCurrentScreen->mName)
  std::string ReadCurrentScreenName() const;

  // Drive DC3 gameplay timelines from a callback that survives past
  // the final menu transition.
  void UpdateDc3HostBeatDrive();
  void ProbeDc3GameplayState();

  // Process screen-aware script state machine
  uint16_t GetScreenAwareButtons();

  bool scripted_mode_ = false;
  std::vector<ScriptedEvent> scripted_events_;
  std::chrono::steady_clock::time_point start_time_;
  uint32_t packet_number_ = 0;
  uint16_t prev_buttons_ = 0;  // For keystroke edge detection
  std::deque<X_INPUT_KEYSTROKE> keystroke_queue_;

  // Screen-aware scripting state
  bool screen_aware_mode_ = false;
  std::vector<ScriptDirective> script_directives_;
  size_t script_index_ = 0;           // Current directive index
  bool wait_satisfied_ = false;       // Current wait_screen matched
  std::chrono::steady_clock::time_point wait_satisfied_time_;  // When wait was satisfied
  std::string last_screen_name_;      // Cache to avoid re-reading every call
  std::chrono::steady_clock::time_point last_screen_read_time_;  // Throttle reads
  bool dc3_host_beat_drive_active_ = false;
  float dc3_host_song_seconds_ = 0.0f;
  float dc3_host_song_beat_ = 0.0f;
  std::chrono::steady_clock::time_point dc3_host_last_update_time_;
  std::chrono::steady_clock::time_point dc3_host_last_log_time_;
  std::chrono::steady_clock::time_point dc3_gameplay_probe_last_log_time_;
  uint32_t dc3_last_game_panel_addr_ = 0;
  uint32_t dc3_last_game_addr_ = 0;
  int dc3_last_game_panel_state_ = -1;
  int dc3_last_game_load_state_ = -1;
  int dc3_last_game_wait_state_ = -1;
  bool dc3_last_game_paused_ = false;
  bool dc3_last_game_time_paused_ = false;
  bool dc3_last_game_real_time_ = false;
  bool dc3_last_game_has_intro_ = false;

  // Guest memory access
  Memory* memory_ = nullptr;

  // Dynamic injection
  std::mutex inject_mutex_;
  std::vector<InjectedEvent> injected_events_;
};

}  // namespace nop
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_NOP_NOP_INPUT_DRIVER_H_
