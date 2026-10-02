/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 NuiSkeletonGetNextFrame sequencer override (NOT upstream).
 *
 * Dc3NuiSequencerExtern moved verbatim out of emulator.cc.
 ******************************************************************************
 */

#include "xenia/titles/dc3/dc3_nui_sequencer.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#if XE_PLATFORM_LINUX
#include <sys/mman.h>
#include <cerrno>
#endif

#include "xenia/config.h"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/apu/apu_flags.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/nop/nop_input_driver.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/titles/dc3/dc3_flags.h"
#include "xenia/titles/dc3/dc3_hack_pack.h"
#include "xenia/titles/dc3/dc3_nui_patch_resolver.h"
#include "xenia/titles/dc3/dc3_runtime_telemetry.h"

namespace xe {

using namespace xe::literals;
using namespace xe::dc3;

void Dc3NuiSequencerExtern(
    cpu::ppc::PPCContext* ppc_context, kernel::KernelState* kernel_state) {
  uint32_t frame_guest_addr = static_cast<uint32_t>(ppc_context->r[4]);
  Memory* memory = kernel_state->memory();
  static int s_skel_calls = 0;
  static uint32_t s_last_screen = 0;
  static int s_screen_stable_count = 0;
  static uint32_t s_fake_frame_number = 0;
  static bool s_nui_entry_logged = false;
  static bool s_screen_name_scan_range_logged = false;
  static uint32_t s_scan_name_min = 0;
  static uint32_t s_scan_name_max = 0;
  static std::unordered_map<std::string, uint32_t> s_name_literal_cache;
  static bool s_loadsong_probe_logged = false;
  static bool s_loadsong_repair_attempted = false;
  static bool s_content_refresh_forced = false;
  static bool s_host_beat_drive_active = false;
  static float s_host_song_seconds = 0.0f;
  static float s_host_song_beat = 0.0f;
  static uint32_t s_last_stuck_cur_screen = 0;
  static uint32_t s_last_stuck_trans_screen = 0;
  static uint32_t s_last_stuck_trans_state = 0;
  static int s_stuck_transition_count = 0;

  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }

  if (!frame_guest_addr) {
    ppc_context->r[3] = 0x80004003u;
    return;
  }

  auto* frame = memory->TranslateVirtual<uint8_t*>(frame_guest_addr);
  if (!frame) {
    ppc_context->r[3] = 0x80004005u;
    return;
  }

  auto write_u32 = [frame](uint32_t offset, uint32_t value) {
    xe::store_and_swap<uint32_t>(frame + offset, value);
  };
  auto write_u64 = [frame](uint32_t offset, uint64_t value) {
    xe::store_and_swap<uint64_t>(frame + offset, value);
  };
  auto write_float = [frame](uint32_t offset, float value) {
    xe::store_and_swap<float>(frame + offset, value);
  };

  uint32_t frame_number = ++s_fake_frame_number;
  constexpr uint32_t kFrameSize = 0x30 + 6 * 0x1B4;
  std::memset(frame, 0, kFrameSize);

  write_u64(0x00, static_cast<uint64_t>(frame_number) * 33333);
  write_u32(0x08, frame_number);
  write_float(0x10, 0.0f);
  write_float(0x14, 1.0f);
  write_float(0x18, 0.0f);
  write_float(0x1C, 0.0f);
  write_float(0x20, 0.0f);
  write_float(0x24, 1.0f);
  write_float(0x28, 0.0f);
  write_float(0x2C, 0.0f);

  constexpr uint32_t kSkel0 = 0x30;
  write_u32(kSkel0 + 0x00, 2);
  write_u32(kSkel0 + 0x04, 1);
  write_float(kSkel0 + 0x10, 0.0f);
  write_float(kSkel0 + 0x14, 0.9f);
  write_float(kSkel0 + 0x18, 2.0f);
  write_float(kSkel0 + 0x1C, 0.0f);

  struct JointPos {
    float x;
    float y;
    float z;
  };
  static constexpr JointPos kJoints[20] = {
      {0.00f, 0.90f, 2.0f},  {0.00f, 1.10f, 2.0f},  {0.00f, 1.40f, 2.0f},
      {0.00f, 1.60f, 2.0f},  {-0.20f, 1.40f, 2.0f}, {-0.30f, 1.10f, 2.0f},
      {-0.25f, 0.90f, 2.0f}, {-0.20f, 0.80f, 2.0f}, {0.20f, 1.40f, 2.0f},
      {0.30f, 1.10f, 2.0f},  {0.25f, 0.90f, 2.0f},  {0.20f, 0.80f, 2.0f},
      {-0.10f, 0.90f, 2.0f}, {-0.10f, 0.50f, 2.0f}, {-0.10f, 0.10f, 2.0f},
      {-0.10f, 0.00f, 2.0f}, {0.10f, 0.90f, 2.0f},  {0.10f, 0.50f, 2.0f},
      {0.10f, 0.10f, 2.0f},  {0.10f, 0.00f, 2.0f},
  };
  constexpr uint32_t kJointsOff = kSkel0 + 0x20;
  for (int j = 0; j < 20; ++j) {
    uint32_t off = kJointsOff + j * 16;
    write_float(off + 0, kJoints[j].x);
    write_float(off + 4, kJoints[j].y);
    write_float(off + 8, kJoints[j].z);
    write_float(off + 12, 0.0f);
  }

  s_skel_calls++;
  ppc_context->r[3] = 0;

  if (!s_nui_entry_logged) {
    XELOGI("DC3: NUI callback alive (original layout) frameBuf={:08X} "
           "fakeFrame={} nuiFrame={}",
           frame_guest_addr, frame_number, s_skel_calls);
    s_nui_entry_logged = true;
  }

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

  // Force GestureMgr.mInControllerMode=true so the game processes
  // XInput button presses for menu navigation (DC3 normally uses Kinect).
  {
    constexpr uint32_t kTheGestureMgr = 0x82F5F7B4;
    constexpr uint32_t kInControllerModeOff = 0x426D;
    auto* gm_slot = memory->TranslateVirtual<uint8_t*>(kTheGestureMgr);
    if (gm_slot) {
      uint32_t gm_addr = xe::load_and_swap<uint32_t>(gm_slot);
      if (gm_addr && gm_addr < 0xF0000000) {
        auto* gm = memory->TranslateVirtual<uint8_t*>(gm_addr);
        if (gm) {
          gm[kInControllerModeOff] = 1;
        }
      }
    }
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
        if ((s_skel_calls % 120) == 0) {
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
      auto* thread_state = ppc_context->thread_state;
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

      if (trans_state_h != 0 && trans_screen_h && processor && thread_state &&
          (s_stuck_transition_count == 1 ||
           (s_stuck_transition_count % 60) == 0)) {
        constexpr uint32_t kUIScreenEntering = 0x827A34F8;
        constexpr uint32_t kUIScreenExiting = 0x827A35C0;
        constexpr uint32_t kUIScreenCheckIsLoaded = 0x827A3A00;
        int trans_loaded = -1;
        int cur_exiting = -1;
        int cur_entering = -1;
        if (trans_state_h == 1) {
          trans_loaded =
              exec_guest_bool(kUIScreenCheckIsLoaded, trans_screen_h) ? 1 : 0;
          cur_exiting = cur_screen_h
                            ? (exec_guest_bool(kUIScreenExiting, cur_screen_h) ? 1
                                                                                : 0)
                            : 0;
          cur_entering =
              exec_guest_bool(kUIScreenEntering, trans_screen_h) ? 1 : 0;
        } else if (trans_state_h == 2) {
          cur_entering = cur_screen_h
                             ? (exec_guest_bool(kUIScreenEntering, cur_screen_h) ? 1
                                                                                 : 0)
                             : 0;
        }
        XELOGI(
            "DC3: Transition diag cur='{}' trans='{}' state={} stable={} "
            "loaded={} curExiting={} curEntering={}",
            cur_name, trans_name, trans_state_h, s_stuck_transition_count,
            trans_loaded, cur_exiting, cur_entering);
      }

      if (trans_state_h == 1 && trans_screen_h && processor && thread_state &&
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

      if (trans_state_h == 2 && cur_screen_h && processor && thread_state &&
          s_stuck_transition_count >= 120) {
        constexpr uint32_t kUIScreenEntering = 0x827A34F8;
        bool cur_entering = exec_guest_bool(kUIScreenEntering, cur_screen_h);
        if (cur_entering || s_stuck_transition_count >= 240) {
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

      if ((s_skel_calls % 60) == 0) {
        XELOGI("DC3: Nav diag: scr={:08X} raw='{}' name='{}' stable={} nui={}",
               cur_screen_h, raw_name, cur_name, s_screen_stable_count,
               s_skel_calls);
      }

      // GATE PROBE (diagnostic): when at game_screen but the GamePanel state
      // machine hasn't entered intro/playing (mState==0), poll the three
      // PollForLoading gates from the host so we can see which one blocks
      // mPollLoadState from reaching 4 (which gates GamePanel::Poll's
      // StartIntro/StartGame). Pure-read guest fns, fired a few times only.
      static int s_gate_probe_count = 0;
      if (cvars::dc3_gameplay_probe && cur_name == "game_screen" &&
          (s_skel_calls % 120) == 0 && s_gate_probe_count < 64) {
        auto* gp_probe = kernel_state->processor();
        auto* ts_probe = ppc_context->thread_state;
        if (gp_probe && ts_probe) {
          constexpr uint32_t kTheGamePanelPtr = 0x83117410;
          constexpr uint32_t kTheHamDirectorPtr = 0x82F603A0;
          constexpr uint32_t kTheHamWardrobePtr = 0x82F60110;
          constexpr uint32_t kIsWorldLoaded = 0x82468838;
          constexpr uint32_t kAllCharsLoaded = 0x824553A0;
          constexpr uint32_t kGameIsReady = 0x82867F80;
          constexpr uint32_t kGamePanelIsLoaded = 0x8287AD78;
          constexpr uint32_t kUIPanelIsLoaded = 0x827A7190;
          auto rd = [&](uint32_t a) -> uint32_t {
            return is_guest_readable(a, 4)
                       ? xe::load_and_swap<uint32_t>(
                             memory->TranslateVirtual<uint8_t*>(a))
                       : 0;
          };
          uint32_t gp = rd(kTheGamePanelPtr);
          uint32_t hd = rd(kTheHamDirectorPtr);
          uint32_t hw = rd(kTheHamWardrobePtr);
          uint32_t game = (gp && is_guest_readable(gp + 0x38, 4))
                              ? rd(gp + 0x38)
                              : 0;
          uint32_t world_loaded = 0, chars_loaded = 0, game_ready = 0;
          if (hd) {
            uint64_t a[1] = {hd};
            world_loaded = static_cast<uint32_t>(
                gp_probe->Execute(ts_probe, kIsWorldLoaded, a, 1));
          }
          if (hw) {
            uint64_t a[1] = {hw};
            chars_loaded = static_cast<uint32_t>(
                gp_probe->Execute(ts_probe, kAllCharsLoaded, a, 1));
          }
          if (game) {
            uint64_t a[1] = {game};
            game_ready = static_cast<uint32_t>(
                gp_probe->Execute(ts_probe, kGameIsReady, a, 1));
          }
          uint32_t panel_loaded = 0, uipanel_loaded = 0, mstate = 0xFFFF;
          if (gp) {
            uint64_t a[1] = {gp};
            panel_loaded = static_cast<uint32_t>(
                gp_probe->Execute(ts_probe, kGamePanelIsLoaded, a, 1));
            uipanel_loaded = static_cast<uint32_t>(
                gp_probe->Execute(ts_probe, kUIPanelIsLoaded, a, 1));
            mstate = (gp && is_guest_readable(gp + 0x80, 4)) ? rd(gp + 0x80)
                                                             : 0xFFFF;
          }
          XELOGI("DC3: GATE PROBE gp={:08X} game={:08X} hd={:08X} hw={:08X} "
                 "worldLoaded={} charsLoaded={} gameReady={} panelLoaded={} "
                 "uiPanelLoaded={} mState={} transState={}",
                 gp, game, hd, hw, world_loaded & 0xFF, chars_loaded & 0xFF,
                 game_ready & 0xFF, panel_loaded & 0xFF, uipanel_loaded & 0xFF,
                 mstate, trans_state_h);

          // ---- PROPKEYS PROBE (diagnostic, pure memory reads) ------------
          // Goal: characterize the corrupt std::list<PropKeys*> mPropKeys that
          // RndPropAnim::GetKeys spins forever in. Walk HamDirector.mSongAnims
          // (std::map<Difficulty,RndPropAnim*> @ hd+0x5c, STLport _Rb_tree),
          // find diff 0's RndPropAnim, read mPropKeys (@ RndPropAnim+0x10,
          // STLport _List), and walk the node ring (capped at 64). Logs each
          // node's addr/next/prev/value(PropKeys*) and each PropKeys' vptr +
          // target(+0x10)/prop(+0x18)/exceptionID(+0x24). NEVER calls a guest
          // fn (GetKeys would hang); reading memory is safe.
          if (hd) {
            // STLport _Rb_tree base @ hd+0x5c:
            //   header._M_data = {color@+0, parent@+4, left@+8, right@+c},
            //   _M_node_count @ +0x10.  Map node: base{c,par,left,right}@0..c,
            //   then pair<const Difficulty,AnimPtr>: key(int)@+0x10, value=
            //   AnimPtr(=ObjRefConcrete<RndPropAnim>) @ +0x14, whose layout is
            //   {vptr@+0, next@+4, prev@+8, mObject(RndPropAnim*)@+0xc}.  So
            //   the REAL RndPropAnim* is at node+0x14+0xc = node+0x20.  Header
            //   is the end()/sentinel.
            const uint32_t map_base = hd + 0x5c;
            const uint32_t map_header = map_base;       // &header._M_data
            const uint32_t map_root = rd(map_base + 4); // header._M_parent
            const uint32_t map_count = rd(map_base + 0x10);
            XELOGI("DC3: PKPROBE mSongAnims hdr={:08X} root={:08X} count={}",
                   map_header, map_root, map_count);
            uint32_t song_anims[3] = {};
            uint32_t stack[16] = {};
            int stack_count = 0;
            if (map_root && map_root != map_header) {
              stack[stack_count++] = map_root;
            }
            for (int i = 0; i < 16 && stack_count > 0; i++) {
              uint32_t node = stack[--stack_count];
              if (!node || node == map_header || !is_guest_readable(node, 0x24)) {
                continue;
              }
              int32_t key = static_cast<int32_t>(rd(node + 0x10));
              uint32_t aptr_vptr = rd(node + 0x14);   // AnimPtr vptr
              uint32_t aptr_obj = rd(node + 0x20);    // AnimPtr.mObject
              uint32_t left = rd(node + 8);
              uint32_t right = rd(node + 0xc);
              XELOGI("DC3: PKPROBE   mapnode {:08X} key={} aptrVptr={:08X} "
                     "RndPropAnim*={:08X} L={:08X} R={:08X}",
                     node, key, aptr_vptr, aptr_obj, left, right);
              if (key >= 0 && key < 3) {
                song_anims[key] = aptr_obj;
              }
              if (right && right != map_header && stack_count < 16) {
                stack[stack_count++] = right;
              }
              if (left && left != map_header && stack_count < 16) {
                stack[stack_count++] = left;
              }
            }

            auto dump_prop_anim = [&](int diff, uint32_t song_anim) {
              XELOGI("DC3: PKPROBE diff{} RndPropAnim={:08X}", diff,
                     song_anim);
              if (!song_anim || !is_guest_readable(song_anim, 0x30)) {
                XELOGI("DC3: PKPROBE diff{} anim unreadable/NULL", diff);
                return;
              }
              uint32_t anim_vptr = rd(song_anim);
              // mPropKeys @ song_anim+0x10 is the embedded list sentinel head:
              //   head._M_next @ +0x10, head._M_prev @ +0x14.
              const uint32_t list_head = song_anim + 0x10;
              uint32_t first = rd(list_head);       // head._M_next
              uint32_t last = rd(list_head + 4);    // head._M_prev
              uint32_t word18 = rd(song_anim + 0x18);
              uint32_t word1c = rd(song_anim + 0x1c);
              XELOGI("DC3: PKPROBE diff{} anim={:08X} vptr={:08X} "
                     "listHead={:08X} head.next={:08X} head.prev={:08X} "
                     "word18={:08X} word1c={:08X}",
                     diff, song_anim, anim_vptr, list_head, first, last,
                     word18, word1c);
              // Walk the node ring. Node: next@+0, prev@+4, value(PropKeys*)@+8.
              // Proper termination: cur == list_head (returned to sentinel).
              uint32_t cur = first;
              int n = 0;
              bool reached_sentinel = false;
              uint32_t prev_seen = list_head;
              for (; n < 64; n++) {
                if (cur == list_head) {
                  reached_sentinel = true;
                  break;
                }
                if (!is_guest_readable(cur, 0xc)) {
                  XELOGI("DC3: PKPROBE   diff{} node[{}] {:08X} UNREADABLE "
                         "(prev node was {:08X}) -> CORRUPT",
                         diff, n, cur, prev_seen);
                  break;
                }
                uint32_t nxt = rd(cur);
                uint32_t prv = rd(cur + 4);
                uint32_t pk = rd(cur + 8);
                // For the PropKeys, read vptr + target(+0x10) prop(+0x18)
                // excID(+0x24).
                uint32_t pk_vptr = 0, pk_tgt = 0, pk_prop = 0, pk_exc = 0;
                bool pk_ok = (pk && is_guest_readable(pk, 0x28));
                if (pk_ok) {
                  pk_vptr = rd(pk);
                  pk_tgt = rd(pk + 0x10);
                  pk_prop = rd(pk + 0x18);
                  pk_exc = rd(pk + 0x24);
                }
                XELOGI("DC3: PKPROBE   diff{} node[{}] @{:08X} next={:08X} "
                       "prev={:08X} PropKeys={:08X} | pk_ok={} vptr={:08X} "
                       "tgt={:08X} prop={:08X} excID={}",
                       diff, n, cur, nxt, prv, pk, pk_ok ? 1 : 0, pk_vptr,
                       pk_tgt, pk_prop, pk_exc);
                prev_seen = cur;
                cur = nxt;
              }
              XELOGI("DC3: PKPROBE diff{} WALK DONE nodes={} reachedSentinel={} "
                     "(64=hit cap => RING NEVER RETURNS TO SENTINEL = the "
                     "GetKeys infinite loop)",
                     diff, n, reached_sentinel ? 1 : 0);
            };

            for (int diff = 0; diff < 3; diff++) {
              dump_prop_anim(diff, song_anims[diff]);
            }

            // Also walk mDancerFaceAnims @ hd+0x74 (same map layout). The
            // GetKeys hang is on the MAIN thread; the corrupt list may be a
            // face anim rather than a song anim, so dump those too. Tag with
            // diff+10 so the log lines are distinguishable (diff10/11/12).
            {
              const uint32_t fmap_base = hd + 0x74;
              const uint32_t fmap_header = fmap_base;
              const uint32_t fmap_root = rd(fmap_base + 4);
              const uint32_t fmap_count = rd(fmap_base + 0x10);
              XELOGI("DC3: PKPROBE mDancerFaceAnims hdr={:08X} root={:08X} "
                     "count={}",
                     fmap_header, fmap_root, fmap_count);
              uint32_t face_anims[3] = {};
              uint32_t fstack[16] = {};
              int fstack_count = 0;
              if (fmap_root && fmap_root != fmap_header) {
                fstack[fstack_count++] = fmap_root;
              }
              for (int i = 0; i < 16 && fstack_count > 0; i++) {
                uint32_t node = fstack[--fstack_count];
                if (!node || node == fmap_header ||
                    !is_guest_readable(node, 0x24)) {
                  continue;
                }
                int32_t key = static_cast<int32_t>(rd(node + 0x10));
                uint32_t aptr_obj = rd(node + 0x20);  // AnimPtr.mObject
                uint32_t left = rd(node + 8);
                uint32_t right = rd(node + 0xc);
                if (key >= 0 && key < 3) {
                  face_anims[key] = aptr_obj;
                }
                if (right && right != fmap_header && fstack_count < 16) {
                  fstack[fstack_count++] = right;
                }
                if (left && left != fmap_header && fstack_count < 16) {
                  fstack[fstack_count++] = left;
                }
              }
              for (int diff = 0; diff < 3; diff++) {
                dump_prop_anim(diff + 10, face_anims[diff]);
              }
            }

            // The GetKeys hang's r3(this)=HamDirector (0x406E8DA8/0x406E8D78),
            // so the RndPropAnim* being dereferenced is a WILD pointer aliasing
            // the HamDirector. The likely sources are the non-map anim ObjPtrs:
            //   mMasterClipAnim @ hd+0x8c  (ObjPtr<RndPropAnim>, .mObject@+0xc)
            //   mPlayer1RoutineBuilderAnim @ hd+0xa0
            //   mPlayer2RoutineBuilderAnim @ hd+0xb4
            // Dump their .mObject + each one's list. tags 20/21/22.
            {
              uint32_t master = rd(hd + 0x8c + 0xc);
              uint32_t rb1 = rd(hd + 0xa0 + 0xc);
              uint32_t rb2 = rd(hd + 0xb4 + 0xc);
              XELOGI("DC3: PKPROBE masterClipAnim={:08X} rbAnim1={:08X} "
                     "rbAnim2={:08X} (hd={:08X} -> if any == hd-region these "
                     "are the wild GetKeys 'this')",
                     master, rb1, rb2, hd);
              dump_prop_anim(20, master);
              dump_prop_anim(21, rb1);
              dump_prop_anim(22, rb2);
            }
          }
          // ---- end PROPKEYS PROBE ---------------------------------------

          s_gate_probe_count++;
        }
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

      if (s_screen_stable_count >= nav_stable_threshold && trans_state_h == 0) {
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
          } else if ((s_skel_calls % 120) == 0) {
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

      if (!s_loadsong_probe_logged && cur_name == "loading_screen") {
        auto* processor = kernel_state->processor();
        auto* thread_state = ppc_context->thread_state;
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

      if (cur_name == "game_screen") {
        // IK telemetry: the HolmesClientPoll override never fires (see
        // dc3_hack_pack.cc), but this NUI hook runs every frame.  Read the
        // always-fresh IK scratch slots + bone walk here, gated to ~2/sec so
        // the log stays readable.  ReadDc3IKTelemetry guards every guest read
        // and no-ops unless --dc3_ik_telemetry is set.
        if (cvars::dc3_ik_telemetry && (s_skel_calls % 30) == 0) {
          ReadDc3IKTelemetry(memory, static_cast<uint32_t>(s_skel_calls));
        }
        constexpr uint32_t kTheTaskMgr = 0x82F64A58;
        auto load_u32 = [&](uint32_t guest_addr) -> uint32_t {
          auto* ptr = is_guest_readable(guest_addr, 4)
                          ? memory->TranslateVirtual<uint8_t*>(guest_addr)
                          : nullptr;
          return ptr ? xe::load_and_swap<uint32_t>(ptr) : 0;
        };
        auto store_float = [&](uint32_t guest_addr, float value) {
          if (!is_guest_readable(guest_addr, 4)) {
            return false;
          }
          auto* ptr = memory->TranslateVirtual<uint8_t*>(guest_addr);
          if (!ptr) {
            return false;
          }
          xe::store_and_swap<float>(ptr, value);
          return true;
        };
        // Blocker 2 (gameplay crash): do not drive the song clock until the
        // Game has actually started playback. Game::PostWaitStart clears
        // mPaused (Game+0x5E) once its load/wait state machine completes;
        // driving earlier runs the gameplay pipeline over a not-ready audio
        // stream -> host SIGSEGV (the HamAudio resync / Voice path).
        // TheGamePanel(0x83117410)->mGame(+0x38)->mPaused(+0x5E).
        constexpr uint32_t kTheGamePanelGate = 0x83117410;
        uint32_t gp_gate = load_u32(kTheGamePanelGate);
        uint32_t game_gate =
            (gp_gate && is_guest_readable(gp_gate + 0x38, 4))
                ? load_u32(gp_gate + 0x38)
                : 0;
        bool beat_gate_ok = false;
        if (game_gate && is_guest_readable(game_gate + 0x5E, 1)) {
          auto* pp = memory->TranslateVirtual<uint8_t*>(game_gate + 0x5E);
          beat_gate_ok = pp ? (*pp == 0) : false;  // mPaused==0 -> playing
        }
        if (!beat_gate_ok && s_host_beat_drive_active) {
          XELOGI(
              "DC3: Beat gate closed (Game paused/not ready) -> suspend beat "
              "drive");
          s_host_beat_drive_active = false;
        }
        uint32_t timelines_addr = load_u32(kTheTaskMgr + 0x2C);
        if (beat_gate_ok && timelines_addr &&
            is_guest_readable(timelines_addr + 0x54, 4) &&
            is_guest_readable(kTheTaskMgr + 0x48, 1)) {
          auto* auto_ptr =
              memory->TranslateVirtual<uint8_t*>(kTheTaskMgr + 0x48);
          if (auto_ptr) {
            *auto_ptr = 0;
          }

          constexpr float kSecondsPerFrame = 1.0f / 30.0f;
          constexpr float kBeatPerFrame = 120.0f / 60.0f * kSecondsPerFrame;
          constexpr uint32_t kTimelineStride = 0x1C;
          constexpr uint32_t kTimeOff = 0x10;
          constexpr uint32_t kLastTimeOff = 0x14;

          uint32_t seconds_time_addr = timelines_addr + 0 * kTimelineStride + kTimeOff;
          uint32_t seconds_last_addr =
              timelines_addr + 0 * kTimelineStride + kLastTimeOff;
          uint32_t beats_time_addr = timelines_addr + 1 * kTimelineStride + kTimeOff;
          uint32_t beats_last_addr =
              timelines_addr + 1 * kTimelineStride + kLastTimeOff;
          uint32_t ui_time_addr = timelines_addr + 2 * kTimelineStride + kTimeOff;
          uint32_t ui_last_addr = timelines_addr + 2 * kTimelineStride + kLastTimeOff;

          auto load_float = [&](uint32_t guest_addr) -> float {
            auto* ptr = is_guest_readable(guest_addr, 4)
                            ? memory->TranslateVirtual<uint8_t*>(guest_addr)
                            : nullptr;
            return ptr ? xe::load_and_swap<float>(ptr) : 0.0f;
          };

          float old_seconds = load_float(seconds_time_addr);
          float old_beats = load_float(beats_time_addr);
          float old_ui = load_float(ui_time_addr);
          if (!s_host_beat_drive_active) {
            s_host_song_seconds = old_seconds;
            s_host_song_beat = old_beats;
            XELOGI(
                "DC3: Host-driven beat activated taskmgr={:08X} timelines={:08X} "
                "sec={:.3f} beat={:.3f}",
                kTheTaskMgr, timelines_addr, s_host_song_seconds,
                s_host_song_beat);
            s_host_beat_drive_active = true;
          }

          s_host_song_seconds += kSecondsPerFrame;
          s_host_song_beat += kBeatPerFrame;

          store_float(seconds_last_addr, old_seconds);
          store_float(seconds_time_addr, s_host_song_seconds);
          store_float(beats_last_addr, old_beats);
          store_float(beats_time_addr, s_host_song_beat);
          store_float(ui_last_addr, old_ui);
          store_float(ui_time_addr, s_host_song_seconds);

          if ((s_skel_calls % 120) == 0) {
            XELOGI("DC3: Beat drive sec={:.3f} beat={:.3f} nui={}",
                   s_host_song_seconds, s_host_song_beat, s_skel_calls);
          }
        }
      } else if (s_host_beat_drive_active) {
        XELOGI("DC3: Host-driven beat deactivated on '{}'", cur_name);
        s_host_beat_drive_active = false;
      }
    }
  }
}

}  // namespace xe
