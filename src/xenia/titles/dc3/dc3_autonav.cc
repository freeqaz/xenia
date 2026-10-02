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
            "guest MAIN thread (HolmesClientPollKeyboard hook): complete "
            "stuck UI transitions, walk attract -> ... -> game_screen with "
            "UIManager::GotoScreen, inject the ymca song at loading_screen, "
            "and drive the 120 BPM song clock on game_screen. Also arms the "
            "scripted-input attract A-press/force. Off: the game boots to its own "
            "screens and only the DTA channel / scripted pad drive it.",
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
  static bool s_loadsong_probe_logged = false;
  static bool s_loadsong_repair_attempted = false;
  static bool s_content_refresh_forced = false;
  static uint32_t s_last_stuck_cur_screen = 0;
  static uint32_t s_last_stuck_trans_screen = 0;
  static uint32_t s_last_stuck_trans_state = 0;
  static int s_stuck_transition_count = 0;

  static const bool kHackTransitionForce = HackGate(
      "seq.transition_force",
      "force-enter/complete UIManager transitions stuck >=120 ticks");
  static const bool kHackNavBridge = HackGate(
      "seq.nav_bridge",
      "UIManager::GotoScreen walk attract->...->game_screen (merge_busy hold)");
  static const bool kHackLoadSong = HackGate(
      "seq.loadsong_repair",
      "loading_screen probe + ymca song injection via guest Executes");

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
      auto exec_guest_bool = [&](uint32_t fn, uint64_t arg0,
                                 uint64_t arg1 = 0) -> bool {
        if (!processor || !thread_state || !fn) {
          return false;
        }
        uint64_t args[2] = {arg0, arg1};
        return static_cast<uint32_t>(processor->Execute(thread_state, fn, args,
                                                        arg1 ? 2 : 1)) != 0;
      };

      if (trans_state_h != 0 && trans_screen_h) {
        if (cur_screen_h == s_last_stuck_cur_screen &&
            trans_screen_h == s_last_stuck_trans_screen &&
            trans_state_h == s_last_stuck_trans_state) {
          ++s_stuck_transition_count;
        } else {
          s_last_stuck_cur_screen = cur_screen_h;
          s_last_stuck_trans_screen = trans_screen_h;
          s_last_stuck_trans_state = trans_state_h;
          s_stuck_transition_count = 1;
        }
      } else {
        s_last_stuck_cur_screen = 0;
        s_last_stuck_trans_screen = 0;
        s_last_stuck_trans_state = 0;
        s_stuck_transition_count = 0;
      }

      if (kHackTransitionForce &&
          trans_state_h == 1 && trans_screen_h && processor && thread_state &&
          s_stuck_transition_count >= 120) {
        constexpr uint32_t kUIScreenExiting = 0x827A35C0;
        constexpr uint32_t kUIScreenCheckIsLoaded = 0x827A3A00;
        constexpr uint32_t kUIScreenEnter = 0x827A51E0;
        bool trans_loaded = exec_guest_bool(kUIScreenCheckIsLoaded, trans_screen_h);
        bool cur_exiting =
            cur_screen_h ? exec_guest_bool(kUIScreenExiting, cur_screen_h) : false;
        bool allow_force_enter =
            trans_loaded && (!cur_exiting || s_stuck_transition_count >= 180) &&
            !(trans_name == "game_screen" && merge_busy);
        if (allow_force_enter) {
          HackFired("seq.transition_force");
          uint32_t old_cur_screen = cur_screen_h;
          xe::store_and_swap<uint32_t>(ui_obj + 0x2C, 2);
          xe::store_and_swap<uint32_t>(ui_obj + 0x48, trans_screen_h);
          xe::store_and_swap<uint32_t>(ui_obj + 0x4C, old_cur_screen);
          uint64_t enter_args[2] = {trans_screen_h, old_cur_screen};
          processor->Execute(thread_state, kUIScreenEnter, enter_args, 2);
          XELOGI(
              "DC3: Force-entered stuck transition '{}' -> '{}' after {} NUI frames "
              "(loaded={} curExiting={})",
              cur_name, trans_name, s_stuck_transition_count,
              trans_loaded ? 1 : 0, cur_exiting ? 1 : 0);
          s_last_screen = trans_screen_h;
          s_screen_stable_count = 0;
          s_last_stuck_cur_screen = 0;
          s_last_stuck_trans_screen = 0;
          s_last_stuck_trans_state = 0;
          s_stuck_transition_count = 0;
          cur_screen_h = trans_screen_h;
          trans_state_h = 2;
          trans_screen_h = old_cur_screen;
          raw_name = raw_trans_name;
          raw_trans_name = read_name_at(old_cur_screen, 0x1C, false);
          if (raw_trans_name.empty()) {
            raw_trans_name = read_name_at(old_cur_screen, 0x20, false);
          }
          cur_name = trans_name;
          trans_name = raw_trans_name;
        } else if (!trans_loaded && s_stuck_transition_count >= 120 &&
                   (trans_name != "game_screen" ||
                    (cvars::dc3_ik_telemetry && !merge_busy))) {
          HackFired("seq.transition_force");
          xe::store_and_swap<uint32_t>(ui_obj + 0x48, trans_screen_h);
          xe::store_and_swap<uint32_t>(ui_obj + 0x4C, 0);
          xe::store_and_swap<uint32_t>(ui_obj + 0x2C, 0);
          XELOGI(
              "DC3: Force-completed unloaded menu transition '{}' -> '{}' after {} NUI frames",
              cur_name, trans_name, s_stuck_transition_count);
          s_last_screen = trans_screen_h;
          s_screen_stable_count = 0;
          s_last_stuck_cur_screen = 0;
          s_last_stuck_trans_screen = 0;
          s_last_stuck_trans_state = 0;
          s_stuck_transition_count = 0;
          cur_screen_h = trans_screen_h;
          trans_state_h = 0;
          trans_screen_h = 0;
          raw_name = raw_trans_name;
          raw_trans_name.clear();
          cur_name = trans_name;
          trans_name.clear();
        }
      }

      if (kHackTransitionForce &&
          trans_state_h == 2 && cur_screen_h && processor && thread_state &&
          s_stuck_transition_count >= 120) {
        constexpr uint32_t kUIScreenEntering = 0x827A34F8;
        bool cur_entering = exec_guest_bool(kUIScreenEntering, cur_screen_h);
        if (cur_entering || s_stuck_transition_count >= 240) {
          HackFired("seq.transition_force");
          // Force-complete the entering phase.  If curEntering is false but
          // we've been stuck for 240+ NUI frames, the enter animation
          // already finished but transState was never cleared (common after
          // Force-entered transitions).
          xe::store_and_swap<uint32_t>(ui_obj + 0x2C, 0);
          xe::store_and_swap<uint32_t>(ui_obj + 0x4C, 0);
          XELOGI(
              "DC3: Force-completed stuck enter for '{}' after {} NUI frames"
              " (curEntering={})",
              cur_name, s_stuck_transition_count, cur_entering ? 1 : 0);
          s_last_screen = cur_screen_h;
          s_screen_stable_count = 0;
          s_last_stuck_cur_screen = 0;
          s_last_stuck_trans_screen = 0;
          s_last_stuck_trans_state = 0;
          s_stuck_transition_count = 0;
          trans_state_h = 0;
          trans_screen_h = 0;
          raw_trans_name.clear();
          trans_name.clear();
        }
      }

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

      if (kHackLoadSong &&
          !s_loadsong_probe_logged && cur_name == "loading_screen") {
        HackFired("seq.loadsong_repair");
        auto* processor = kernel_state->processor();
        auto* thread_state = ts;
        if (processor && thread_state) {
          constexpr uint32_t kTheGameData = 0x82F60034;
          constexpr uint32_t kTheContentMgr = 0x82F123BC;
          constexpr uint32_t kTheHamProvider = 0x82F601B4;
          constexpr uint32_t kTheHamSongMgr = 0x83118C6C;
          constexpr uint32_t kTheMoveMgr = 0x82F60308;
          constexpr uint32_t kTheGameMode = 0x83117710;
          constexpr uint32_t kMetaPerformerCurrent = 0x828CB8A8;
          constexpr uint32_t kDataReadFile = 0x825C1AD0;
          constexpr uint32_t kHamSongMgrAddSongs = 0x828C5BC0;
          constexpr uint32_t kHamSongMgrData = 0x828C5AD8;
          constexpr uint32_t kHamSongMgrSongAudioData = 0x828C62D0;
          constexpr uint32_t kHamSongMgrGetShortNameFromSongID = 0x828C7CE8;
          constexpr uint32_t kHamSongMgrGetSongIDFromShortName = 0x828C7DE0;
          constexpr uint32_t kHamGameDataSetAssociatedPadNum = 0x82452268;
          constexpr uint32_t kHamGameDataPlayer = 0x82451CB8;
          constexpr uint32_t kSymbolCtor = 0x827D37C8;

          auto load_u32 = [&](uint32_t guest_addr) -> uint32_t {
            auto* ptr = is_guest_readable(guest_addr, 4)
                            ? memory->TranslateVirtual<uint8_t*>(guest_addr)
                            : nullptr;
            return ptr ? xe::load_and_swap<uint32_t>(ptr) : 0;
          };
          auto alloc_guest_cstr = [&](const char* text) -> uint32_t {
            if (!text) {
              return 0;
            }
            size_t len = std::strlen(text) + 1;
            uint32_t guest_addr =
                memory->SystemHeapAlloc(static_cast<uint32_t>(len), 4);
            if (!guest_addr) {
              return 0;
            }
            auto* dst = memory->TranslateVirtual<uint8_t*>(guest_addr);
            if (!dst) {
              memory->SystemHeapFree(guest_addr);
              return 0;
            }
            std::memcpy(dst, text, len);
            return guest_addr;
          };
          auto try_direct_song_catalog_load = [&]() -> bool {
            struct PathCandidate {
              const char* path;
              const char* label;
            };
            constexpr PathCandidate kCandidates[] = {
                {"d:\\songs\\songs.dta", "disc_root"},
                {"devkit:\\songs\\gen\\songs.dtb", "devkit_dtb"},
                {"devkit:\\songs\\songs.dta", "devkit_dta"},
            };
            for (const auto& candidate : kCandidates) {
              XELOGI(
                  "DC3: LoadSong repair: probing song catalog '{}' [{}]",
                  candidate.path, candidate.label);
              uint32_t path_addr = alloc_guest_cstr(candidate.path);
              if (!path_addr) {
                XELOGW("DC3: LoadSong repair: failed to allocate guest path "
                       "for {}", candidate.label);
                continue;
              }
              uint64_t read_args[2] = {path_addr, 1};
              uint32_t data_arr = static_cast<uint32_t>(
                  processor->Execute(thread_state, kDataReadFile, read_args, 2));
              XELOGI("DC3: LoadSong repair: DataReadFile('{}') [{}] -> {:08X}",
                     candidate.path, candidate.label, data_arr);
              memory->SystemHeapFree(path_addr);
              if (!data_arr) {
                continue;
              }
              uint64_t add_args[2] = {kTheHamSongMgr, data_arr};
              processor->Execute(thread_state, kHamSongMgrAddSongs, add_args, 2);
              XELOGI("DC3: LoadSong repair: HamSongMgr::AddSongs({:08X}) "
                     "completed via {}",
                     data_arr, candidate.label);
              return true;
            }
            return false;
          };

          uint32_t gd_addr = load_u32(kTheGameData);
          uint32_t content_mgr_addr = load_u32(kTheContentMgr);
          uint32_t hp_addr = load_u32(kTheHamProvider);
          uint32_t mm_addr = load_u32(kTheMoveMgr);
          uint32_t gm_addr = load_u32(kTheGameMode);

          uint64_t meta_ret =
              processor->Execute(thread_state, kMetaPerformerCurrent, nullptr, 0);
          uint32_t meta_addr = static_cast<uint32_t>(meta_ret);

          uint32_t p0_addr = 0;
          uint32_t p1_addr = 0;
          if (gd_addr && gd_addr < 0xF0000000) {
            uint64_t p0_args[2] = {gd_addr, 0};
            uint64_t p1_args[2] = {gd_addr, 1};
            p0_addr = static_cast<uint32_t>(
                processor->Execute(thread_state, kHamGameDataPlayer, p0_args, 2));
            p1_addr = static_cast<uint32_t>(
                processor->Execute(thread_state, kHamGameDataPlayer, p1_args, 2));
          }

          uint32_t song_sym =
              (gd_addr && is_guest_readable(gd_addr + 0x30, 4))
                  ? load_u32(gd_addr + 0x30)
                  : 0;
          std::string song_name = read_guest_name(song_sym, false);
          if ((song_name.empty()) && !s_loadsong_repair_attempted && gd_addr) {
            s_loadsong_repair_attempted = true;
            XELOGI(
                "DC3: LoadSong repair: trying direct song catalog load "
                "(gd={:08X} content={:08X} ham_song_mgr={:08X})",
                gd_addr, content_mgr_addr, kTheHamSongMgr);
            bool direct_song_load_ok = try_direct_song_catalog_load();
            if (!direct_song_load_ok && !s_content_refresh_forced) {
              s_content_refresh_forced = true;
              // NOTE: ContentMgr::RefreshSynchronously blocks forever under
              // Xenia because content enumeration never completes. Skip it
              // and let the nav bridge force-advance through the loading
              // screens instead.
              XELOGW("DC3: LoadSong repair: direct song catalog load failed; "
                     "skipping ContentMgr::RefreshSynchronously (blocks forever)");
            }
            constexpr uint32_t kYmcaSongId = 7011;
            uint64_t short_name_args[4] = {gd_addr + 0x30, kTheHamSongMgr,
                                           kYmcaSongId, 0};
            processor->Execute(
                thread_state, kHamSongMgrGetShortNameFromSongID,
                short_name_args, 4);
            if (is_guest_readable(gd_addr + 0x30, 4)) {
              auto* song_slot = memory->TranslateVirtual<uint8_t*>(gd_addr + 0x30);
              song_sym = xe::load_and_swap<uint32_t>(song_slot);
              song_name = read_guest_name(song_sym, false);
              XELOGI(
                  "DC3: LoadSong repair: canonical song id {} -> {:08X} '{}'",
                  kYmcaSongId, song_sym, song_name);
            }

            uint32_t song_name_ptr = 0;
            if (song_name.empty()) {
              song_name_ptr = find_name_literal_ptr("ymca");
            }
            if (song_name.empty()) {
              if (song_name_ptr) {
                XELOGI(
                    "DC3: LoadSong repair: constructing song symbol from {:08X}",
                    song_name_ptr);
                uint64_t ctor_args[2] = {gd_addr + 0x30, song_name_ptr};
                processor->Execute(thread_state, kSymbolCtor, ctor_args, 2);
                song_sym = load_u32(gd_addr + 0x30);
                song_name = read_guest_name(song_sym, false);
              } else {
                XELOGW("DC3: LoadSong repair: guest literal 'ymca' not found");
              }
            }
            if (!song_name.empty()) {
              XELOGI(
                  "DC3: LoadSong repair: injected song={:08X} '{}'", song_sym,
                  song_name);
              uint64_t pad0_args[3] = {gd_addr, 0, 0};
              uint64_t pad1_args[3] = {gd_addr, 1, 1};
              processor->Execute(thread_state, kHamGameDataSetAssociatedPadNum,
                                 pad0_args, 3);
              processor->Execute(thread_state, kHamGameDataSetAssociatedPadNum,
                                 pad1_args, 3);

              if (gd_addr && gd_addr < 0xF0000000) {
                uint64_t p0_args[2] = {gd_addr, 0};
                uint64_t p1_args[2] = {gd_addr, 1};
                p0_addr = static_cast<uint32_t>(processor->Execute(
                    thread_state, kHamGameDataPlayer, p0_args, 2));
                p1_addr = static_cast<uint32_t>(processor->Execute(
                    thread_state, kHamGameDataPlayer, p1_args, 2));
              }
            }
          }
          uint32_t p0_char =
              (p0_addr && is_guest_readable(p0_addr + 0x44, 4))
                  ? load_u32(p0_addr + 0x44)
                  : 0;
          uint32_t p1_char =
              (p1_addr && is_guest_readable(p1_addr + 0x44, 4))
                  ? load_u32(p1_addr + 0x44)
                  : 0;
          uint32_t p0_diff =
              (p0_addr && is_guest_readable(p0_addr + 0x58, 4))
                  ? load_u32(p0_addr + 0x58)
                  : 0;
          uint32_t p1_diff =
              (p1_addr && is_guest_readable(p1_addr + 0x58, 4))
                  ? load_u32(p1_addr + 0x58)
                  : 0;
          uint32_t p0_pad =
              (p0_addr && is_guest_readable(p0_addr + 0x7C, 4))
                  ? load_u32(p0_addr + 0x7C)
                  : 0;
          uint32_t p1_pad =
              (p1_addr && is_guest_readable(p1_addr + 0x7C, 4))
                  ? load_u32(p1_addr + 0x7C)
                  : 0;
          uint32_t song_id = 0;
          uint32_t song_data = 0;
          uint32_t song_audio = 0;
          uint32_t default_outfit = 0;
          uint32_t default_venue = 0;
          if (!song_name.empty()) {
            uint64_t song_id_args[3] = {kTheHamSongMgr, song_sym, 0};
            song_id = static_cast<uint32_t>(processor->Execute(
                thread_state, kHamSongMgrGetSongIDFromShortName, song_id_args,
                3));
            if (song_id) {
              uint64_t data_args[2] = {kTheHamSongMgr, song_id};
              song_data = static_cast<uint32_t>(processor->Execute(
                  thread_state, kHamSongMgrData, data_args, 2));
              song_audio = static_cast<uint32_t>(processor->Execute(
                  thread_state, kHamSongMgrSongAudioData, data_args, 2));
              if (song_data && is_guest_readable(song_data + 0xC0, 4)) {
                default_outfit = load_u32(song_data + 0xC0);
              }
              if (song_data && is_guest_readable(song_data + 0xD0, 4)) {
                default_venue = load_u32(song_data + 0xD0);
              }
            }
          }

          XELOGI(
              "DC3: LoadSong probe gd={:08X} gm={:08X} hp={:08X} mm={:08X} mp={:08X} "
              "cm={:08X} "
              "p0={:08X} char={:08X} '{}' diff={} pad={} "
              "p1={:08X} char={:08X} '{}' diff={} pad={} "
              "song={:08X} '{}' id={} data={:08X} audio={:08X} "
              "default_outfit={:08X} '{}' venue={:08X} '{}'",
              gd_addr, gm_addr, hp_addr, mm_addr, meta_addr, content_mgr_addr,
              p0_addr, p0_char, read_guest_name(p0_char, false), p0_diff, p0_pad,
              p1_addr, p1_char, read_guest_name(p1_char, false), p1_diff, p1_pad,
              song_sym, song_name, song_id, song_data, song_audio,
              default_outfit, read_guest_name(default_outfit, false), default_venue,
              read_guest_name(default_venue, false));
          s_loadsong_probe_logged = true;
        }
      }

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
