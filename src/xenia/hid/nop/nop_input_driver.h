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
#include <cstdint>
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

// Title hooks for the scripted-input player (NOT upstream). The player below
// is title-agnostic: it parses the script, times presses and turns them into
// XInput state/keystrokes. Everything that knows a title's memory layout (how
// to read the current UI screen name, per-title harness automation) lives in
// an adapter the title module installs with SetScriptedInputTitleAdapter().
class ScriptedInputTitleAdapter {
 public:
  virtual ~ScriptedInputTitleAdapter() = default;
  // The current UI screen name, "" if it cannot be read.
  virtual std::string ReadCurrentScreenName(Memory* memory) = 0;
  // Called on every primary-pad poll with the latest screen name.
  virtual void OnPrimaryPadPoll(Memory* memory, const std::string& screen) {}
  // Called on every poll while a `wait_screen <wanted>` directive is not yet
  // satisfied. May return buttons to press, and may replace *screen when it
  // moved the UI itself.
  virtual uint16_t WhileWaitingForScreen(Memory* memory,
                                         const std::string& wanted,
                                         std::string* screen) {
    return 0;
  }
  // Called every 2 s while a wait_screen directive is pending.
  virtual void LogWaitStatus(Memory* memory, const std::string& wanted,
                             const std::string& screen) {}
  // True while the UI is between screens (the game's UIManager::InTransition).
  // A `wait_screen` is satisfied only on a settled screen, as in the native
  // port's player (dc3-decomp native/src/platform/Joypad_Native.cpp).
  virtual bool InTransition(Memory* memory) { return false; }
  // A guest frame clock: true if FrameNumber() counts guest frames (one tick
  // per pass of the game's main loop). With a clock the player runs the
  // native port's semantics: `+N` is N frames after the last satisfied
  // wait_screen, a press lasts exactly one frame, and a wait_screen gives up
  // after 1800 frames. Without one it keeps the legacy wall-clock timing
  // (`+N` = N x 50 ms, 350 ms hold).
  virtual bool HasFrameClock() { return false; }
  virtual int64_t FrameNumber() { return -1; }
};

// True once LoadScriptFile() loaded a screen-aware script (set before the
// title launches, so a title module can install its frame clock).
bool ScreenAwareScriptLoaded();

// Installs the adapter for the running title (nullptr removes it). The
// adapter must outlive every driver call; title modules use a static one.
void SetScriptedInputTitleAdapter(ScriptedInputTitleAdapter* adapter);

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
  //   wait_screen <screen_name>    — wait until the current screen is
  //                                  <screen_name> and not in transition
  //   +<frames> <button>           — press <button> N frames after the last
  //                                  satisfied wait_screen
  //   <frames> <button>            — press <button> at absolute frame N
  //   # comment                    — ignored
  // The format and the semantics are the native port's
  // (dc3-decomp native/src/platform/Joypad_Native.cpp), so one flow file
  // drives both. Buttons: confirm/a, cancel/b, x, y, start, option/back/
  // select, up/down/left/right, l1/lb, r1/rb, l2/lt, r2/rt, l3/ls, r3/rs.
  // NONE/NOOP/IDLE lines are accepted and press nothing.
  //
  // Requires SetMemory() to be called first so we can read guest memory.
  void LoadScriptFile(const std::string& path);

  // Set guest memory pointer so screen-aware scripting can read TheUI.
  void SetMemory(Memory* memory) { memory_ = memory; }

  // Inject a one-shot button press from external code (e.g., NUI handler,
  // the RB3 menu autopilot). Thread-safe. The press will be active
  // for duration_ms on the given pad.
  void InjectButtonPress(uint16_t buttons, uint64_t duration_ms = 200,
                         uint32_t pad = 0);

 private:
  static constexpr uint32_t kMaxPads = 2;

  struct ScriptedEvent {
    uint64_t time_ms;      // Milliseconds after start
    uint16_t buttons;      // Button flags to press
    uint64_t duration_ms;  // How long to hold (default 200ms)
    uint8_t pad = 0;       // Target controller port (0 or 1)
  };

  // Screen-aware script directive
  struct ScriptDirective {
    enum Type { kWaitScreen, kDelayedPress };
    Type type;
    std::string screen_name;  // For kWaitScreen
    int delay_ms;             // For kDelayedPress (legacy timing): N x 50 ms
    uint16_t buttons;         // For kDelayedPress
    int frame = 0;            // For kDelayedPress (frame clock): N
    bool relative = true;     // `+N` (after the last wait) or absolute `N`
    uint8_t triggers = 0;     // bit 0 = left trigger, bit 1 = right trigger
  };

  // Dynamic injection event
  struct InjectedEvent {
    std::chrono::steady_clock::time_point start;
    uint64_t duration_ms;
    uint16_t buttons;
    uint8_t pad = 0;
  };

  uint16_t GetCurrentButtons(uint32_t pad);
  // Queues KEYDOWN/KEYUP keystrokes for the edges since the previous poll.
  void QueueKeystrokeEdges(uint32_t user_index, uint16_t active_buttons);
  uint16_t ButtonToVK(uint16_t button) const;

  // The current screen name, through the title adapter.
  std::string ReadCurrentScreenName() const;
  // Primary-pad poll: refresh the screen name (every 100 ms) and hand it to
  // the title adapter.
  void PollTitleAdapter();

  // Process screen-aware script state machine
  uint16_t GetScreenAwareButtons();
  // Frame-clock mode: the native port's player, evaluated once per guest
  // frame. Returns the buttons for the current frame.
  uint16_t GetFrameScriptButtons(ScriptedInputTitleAdapter* adapter);
  // One frame of the native state machine.
  uint16_t StepFrameScript(ScriptedInputTitleAdapter* adapter, int64_t frame,
                           uint8_t* triggers);

  bool scripted_mode_ = false;
  std::vector<ScriptedEvent> scripted_events_;
  std::chrono::steady_clock::time_point start_time_;
  uint32_t packet_number_ = 0;
  uint16_t prev_buttons_[kMaxPads] = {0, 0};  // Per-pad keystroke edge detection
  std::deque<X_INPUT_KEYSTROKE> keystroke_queue_[kMaxPads];

  // Screen-aware scripting state
  bool screen_aware_mode_ = false;
  std::vector<ScriptDirective> script_directives_;
  size_t script_index_ = 0;           // Current directive index
  bool wait_satisfied_ = false;       // Current wait_screen matched
  std::chrono::steady_clock::time_point wait_satisfied_time_;  // When wait was satisfied
  std::string last_screen_name_;      // Cache to avoid re-reading every call
  std::chrono::steady_clock::time_point last_screen_read_time_;  // Throttle reads

  // Frame-clock scripting state (mode decided at the first evaluation).
  int frame_mode_ = -1;  // -1 undecided, 0 legacy, 1 frame clock
  bool frame_waiting_ = false;
  int64_t frame_wait_start_ = -1;
  int64_t frame_wait_satisfied_ = -1;
  int64_t frame_last_eval_ = -1;
  uint16_t frame_buttons_ = 0;
  uint8_t frame_triggers_ = 0;
  std::chrono::steady_clock::time_point frame_last_log_;

  // Guest memory access
  Memory* memory_ = nullptr;

  // Dynamic injection
  std::mutex inject_mutex_;
  std::vector<InjectedEvent> injected_events_;
};

// Process-wide bridge to the (single) live NopInputDriver instance so
// host-side threads (e.g. the RB3 menu autopilot, titles/rb3/rb3_autopilot.cc)
// can inject screen-conditional presses without holding an InputSystem
// reference. No-op if no nop driver is active.
void NopInjectButtonPress(uint32_t pad, uint16_t buttons,
                          uint64_t duration_ms);

}  // namespace nop
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_NOP_NOP_INPUT_DRIVER_H_
