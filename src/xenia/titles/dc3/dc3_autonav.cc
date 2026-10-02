/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 headless autonav (NOT upstream). See dc3_autonav.h.
 *
 * Moved off the SkeletonUpdate worker (the NuiSkeletonGetNextFrame override,
 * dc3_nui_sequencer.cc) onto the guest main thread (dc3_main_thread.h). The
 * step logic is unchanged except: thresholds count autonav ticks (one per
 * >= 33 ms main-thread poll, the old 30 Hz NUI frame) instead of NUI frames;
 * the off-thread "Transition diag" Executes and the dc3_gameplay_probe
 * GATE/PKPROBE diagnostics are gone; the IK telemetry read stayed with the
 * NUI callback.
 ******************************************************************************
 */

#include "xenia/titles/dc3/dc3_autonav.h"

#include <chrono>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/memory.h"
#include "xenia/titles/dc3/dc3_flags.h"
#include "xenia/titles/dc3/dc3_hacks.h"
#include "xenia/titles/dc3/dc3_main_thread.h"

DEFINE_bool(dc3_headless_autonav, false,
            "DC3 (original debug.xex): drive the menus headless from the "
            "guest MAIN thread (HolmesClientPollKeyboard hook): walk "
            "attract -> ... -> game_screen with UIManager::GotoScreen when a "
            "screen sits idle. Also arms the scripted-input attract "
            "A-press/force. Off: the game boots to its own screens and only "
            "the DTA channel / scripted pad drive it.",
            "DC3");

namespace xe {
namespace dc3 {

namespace {

cpu::Processor* g_processor = nullptr;
Memory* g_memory = nullptr;
kernel::KernelState* g_kernel_state = nullptr;

void AutonavStep(cpu::ThreadState* ts) {
  Memory* memory = g_memory;
  auto* kernel_state = g_kernel_state;
  static int s_ticks = 0;
  static uint32_t s_last_screen = 0;
  static int s_screen_stable_count = 0;
  static bool s_screen_name_scan_range_logged = false;
  static uint32_t s_scan_name_min = 0;
  static uint32_t s_scan_name_max = 0;
  static std::unordered_map<std::string, uint32_t> s_name_literal_cache;

  static const bool kHackNavBridge = HackGate(
      "seq.nav_bridge",
      "UIManager::GotoScreen walk attract->...->game_screen (merge_busy hold)");
  s_ticks++;

  if (!s_scan_name_max) {
    if (auto module = kernel_state->GetExecutableModule()) {
      if (auto* xex = module->xex_module()) {
        if (auto* rdata = xex->GetPESection(".rdata")) {
          s_scan_name_min = rdata->address;
          s_scan_name_max = rdata->address + rdata->size;
        }
      }
    }
    if (!s_scan_name_max) {
      // Original debug XEX strings live in low 0x82xxxxxx; keep the fallback
      // tight so speculative scans don't treat arbitrary data as names.
      s_scan_name_min = 0x82000000;
      s_scan_name_max = 0x82400000;
    }
  }
  if (!s_screen_name_scan_range_logged) {
    XELOGI("DC3: screen-name scan range {:08X}-{:08X}", s_scan_name_min,
           s_scan_name_max);
    s_screen_name_scan_range_logged = true;
  }

  auto is_guest_readable = [&](uint32_t guest_addr, uint32_t size) -> bool {
    if (!guest_addr || guest_addr >= 0xF0000000 || !size) {
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
  };

  constexpr uint32_t kTheUI = 0x82F1A8E0;
  auto* ui_ptr = is_guest_readable(kTheUI, 4)
                     ? memory->TranslateVirtual<uint8_t*>(kTheUI)
                     : nullptr;
  uint32_t ui_addr = ui_ptr ? xe::load_and_swap<uint32_t>(ui_ptr) : 0;
  if (is_guest_readable(ui_addr, 0x50)) {
    auto* ui_obj = memory->TranslateVirtual<uint8_t*>(ui_addr);
    if (ui_obj) {
      uint32_t cur_screen_h = xe::load_and_swap<uint32_t>(ui_obj + 0x48);
      uint32_t trans_state_h = xe::load_and_swap<uint32_t>(ui_obj + 0x2c);
      uint32_t trans_screen_h = xe::load_and_swap<uint32_t>(ui_obj + 0x4c);
      if (cur_screen_h != s_last_screen) { s_last_screen = cur_screen_h; s_screen_stable_count = 0; }
      else if (trans_state_h == 0) s_screen_stable_count++;

      // Blocker 2 (song content not ready): the song .milo / RndPropAnim
      // FileMerger merge runs ASYNC, driven by the MAIN thread's LoadMgr::Poll.
      // We must NOT force the loading->game_screen transition until that merge
      // completes, or HamDirector::Enter reads a half-built anim (corrupt
      // mSongAnims/mPropKeys -> operator[] crash + GetKeys hang). The merge is
      // in flight while TheFileMergerOrganizer(*0x82f5ef44)->mActiveOrg(+0x38)
      // is non-null. We canNOT pump LoadMgr ourselves (races the main thread ->
      // crash); we just HOLD the transition so the main thread can finish, then
      // release. (mActiveOrg per src/system/char/FileMergerOrganizer.h:67.)
      bool merge_busy = false;
      {
        uint32_t org = is_guest_readable(0x82f5ef44, 4)
                           ? xe::load_and_swap<uint32_t>(
                                 memory->TranslateVirtual<uint8_t*>(0x82f5ef44))
                           : 0;
        if (org && is_guest_readable(org + 0x38, 4)) {
          merge_busy = xe::load_and_swap<uint32_t>(
                           memory->TranslateVirtual<uint8_t*>(org + 0x38)) != 0;
        }
        static bool s_merge_seen_busy = false;
        if (merge_busy) s_merge_seen_busy = true;
        // Only gate once we've actually observed a merge start (avoids holding
        // forever if the organizer is idle for unrelated reasons before the
        // song load is even queued).
        if (!s_merge_seen_busy) merge_busy = false;
        if ((s_ticks % 120) == 0) {
          XELOGI("DC3: merge_busy={} org={:08X} seenBusy={}", merge_busy ? 1 : 0,
                 org, s_merge_seen_busy ? 1 : 0);
        }
      }

      auto read_guest_name = [&](uint32_t name_ptr, bool strict_scan_range)
          -> std::string {
        if (!name_ptr || name_ptr >= 0xF0000000) {
          return "";
        }
        if (strict_scan_range &&
            (name_ptr < s_scan_name_min || name_ptr >= s_scan_name_max)) {
          return "";
        }
        std::string result;
        result.reserve(32);
        for (uint32_t i = 0; i < 64; ++i) {
          if (!is_guest_readable(name_ptr + i, 1)) {
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

      auto read_name_ptr_at = [&](uint32_t screen_addr, uint32_t offset,
                                  bool strict_scan_range) -> uint32_t {
        if (!screen_addr || screen_addr >= 0xF0000000) {
          return 0;
        }
        if (!is_guest_readable(screen_addr + offset, 4)) {
          return 0;
        }
        auto* screen = memory->TranslateVirtual<uint8_t*>(screen_addr);
        uint32_t name_ptr = xe::load_and_swap<uint32_t>(screen + offset);
        if (read_guest_name(name_ptr, strict_scan_range).empty()) {
          return 0;
        }
        return name_ptr;
      };

      auto read_name_at = [&](uint32_t screen_addr, uint32_t offset,
                              bool strict_scan_range) -> std::string {
        return read_guest_name(
            read_name_ptr_at(screen_addr, offset, strict_scan_range),
            strict_scan_range);
      };
      auto find_name_literal_ptr = [&](const std::string& target_name)
          -> uint32_t {
        auto it = s_name_literal_cache.find(target_name);
        if (it != s_name_literal_cache.end() &&
            read_guest_name(it->second, false) == target_name) {
          return it->second;
        }

        auto scan_for_literal = [&](uint32_t range_min,
                                    uint32_t range_max) -> uint32_t {
          if (!range_min || range_min >= range_max) {
            return 0;
          }
          const size_t target_len = target_name.size();
          for (uint32_t addr = range_min; addr + target_len < range_max;
               ++addr) {
            if (!is_guest_readable(addr, static_cast<uint32_t>(target_len + 1))) {
              continue;
            }
            auto* candidate = memory->TranslateVirtual<uint8_t*>(addr);
            if (!candidate) {
              continue;
            }
            if (std::memcmp(candidate, target_name.data(), target_len) == 0 &&
                candidate[target_len] == 0) {
              s_name_literal_cache[target_name] = addr;
              return addr;
            }
          }
          return 0;
        };

        uint32_t literal_ptr = 0;
        if (s_scan_name_min && s_scan_name_max && s_scan_name_min < s_scan_name_max) {
          literal_ptr = scan_for_literal(s_scan_name_min, s_scan_name_max);
        }
        if (!literal_ptr) {
          literal_ptr = scan_for_literal(0x82000000, 0x82400000);
        }
        return literal_ptr;
      };

      std::string raw_name = read_name_at(cur_screen_h, 0x1C, false);
      if (raw_name.empty()) {
        raw_name = read_name_at(cur_screen_h, 0x20, false);
      }
      std::string raw_trans_name = read_name_at(trans_screen_h, 0x1C, false);
      if (raw_trans_name.empty()) {
        raw_trans_name = read_name_at(trans_screen_h, 0x20, false);
      }
      std::string cur_name = raw_name;
      if (cur_name.empty()) {
        cur_name = "attract_screen";
      }
      std::string trans_name = raw_trans_name;

      auto* processor = kernel_state->processor();
      auto* thread_state = ts;

      // (RETIRED 2026-10-02, lane B2) seq.transition_force: after 120 ticks
      // in a UIManager transition, the host stored mTransitionState /
      // mCurrentScreen / mTransitionScreen and Executed UIScreen::Enter. With
      // the real Bink/Splash/HamAudio paths every transition completes by
      // itself (S1 5/5 with it off; docs/fork/dc3/BASELINE.md).

      // (try_bootstrap_gameplay lambda deleted -- retired experiment;
      // see the NOTE below about GamePanel::CreateGame blocking. fork-cleanup C.)

      if ((s_ticks % 60) == 0) {
        XELOGI("DC3: Nav diag: scr={:08X} raw='{}' name='{}' stable={} nui={}",
               cur_screen_h, raw_name, cur_name, s_screen_stable_count,
               s_ticks);
      }

      // NOTE (2026-06-02): tried pumping LoadMgr::Poll(&TheLoadMgr) here to drain
      // the song merge before game_screen — it RACES the main thread's own load
      // polling (confirmed: crash in LoadMgr::Poll->PollFrontLoader->
      // DataArray::Node @0x8259F758 with LoadMgr frames on the stack). The main
      // thread DOES drive LoadMgr::Poll, so this is a timing/merge-completeness
      // problem (game transitions before the merge finishes), NOT a missing
      // driver. The safe fix is to HOLD the loading->game_screen transition until
      // the song FileMerger merge completes (host-readable signal:
      // TheFileMergerOrganizer @0x82f5ef44 idle, or the song Merger::mLoaded
      // set), with NO concurrent polling. See task #21.

      int nav_stable_threshold = 20;
      // IK telemetry: use default thresholds, the nav bridge is needed
      // because scripted input A-presses don't trigger game transitions
      // (DC3 uses Kinect, not standard XInput for menu navigation).
      if (cur_name == "title_screen") {
        // Let the scripted A presses try first; only force past title after
        // it's been idle for a few seconds.
        nav_stable_threshold = 180;
      } else if (cur_name == "main_screen" ||
                 cur_name == "choose_mode_screen") {
        // Menu A-button input still flakes in the original-XEX path. Give the
        // scripted controller presses time to work before using the same
        // UIManager::GotoScreen bridge that gets us through the boot flow.
        nav_stable_threshold = 160;
      } else if (cur_name == "song_select_screen") {
        // Let the scripted DOWN/A sequence try to select a real song first.
        // If the screen never leaves song select, fall back to the same
        // headless bridge chain used by older boot probes.
        nav_stable_threshold = 260;
      } else if (cur_name == "multiuser_screen" ||
                 cur_name == "loading_screen" ||
                 cur_name == "preloading_screen" ||
                 cur_name == "real_loading_screen") {
        nav_stable_threshold = 120;
      } else if (cur_name == "wait_main_after_saveload_screen") {
        // Save/load is stubbed in the original-XEX headless path, so if the
        // screen settles without firing its completion handler, advance to the
        // real menu flow after a short grace period.
        nav_stable_threshold = 120;
      }

      if (kHackNavBridge &&
          s_screen_stable_count >= nav_stable_threshold && trans_state_h == 0) {
        std::string target_name;
        if (cur_screen_h && cur_name == "attract_screen") {
          target_name = "title_screen";
        } else if (cur_screen_h && cur_name == "title_screen") {
          target_name = "wait_main_after_saveload_screen";
        } else if (cur_screen_h && cur_name == "wait_main_after_saveload_screen") {
          target_name = "main_screen";
        } else if (cur_screen_h && cur_name == "main_screen") {
          target_name = "choose_mode_screen";
        } else if (cur_screen_h && cur_name == "choose_mode_screen") {
          target_name = "song_select_screen";
        } else if (cur_screen_h && cur_name == "song_select_screen") {
          target_name = "multiuser_screen";
        } else if (cur_screen_h && cur_name == "multiuser_screen") {
          target_name = "loading_screen";
        } else if (cur_screen_h &&
                   (cur_name == "loading_screen" ||
                    cur_name == "preloading_screen" ||
                    cur_name == "real_loading_screen")) {
          // Skip intermediate loading screens — go straight to game_screen,
          // but ONLY once the song FileMerger merge is done (merge_busy==false).
          // Holding here lets the main thread's LoadMgr::Poll finish the merge
          // so HamDirector::Enter sees a fully-built anim. (task #21)
          if (!merge_busy) {
            target_name = "game_screen";
          } else if ((s_ticks % 120) == 0) {
            XELOGI("DC3: HOLD at '{}' — song merge still busy, not advancing",
                   cur_name);
          }
        }

        if (!target_name.empty()) {
          uint32_t found_screen = 0;
          uint32_t found_name_ptr = 0;
          for (int scan_pass = 0; scan_pass < 2 && !found_screen; ++scan_pass) {
            bool strict_scan_range = scan_pass == 0;
            if (scan_pass == 1) {
              XELOGI("DC3: Nav scan retrying '{}' without .rdata fence",
                     target_name);
            }
            for (uint32_t s_base = 0x40C00000;
                 s_base < 0x41000000 && !found_screen; s_base += 0x10000) {
              if (!is_guest_readable(s_base, 1)) {
                continue;
              }
              for (uint32_t addr = s_base;
                   addr < s_base + 0x10000 && !found_screen; addr += 4) {
                if (!is_guest_readable(addr + 0x20, 4)) {
                  continue;
                }
                uint32_t candidate_name_ptr =
                    read_name_ptr_at(addr, 0x1C, strict_scan_range);
                std::string candidate =
                    read_guest_name(candidate_name_ptr, strict_scan_range);
                if (candidate.empty()) {
                  candidate_name_ptr =
                      read_name_ptr_at(addr, 0x20, strict_scan_range);
                  candidate =
                      read_guest_name(candidate_name_ptr, strict_scan_range);
                }
                if (candidate == target_name) {
                  found_screen = addr;
                  found_name_ptr = candidate_name_ptr;
                  break;
                }
              }
            }
          }
          if (!found_name_ptr) {
            found_name_ptr = find_name_literal_ptr(target_name);
            if (found_name_ptr) {
              XELOGI("DC3: Nav literal: {} -> {} (name={:08X})", cur_name,
                     target_name, found_name_ptr);
            }
          }
          if (found_screen || found_name_ptr) {
            HackFired("seq.nav_bridge");
          }
          if (found_screen && found_name_ptr) {
            if (processor && thread_state) {
              if (target_name == "game_screen" &&
                  cvars::dc3_game_screen_real_goto) {
                // Real path: drive the genuine UIManager::GotoScreen so
                // game_panel->Load() runs CreateGame() (new Game()) and the
                // per-frame GamePanel::Poll() -> Game::HandleWait() state machine
                // fires SetupAnims()/OnSongLoaded()/StartGame() -> animating
                // dancer. This previously blocked because the song FileMerger
                // merge wasn't done; we only reach here once merge_busy==false
                // (see the HOLD above), so GotoScreen can complete. (task #21)
                constexpr uint32_t kUIManagerGotoScreenByName = 0x8277B378;
                XELOGI("DC3: Nav goto (real, game_screen): {} -> {} "
                       "({:08X}, name={:08X})",
                       cur_name, target_name, found_screen, found_name_ptr);
                uint64_t args[4] = {ui_addr, found_name_ptr, 0, 0};
                processor->Execute(thread_state, kUIManagerGotoScreenByName,
                                   args, 4);
              } else if (target_name == "game_screen") {
                // Fallback (dc3_game_screen_real_goto=false): host force-set the
                // UIManager screen pointers directly. Reaches game_screen visually
                // but never creates the Game (no Load()/CreateGame()), so no
                // animating dancer. Kept for A/B comparison.
                xe::store_and_swap<uint32_t>(ui_obj + 0x48, found_screen);
                xe::store_and_swap<uint32_t>(ui_obj + 0x4C, 0);
                xe::store_and_swap<uint32_t>(ui_obj + 0x2C, 0);
                XELOGI("DC3: Nav force-set: {} -> {} ({:08X}, name={:08X})",
                       cur_name, target_name, found_screen, found_name_ptr);
                cur_screen_h = found_screen;
                trans_state_h = 0;
                trans_screen_h = 0;
                cur_name = target_name;
                raw_name = target_name;
                trans_name.clear();
                raw_trans_name.clear();
              } else {
                constexpr uint32_t kUIManagerGotoScreenByName = 0x8277B378;
                XELOGI("DC3: Nav goto: {} -> {} ({:08X}, name={:08X})",
                       cur_name, target_name, found_screen, found_name_ptr);
                uint64_t args[4] = {ui_addr, found_name_ptr, 0, 0};
                processor->Execute(thread_state, kUIManagerGotoScreenByName,
                                   args, 4);
              }
            } else {
              XELOGW(
                  "DC3: Nav goto skipped for '{}' (processor/thread_state missing)",
                  target_name);
            }
            s_screen_stable_count = 0;
          } else if (found_name_ptr) {
            if (processor && thread_state) {
              if (target_name == "game_screen" &&
                  cvars::dc3_game_screen_real_goto) {
                constexpr uint32_t kUIManagerGotoScreenByName = 0x8277B378;
                XELOGI("DC3: Nav goto by literal (real, game_screen): {} -> {} "
                       "(name={:08X})",
                       cur_name, target_name, found_name_ptr);
                uint64_t args[4] = {ui_addr, found_name_ptr, 0, 0};
                processor->Execute(thread_state, kUIManagerGotoScreenByName,
                                   args, 4);
              } else if (target_name == "game_screen") {
                XELOGI("DC3: Nav force-set by literal: {} -> {} (name={:08X})",
                       cur_name, target_name, found_name_ptr);
                // Can't force-set without found_screen, fall through
              } else {
                constexpr uint32_t kUIManagerGotoScreenByName = 0x8277B378;
                XELOGI("DC3: Nav goto by literal: {} -> {} (name={:08X})",
                       cur_name, target_name, found_name_ptr);
                uint64_t args[4] = {ui_addr, found_name_ptr, 0, 0};
                processor->Execute(thread_state, kUIManagerGotoScreenByName,
                                   args, 4);
              }
              s_screen_stable_count = 0;
            } else {
              XELOGW(
                  "DC3: Nav goto-by-literal skipped for '{}' (processor/thread_state missing)",
                  target_name);
            }
          } else if (found_screen) {
            XELOGW("DC3: Nav target '{}' found at {:08X} without usable name ptr",
                   target_name, found_screen);
          } else {
            XELOGW("DC3: Nav target '{}' unresolved from '{}'", target_name,
                   cur_name);
          }
        }
      }

      // (RETIRED 2026-10-02, lane B2) seq.loadsong_repair: at loading_screen
      // the host Executed DataReadFile + HamSongMgr::AddSongs on a guessed
      // songs.dta path and constructed the 'ymca' Symbol into
      // HamGameData+0x30. The scripted flow selects the song on song_select,
      // so it is already set when loading_screen is reached (S1 5/5 with it
      // off; docs/fork/dc3/BASELINE.md).

      // Gameplay bootstrap is disabled — GamePanel::CreateGame blocks
      // because it tries to load song/character resources via async I/O
      // that depend on ARK file content not fully accessible in Xenia.
      // IK telemetry capture requires the full gameplay pipeline running,
      // which in turn requires working ARK loading for all game assets.

      // (RETIRED 2026-10-02) beat drive A: 1/30 s per tick written into the
      // TheTaskMgr seconds/beats/ui timelines on game_screen. With real XMA
      // contexts and the paced nop driver the game's own song clock runs
      // (gpState=3 on the audio clock, docs/fork/dc3/BASELINE.md).
    }
  }
}

void AutonavTask(cpu::ThreadState* ts, uint64_t) {
  // One step per >= 33 ms: the cadence of the NUI frames the thresholds
  // below were tuned on (the SkeletonUpdate worker's 33 ms wait).
  static auto s_last_step = std::chrono::steady_clock::time_point{};
  auto now = std::chrono::steady_clock::now();
  if (now - s_last_step < std::chrono::milliseconds(33)) {
    return;
  }
  s_last_step = now;
  AutonavStep(ts);
}

}  // namespace

bool AutonavEnabled() { return cvars::dc3_headless_autonav; }

void InstallAutonav(cpu::Processor* processor, Memory* memory,
                    kernel::KernelState* kernel_state) {
  if (!cvars::dc3_headless_autonav) {
    XELOGI("DC3: headless autonav off (--dc3_headless_autonav=false)");
    return;
  }
  g_processor = processor;
  g_memory = memory;
  g_kernel_state = kernel_state;
  AddMainThreadTask(processor, memory, "headless_autonav", &AutonavTask);
}

}  // namespace dc3
}  // namespace xe
