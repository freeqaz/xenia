/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914): --rb3dx_autoconfirm_parts, the closed-loop
 * menu autopilot (NOT upstream).
 *
 * Harness automation, not a probe: every ~2 s it reads the current UI screen
 * and injects the pad press that advances it. Fixed-time --scripted_input
 * presses cannot hit these windows reliably (menu/ark load times vary by tens
 * of seconds between runs; si6..si10). Presses go through the nop HID
 * driver's per-pad injection; it writes no guest memory.
 ******************************************************************************
 */

#include <string>

#include "xenia/base/logging.h"
#include "xenia/hid/nop/nop_input_driver.h"
#include "xenia/memory.h"
#include "xenia/titles/probe_threads.h"
#include "xenia/titles/rb3/rb3_flags.h"
#include "xenia/titles/rb3/rb3_guest.h"
#include "xenia/titles/rb3/rb3_internal.h"
#include "xenia/titles/title_hooks.h"

namespace xe {
namespace titles {
namespace rb3 {

namespace {

constexpr uint16_t kButtonDpadUp = 0x0001;
constexpr uint16_t kButtonDpadDown = 0x0002;
constexpr uint16_t kButtonStart = 0x0010;
constexpr uint16_t kButtonA = 0x1000;
constexpr uint16_t kButtonB = 0x2000;
constexpr uint64_t kPressMs = 250;

const char* ButtonName(uint16_t button) {
  switch (button) {
    case kButtonDpadUp:
      return "UP";
    case kButtonDpadDown:
      return "DOWN";
    case kButtonStart:
      return "START";
    case kButtonA:
      return "A";
    case kButtonB:
      return "B";
    default:
      return "?";
  }
}

class Autopilot {
 public:
  explicit Autopilot(Memory* memory)
      : reader_(memory) {}

  void Run() {
    while (ProbeSleep(2000)) {
      ++sample_;
      Step();
    }
  }

 private:
  void Press(uint32_t pad, uint16_t button, const std::string& why) {
    xe::hid::nop::NopInjectButtonPress(pad, button, kPressMs);
    XELOGW("RB3 AUTOPILOT[{}]: {}@{} ({})", sample_, ButtonName(button), pad,
           why);
  }

  // Occupied slots in BandUserMgr's guid-keyed slot map: the real "who has
  // joined the band" signal (the pad table's LocalUser binding is just
  // sign-in and is non-zero for every --local_user_count user).
  int SlotsOccupied() const {
    uint32_t mgr = reader_.R32(kTheBandUserMgr);
    if (!mgr) return 0;
    int n = 0;
    for (uint32_t s = 0; s < 4; ++s) {
      uint32_t g = mgr + 0x50 + s * 0x10;
      if (reader_.R32(g) | reader_.R32(g + 4) | reader_.R32(g + 8) |
          reader_.R32(g + 12)) {
        ++n;
      }
    }
    return n;
  }

  uint32_t PadLocalUser(uint32_t pad) const {
    return reader_.R32(kJoypadData + kJoypadUserOffset + pad * kJoypadStride);
  }

  void Step();

  GuestReader reader_;
  int sample_ = 0;
  int part_screen_samples_ = 0;
  int p2_ups_done_ = 0;
  int hub_samples_ = 0;
};

void Autopilot::Step() {
  // Only act on a settled UI: a press during a transition is lost or lands on
  // the screen being left.
  if (ReadTransitionState(reader_) != 0) {
    return;
  }
  std::string screen = ReadCurrentScreenName(reader_);
  bool odd = (sample_ & 1) != 0;
  if (screen != "part_difficulty_screen") {
    part_screen_samples_ = 0;
    p2_ups_done_ = 0;
  }
  if (screen != "main_hub_screen") {
    hub_samples_ = 0;
  }
  if (screen == "part_difficulty_screen") {
    // Pad-1-first ordering is load-bearing: if P1 confirms while P2's card is
    // untouched, the game leaves the screen and AutoAssignMissingSlots
    // handles P2 -- with the SI hooks armed, that path wedges in the track_-1
    // vector[-1] fault livelock (si12). An A on pad 1 here JOINS P2 directly
    // (si33); then N UPs (--rb3dx_autoconfirm_p2_up) walk P2's CHOOSE
    // INSTRUMENT list off its slot default; then the confirm cadence.
    ++part_screen_samples_;
    int slots = SlotsOccupied();
    if (slots < 2) {
      Press(1, kButtonA,
            "part_difficulty_screen join, slots=" + std::to_string(slots));
    } else if (p2_ups_done_ < cvars::rb3dx_autoconfirm_p2_up) {
      ++p2_ups_done_;
      Press(1, kButtonDpadUp,
            "part_difficulty_screen up " + std::to_string(p2_ups_done_));
    } else {
      uint32_t pad = (part_screen_samples_ <= 6 || odd) ? 1u : 0u;
      Press(pad, kButtonA,
            "part_difficulty_screen sample " +
                std::to_string(part_screen_samples_));
    }
  } else if (screen == "song_select_screen") {
    if (!odd) return;
    // P2 joins HERE: a join from the hub resolves the instrument inside the
    // join overshell (si30), while a join from song_select defers the part
    // pick to part_difficulty_screen. Alternate START/A on pad 1 until its
    // LocalUser binds, and only then confirm the song: confirming with P2
    // unjoined hands the empty slot to AutoAssignMissingSlots (si12).
    if (PadLocalUser(1) != 0) {
      Press(0, kButtonA, screen);
    } else {
      Press(1, (sample_ & 2) ? kButtonStart : kButtonA,
            "song_select_screen, joining P2");
    }
  } else if (screen == "intro_movie_screen" || screen == "splash_screen" ||
             screen == "dx_welcome_screen" ||
             screen == "dx_settings_error_screen") {
    if (odd) Press(0, kButtonA, screen);
  } else if (screen == "hint_rb3_welcome_screen") {
    // Fresh-save first-boot chain: message page(s), then a CUSTOMIZE BAND /
    // CONTINUE choice whose default is CUSTOMIZE. DOWN is a no-op on message
    // pages and moves focus to CONTINUE on the choice page, so alternating
    // DOWN / A dismisses the chain without entering Customize Band.
    Press(0, odd ? kButtonA : kButtonDpadDown, screen);
  } else if (screen == "manage_band_screen") {
    // Recovery: a stray confirm entered Customize Band; B backs out.
    if (odd) Press(0, kButtonB, screen);
  } else if (screen == "main_hub_screen") {
    // PLAY NOW is at the top of the hub list: two UPs pin focus there from
    // any tile, then A enters it.
    ++hub_samples_;
    if (hub_samples_ <= 2) {
      Press(0, kButtonDpadUp, screen + " " + std::to_string(hub_samples_));
    } else if (odd) {
      Press(0, kButtonA, screen);
    }
  }
}

}  // namespace

void StartAutopilot(const TitleLaunchContext& ctx) {
  if (!cvars::rb3dx_autoconfirm_parts) {
    return;
  }
  Memory* memory = ctx.memory;
  SpawnProbeThread([memory]() { Autopilot(memory).Run(); });
  XELOGI("RB3: menu autopilot thread started (--rb3dx_autoconfirm_parts)");
}

}  // namespace rb3
}  // namespace titles
}  // namespace xe
