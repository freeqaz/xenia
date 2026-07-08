/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/emulator.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string_view>
#include <set>
#include <unordered_map>

#if XE_PLATFORM_LINUX
#include <sys/mman.h>
#include <cerrno>
#endif

#include "config.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/error/en.h"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/apu/audio_system.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/backend/null_backend.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/milo_trace.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/dc3_hack_pack.h"
#include "xenia/dc3_nui_patch_resolver.h"
#include "xenia/dc3_runtime_telemetry.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/gameinfo_utils.h"
#include "xenia/kernel/util/xdbf_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xbdm/xbdm_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/memory.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/devices/stfs_container_device.h"
#include "xenia/vfs/virtual_file_system.h"

#ifndef XE_HEADLESS_BUILD
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#endif

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#endif  // XE_ARCH

DECLARE_int32(user_language);

DEFINE_double(time_scalar, 1.0,
              "Scalar used to speed or slow time (1x, 2x, 1/2x, etc).",
              "General");
DEFINE_string(
    launch_module, "",
    "Executable to launch from the .iso or the package instead of default.xex "
    "or the module specified by the game. Leave blank to launch the default "
    "module.",
    "General");

DEFINE_int32(dc3_crt_bisect_max, -1,
             "DC3: max CRT constructor index to allow (-1=disabled/all run, "
             "0=only index 0 runs, N=indices 0..N run). "
             "Used for binary search to find heap-corrupting constructor.",
             "DC3");
DEFINE_string(dc3_crt_skip_indices, "",
              "DC3: comma-separated list of CRT constructor indices to "
              "nullify. Supports ranges: '69,75,98-340'. "
              "Default empty = use dc3_crt_skip_nui.",
              "DC3");
DEFINE_bool(dc3_ik_telemetry, false, "DC3: enable IK telemetry", "DC3");
DEFINE_bool(rb3_mount_update, false,
            "RB3 boot-to-menu experiment: mount update: at the disc dir so RB3 "
            "finds update:\\gen\\patch_xbox.hdr (title-update content). Default "
            "OFF (inert for DC3 and normal runs).",
            "RB3");
DEFINE_bool(dc3_enable_gameplay_bootstrap, false,
            "DC3: experimental host-driven guest bootstrap for CreateGame/"
            "SetupAnims/OnSongLoaded/StartGame. Disabled by default because "
            "current call sites are unstable.",
            "DC3");
DEFINE_bool(dc3_game_screen_real_goto, true,
            "DC3: drive loading->game_screen via the real UIManager::GotoScreen "
            "(runs game_panel Load()->CreateGame() and the per-frame Poll state "
            "machine that creates the Game and sets up dancer anims). Only safe "
            "once the song FileMerger merge has completed (gated on merge_busy). "
            "Set false to fall back to the old host force-set (no Game created).",
            "DC3");
DEFINE_bool(dc3_gameplay_probe, false,
            "DC3: enable the host-side GATE PROBE / PKPROBE gameplay diagnostics "
            "(reads HamDirector anim state + walks mSongAnims/mPropKeys from the "
            "NUI thread). Off by default — it executes guest helper fns each "
            "game_screen frame, which perturbs timing. Was used to diagnose the "
            "RndPropAnim::GetKeys wrong-receiver hang; keep for recurrence.",
            "DC3");
DEFINE_bool(dc3_null_read_cache_stream, false, "DC3: null read cache", "DC3");
DEFINE_bool(dc3_crt_skip_nui, true,
            "DC3: auto-nullify NUI/Kinect SDK CRT constructors (indices "
            "75,98-101,210-328). These call unresolved internal NUI "
            "functions that corrupt the heap. Set false to disable.",
            "DC3");
DEFINE_bool(dc3_guest_overrides, true,
            "DC3: use guest extern overrides for eligible simple NUI/XBC "
            "stub-return functions (default cutover path; skips byte patching "
            "for registered entries; preserves fake_kinect_data "
            "NuiSkeletonGetNextFrame path). The legacy NUI/XBC byte-patch "
            "fallback path has been removed; false logs a warning and is "
            "ignored for this path.",
            "DC3");
DEFINE_bool(dc3_debug_read_cache_stream_step_override, false,
            "DC3: enable invasive ReadCacheStream step-by-step guest override "
            "for DTB debugging. WARNING: performs extra reads/seeks and can "
            "perturb checksum/parser behavior; use only in dedicated probe runs.",
            "DC3");
DEFINE_bool(dc3_debug_memmgr_assert_nop_bypass, false,
            "DC3: debug-only bypass of selected MemMgr Debug::Fail callsites "
            "(replaces assert calls with nop). Temporary progression tool only; "
            "can mask data/config corruption.",
            "DC3");
DEFINE_bool(dc3_debug_mempool_alloc_probe, false,
            "DC3: log-only probe for MemOrPoolAlloc. Captures caller LR, "
            "requested size, file/line/name args, and return value. "
            "Detailed logs only on failure or from known crash-path callers.",
            "DC3");
DEFINE_string(
    dc3_debug_findarray_override_mode, "off",
    "DC3: debug mode for DataArray::FindArray(Symbol,bool) override "
    "(off|log_only|stub_on_fail|null_on_fail|setupfont_fix). "
    "'setupfont_fix' enables an emulated SystemConfig(Symbol,Symbol) probe path "
    "that repairs the known Rnd::SetupFont bad 'font' key literal in some "
    "decomp builds. Use only for DC3 decomp runtime forensics/progression.",
    "DC3");
DEFINE_string(dc3_nui_patch_layout, "auto",
              "DC3: NUI/XBC patch address layout selector "
              "(auto|original|decomp). 'auto' uses the zero-padding heuristic "
              "and logs a .text fingerprint for future resolver matching.",
              "DC3");
DEFINE_string(dc3_nui_layout_fingerprint_original, "",
              "DC3: optional .text FNV1a64 fingerprint (hex) for original "
              "NUI/XBC patch layout selection in auto mode.",
              "DC3");
DEFINE_string(dc3_nui_layout_fingerprint_decomp, "",
              "DC3: optional .text FNV1a64 fingerprint (hex) for decomp "
              "NUI/XBC patch layout selection in auto mode.",
              "DC3");
DEFINE_string(
    dc3_nui_layout_fingerprint_cache_path, "",
    "DC3: optional fingerprint cache file with lines "
    "'original=<hex>' and/or 'decomp=<hex>' for auto layout selection.",
    "DC3");
DEFINE_string(
    dc3_nui_symbol_map_path, "",
    "DC3: optional symbol map manifest used by the NUI/XBC resolver "
    "(symbols.txt-style 'name = .text:0xADDR;'). If unset, a local "
    "dc3-decomp symbols.txt path is auto-probed.",
    "DC3");
DEFINE_string(dc3_nui_patch_resolver_mode, "hybrid",
              "DC3: NUI/XBC patch target resolver mode "
              "(hybrid|strict). hybrid uses manifest/symbol/signature "
              "resolution before catalog fallback; strict disables raw "
              "catalog fallback.",
              "DC3");
DEFINE_string(
    dc3_nui_patch_manifest_path, "",
    "DC3: optional machine-readable DC3 NUI/XBC patch manifest JSON "
    "(xenia_dc3_patch_manifest.json). Preferred over symbols.txt when present.",
    "DC3");
DEFINE_bool(dc3_nui_enable_signature_resolver, true,
            "DC3: enable signature-resolver hook for NUI/XBC patch targets "
            "(default on; used by hybrid/strict resolver modes).",
            "DC3");
DEFINE_bool(dc3_nui_signature_trace, false,
            "DC3: log runtime PPC words for NUI/XBC patch targets "
            "at catalog and resolved addresses (debugging signature resolver).",
            "DC3");
DEFINE_string(
    dc3_crash_snapshot_path, "",
    "DC3: optional JSON crash snapshot output path written from guest crash "
    "dumps (headless-friendly structured artifact for postmortem tools).",
    "DC3");

namespace xe {

using namespace xe::literals;

namespace {
using namespace xe::dc3;

const char* AccessOpName(Exception::AccessViolationOperation op) {
  switch (op) {
    case Exception::AccessViolationOperation::kRead:
      return "READ";
    case Exception::AccessViolationOperation::kWrite:
      return "WRITE";
    default:
      return "UNKNOWN";
  }
}

void MaybeWriteDc3CrashSnapshotJson(const Emulator* emulator, Exception* ex,
                                    kernel::XThread* current_thread,
                                    const cpu::Function* guest_function,
                                    const cpu::ppc::PPCContext* context) {
  if (cvars::dc3_crash_snapshot_path.empty() || !emulator || !ex ||
      !current_thread || !context) {
    return;
  }

  uint32_t guest_pc = 0;
  if (guest_function && guest_function->is_guest()) {
    guest_pc = static_cast<const cpu::GuestFunction*>(guest_function)
                   ->MapMachineCodeToGuestAddress(ex->pc());
  }

  uint64_t host_fault = 0;
  uint32_t guest_fault = 0;
  bool has_fault = ex->code() == Exception::Code::kAccessViolation;
  if (has_fault) {
    host_fault = ex->fault_address();
    if (emulator->memory() && emulator->memory()->virtual_membase()) {
      uint64_t membase =
          reinterpret_cast<uintptr_t>(emulator->memory()->virtual_membase());
      guest_fault = static_cast<uint32_t>(host_fault - membase);
    }
  }

  std::ofstream out(cvars::dc3_crash_snapshot_path,
                    std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    XELOGW("DC3 crash snapshot: failed to open {}", cvars::dc3_crash_snapshot_path);
    return;
  }

  out << "{\n";
  out << fmt::format("  \"host_pc\": {},\n", static_cast<uint64_t>(ex->pc()));
  out << fmt::format("  \"guest_pc\": {},\n", guest_pc);
  out << fmt::format("  \"guest_lr\": {},\n", static_cast<uint32_t>(context->lr));
  out << fmt::format("  \"guest_ctr\": {},\n", static_cast<uint32_t>(context->ctr));
  out << fmt::format("  \"guest_cr\": {},\n", static_cast<uint32_t>(context->cr()));
  out << fmt::format(
      "  \"guest_xer\": {{\"ca\": {}, \"ov\": {}, \"so\": {}}},\n",
      static_cast<uint32_t>(context->xer_ca), static_cast<uint32_t>(context->xer_ov),
      static_cast<uint32_t>(context->xer_so));
  out << fmt::format("  \"thread\": {{\"host_id\": {}, \"guest_id\": {}, \"handle\": {}}},\n",
                     current_thread->thread()->system_id(), current_thread->thread_id(),
                     current_thread->handle());
  out << "  \"gpr\": [";
  for (int i = 0; i < 32; ++i) {
    if (i) out << ", ";
    out << static_cast<uint64_t>(context->r[i]);
  }
  out << "],\n";
  if (has_fault) {
    out << fmt::format(
        "  \"fault\": {{\"host\": {}, \"guest\": {}, \"access\": \"{}\"}},\n",
        host_fault, guest_fault, AccessOpName(ex->access_violation_operation()));
  } else {
    out << "  \"fault\": null,\n";
  }
  out << "  \"guest_code_words\": [";
  bool first = true;
  if (guest_pc && emulator->memory() && guest_pc >= 0x80000000 && guest_pc < 0xA0000000) {
    uint32_t dump_start = (guest_pc > 0x20) ? (guest_pc - 0x20) : guest_pc;
    uint32_t dump_end = guest_pc + 0x20;
    for (uint32_t addr = dump_start; addr < dump_end; addr += 4) {
      auto* mem_ptr = emulator->memory()->TranslateVirtual<uint8_t*>(addr);
      if (!mem_ptr) break;
      uint32_t instr = xe::load_and_swap<uint32_t>(mem_ptr);
      if (!first) out << ", ";
      first = false;
      out << fmt::format("{{\"addr\": {}, \"word\": {}}}", addr, instr);
    }
  }
  out << "]\n";
  out << "}\n";
  out.flush();
  if (!out.good()) {
    XELOGW("DC3 crash snapshot: write failed {}", cvars::dc3_crash_snapshot_path);
    return;
  }
  XELOGI("DC3 crash snapshot JSON written: {}", cvars::dc3_crash_snapshot_path);
}

void Dc3NuiReturnOkExtern(cpu::ppc::PPCContext* ppc_context,
                          kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }
  ppc_context->r[3] = 0;
}

void Dc3NuiReturnNeg1Extern(cpu::ppc::PPCContext* ppc_context,
                            kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }
  ppc_context->r[3] = UINT64_C(0xFFFFFFFFFFFFFFFF);
}

void Dc3NuiReturn1Extern(cpu::ppc::PPCContext* ppc_context,
                         kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }
  ppc_context->r[3] = 1;
}

void Dc3NuiSequencerExtern(
    cpu::ppc::PPCContext* ppc_context, kernel::KernelState* kernel_state) {
  uint32_t frame_guest_addr = static_cast<uint32_t>(ppc_context->r[4]);
  Memory* memory = kernel_state->memory();
  static int s_skel_calls = 0;
  static uint32_t s_last_screen = 0;
  static int s_screen_stable_count = 0;
  static bool s_gameplay_setup_done = false;
  static uint32_t s_fake_frame_number = 0;
  static bool s_nui_entry_logged = false;
  static bool s_screen_name_scan_range_logged = false;
  static uint32_t s_scan_name_min = 0;
  static uint32_t s_scan_name_max = 0;
  static std::unordered_map<std::string, uint32_t> s_name_literal_cache;
  static bool s_loadsong_probe_logged = false;
  static bool s_loadsong_repair_attempted = false;
  static bool s_content_refresh_forced = false;
  // Auto-enable gameplay bootstrap when IK telemetry wants to reach game_screen
  static bool s_ik_bootstrap_override_applied = false;
  if (cvars::dc3_ik_telemetry && !s_ik_bootstrap_override_applied) {
    cvars::dc3_enable_gameplay_bootstrap = true;
    s_ik_bootstrap_override_applied = true;
    XELOGI("DC3: IK telemetry auto-enabled gameplay bootstrap");
  }
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
      auto* scr_obj = memory->TranslateVirtual<uint8_t*>(cur_screen_h);
      (void)scr_obj;
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

      auto try_bootstrap_gameplay = [&](const std::string& cur_name_for_log,
                                        const std::string& trans_name_for_log) {
        if (s_gameplay_setup_done || !cvars::dc3_enable_gameplay_bootstrap) {
          return;
        }
        constexpr uint32_t kTheHamDirector = 0x82F603A0;
        constexpr uint32_t kTheGamePanel = 0x83117410;
        constexpr uint32_t kGamePanelCreateGame = 0x8287ACD0;
        constexpr uint32_t kHamDirectorSetupAnims = 0x82474868;
        constexpr uint32_t kSongSequenceOnSongLoaded = 0x8288BDF8;
        constexpr uint32_t kGamePanelStartGame = 0x8287AE28;
        constexpr uint32_t kSongSequenceSingleton = 0x8311787C;

        auto* hd_ptr = memory->TranslateVirtual<uint8_t*>(kTheHamDirector);
        auto* gp_ptr = memory->TranslateVirtual<uint8_t*>(kTheGamePanel);
        uint32_t hd_addr = hd_ptr ? xe::load_and_swap<uint32_t>(hd_ptr) : 0;
        uint32_t gp_addr = gp_ptr ? xe::load_and_swap<uint32_t>(gp_ptr) : 0;
        if (!hd_addr || !gp_addr || hd_addr >= 0xF0000000 ||
            gp_addr >= 0xF0000000) {
          return;
        }

        auto* processor = kernel_state->processor();
        auto* thread_state = ppc_context->thread_state;
        kernel::XThread* execute_thread = nullptr;
        uint32_t execute_thread_id = 0;
        auto threads =
            kernel_state->object_table()->GetObjectsByType<kernel::XThread>(
                kernel::XObject::Type::Thread);
        for (auto thread : threads) {
          if (thread->main_thread() && thread->thread_state()) {
            execute_thread = thread.get();
            thread_state = thread->thread_state();
            execute_thread_id = thread->thread_id();
            break;
          }
        }
        if (!execute_thread_id && kernel::XThread::IsInThread() &&
            thread_state) {
          execute_thread = kernel::XThread::GetCurrentThread();
          execute_thread_id = kernel::XThread::GetCurrentThread()->thread_id();
        }
        if (!processor || !thread_state || !execute_thread) {
          return;
        }

        auto load_u32 = [&](uint32_t guest_addr) -> uint32_t {
          auto* ptr = is_guest_readable(guest_addr, 4)
                          ? memory->TranslateVirtual<uint8_t*>(guest_addr)
                          : nullptr;
          return ptr ? xe::load_and_swap<uint32_t>(ptr) : 0;
        };
        uint32_t game_addr =
            is_guest_readable(gp_addr + 0x38, 4) ? load_u32(gp_addr + 0x38) : 0;
        uint32_t gp_state =
            is_guest_readable(gp_addr + 0x80, 4) ? load_u32(gp_addr + 0x80) : 0;
        XELOGI(
            "DC3: Gameplay bootstrap cur='{}' trans='{}' hd={:08X} "
            "gp={:08X} game={:08X} gpState={} execThread={}",
            cur_name_for_log, trans_name_for_log, hd_addr, gp_addr, game_addr,
            gp_state, execute_thread_id);
        auto queue_member_apc = [&](uint32_t fn, uint32_t tp) {
          execute_thread->EnqueueApc(fn, tp, 0, 0);
        };
        // APC queue is LIFO, so insert the desired call chain in reverse.
        queue_member_apc(kGamePanelStartGame, gp_addr);
        queue_member_apc(kSongSequenceOnSongLoaded, kSongSequenceSingleton);
        queue_member_apc(kHamDirectorSetupAnims, hd_addr);
        if (!game_addr) {
          queue_member_apc(kGamePanelCreateGame, gp_addr);
        }
        XELOGI(
            "DC3: Gameplay bootstrap queued APC chain on thread {} "
            "(createGame={} setupAnims=1 onSongLoaded=1 startGame=1)",
            execute_thread_id, game_addr ? 0 : 1);
        s_gameplay_setup_done = true;
      };

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
          constexpr uint32_t kContentMgrRefreshSynchronously = 0x825FEBA8;
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


}  // namespace

Emulator::GameConfigLoadCallback::GameConfigLoadCallback(Emulator& emulator)
    : emulator_(emulator) {
  emulator_.AddGameConfigLoadCallback(this);
}

Emulator::GameConfigLoadCallback::~GameConfigLoadCallback() {
  emulator_.RemoveGameConfigLoadCallback(this);
}

Emulator::Emulator(const std::filesystem::path& command_line,
                   const std::filesystem::path& storage_root,
                   const std::filesystem::path& content_root,
                   const std::filesystem::path& cache_root)
    : on_launch(),
      on_terminate(),
      on_exit(),
      command_line_(command_line),
      storage_root_(storage_root),
      content_root_(content_root),
      cache_root_(cache_root),
      title_name_(),
      title_version_(),
      display_window_(nullptr),
      memory_(),
      audio_system_(),
      graphics_system_(),
      input_system_(),
      export_resolver_(),
      file_system_(),
      kernel_state_(),
      main_thread_(),
      title_id_(std::nullopt),
      paused_(false),
      restoring_(false),
      restore_fence_() {}

Emulator::~Emulator() {
  // Note that we delete things in the reverse order they were initialized.

  // Give the systems time to shutdown before we delete them.
  if (graphics_system_) {
    graphics_system_->Shutdown();
  }
  if (audio_system_) {
    audio_system_->Shutdown();
  }

  input_system_.reset();
  graphics_system_.reset();
  audio_system_.reset();

  kernel_state_.reset();
  file_system_.reset();

  processor_.reset();

  export_resolver_.reset();

  ExceptionHandler::Uninstall(Emulator::ExceptionCallbackThunk, this);
}

X_STATUS Emulator::Setup(
    ui::Window* display_window, ui::ImGuiDrawer* imgui_drawer,
    bool require_cpu_backend,
    std::function<std::unique_ptr<apu::AudioSystem>(cpu::Processor*)>
        audio_system_factory,
    std::function<std::unique_ptr<gpu::GraphicsSystem>()>
        graphics_system_factory,
    std::function<std::vector<std::unique_ptr<hid::InputDriver>>(ui::Window*)>
        input_driver_factory) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  display_window_ = display_window;
  imgui_drawer_ = imgui_drawer;

  // Initialize clock.
  // 360 uses a 50MHz clock.
  Clock::set_guest_tick_frequency(50000000);
  // We could reset this with save state data/constant value to help replays.
  Clock::set_guest_system_time_base(Clock::QueryHostSystemTime());
  // This can be adjusted dynamically, as well.
  Clock::set_guest_time_scalar(cvars::time_scalar);

  // Before we can set thread affinity we must enable the process to use all
  // logical processors.
  xe::threading::EnableAffinityConfiguration();

  // Create memory system first, as it is required for other systems.
  memory_ = std::make_unique<Memory>();
  if (!memory_->Initialize()) {
    return false;
  }

  // Shared export resolver used to attach and query for HLE exports.
  export_resolver_ = std::make_unique<xe::cpu::ExportResolver>();

  std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
  if (cvars::cpu == "x64") {
    backend.reset(new xe::cpu::backend::x64::X64Backend());
  }
#endif  // XE_ARCH
  if (cvars::cpu == "any") {
    if (!backend) {
#if XE_ARCH_AMD64
      backend.reset(new xe::cpu::backend::x64::X64Backend());
#endif  // XE_ARCH
    }
  }
  if (!backend && !require_cpu_backend) {
    backend.reset(new xe::cpu::backend::NullBackend());
  }

  // Initialize the CPU.
  processor_ = std::make_unique<xe::cpu::Processor>(memory_.get(),
                                                    export_resolver_.get());
  if (!processor_->Setup(std::move(backend))) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Initialize the APU.
  if (audio_system_factory) {
    audio_system_ = audio_system_factory(processor_.get());
    if (!audio_system_) {
      return X_STATUS_NOT_IMPLEMENTED;
    }
  }

  // Initialize the GPU.
  graphics_system_ = graphics_system_factory();
  if (!graphics_system_) {
    return X_STATUS_NOT_IMPLEMENTED;
  }

  // Initialize the HID.
  input_system_ = std::make_unique<xe::hid::InputSystem>(display_window_);
  if (!input_system_) {
    return X_STATUS_NOT_IMPLEMENTED;
  }
  if (input_driver_factory) {
    auto input_drivers = input_driver_factory(display_window_);
    for (size_t i = 0; i < input_drivers.size(); ++i) {
      auto& input_driver = input_drivers[i];
      input_driver->set_is_active_callback(
          []() -> bool { return !xe::kernel::xam::xeXamIsUIActive(); });
      input_system_->AddDriver(std::move(input_driver));
    }
  }

  result = input_system_->Setup();
  if (result) {
    return result;
  }

  // Bring up the virtual filesystem used by the kernel.
  file_system_ = std::make_unique<xe::vfs::VirtualFileSystem>();

  // Shared kernel state.
  kernel_state_ = std::make_unique<xe::kernel::KernelState>(this);

  // Setup the core components.
  result = graphics_system_->Setup(
      processor_.get(), kernel_state_.get(),
      display_window_ ? &display_window_->app_context() : nullptr,
      display_window_ != nullptr);
  if (result) {
    return result;
  }

  if (audio_system_) {
    result = audio_system_->Setup(kernel_state_.get());
    if (result) {
      return result;
    }
  }

#define LOAD_KERNEL_MODULE(t) \
  static_cast<void>(kernel_state_->LoadKernelModule<kernel::t>())
  // HLE kernel modules.
  LOAD_KERNEL_MODULE(xboxkrnl::XboxkrnlModule);
  LOAD_KERNEL_MODULE(xam::XamModule);
  LOAD_KERNEL_MODULE(xbdm::XbdmModule);
#undef LOAD_KERNEL_MODULE

  // Initialize emulator fallback exception handling last.
  ExceptionHandler::Install(Emulator::ExceptionCallbackThunk, this);

  return result;
}

X_STATUS Emulator::TerminateTitle() {
  if (!is_title_open()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (processor_) {
    processor_->ClearGuestFunctionOverrides();
  }
  Dc3RuntimeTelemetryEndSession("terminate_title");
  cpu::MiloTraceEnd("terminate_title");
  kernel_state_->TerminateTitle();
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  on_terminate();
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::LaunchPath(const std::filesystem::path& path) {
  Dc3RuntimeTelemetryEndSession("launch_path_reset");
  cpu::MiloTraceEnd("launch_path_reset");
  if (processor_) {
    processor_->ClearGuestFunctionOverrides();
  }
  // Launch based on file type.
  // This is a silly guess based on file extension.
  if (!path.has_extension()) {
    // Likely an STFS container.
    return LaunchStfsContainer(path);
  };
  auto extension = xe::utf8::lower_ascii(xe::path_to_utf8(path.extension()));
  if (extension == ".xex" || extension == ".elf" || extension == ".exe") {
    // Treat as a naked xex file.
    return LaunchXexFile(path);
  } else {
    // Assume a disc image.
    return LaunchDiscImage(path);
  }
}

X_STATUS Emulator::LaunchXexFile(const std::filesystem::path& path) {
  // We create a virtual filesystem pointing to its directory and symlink
  // that to the game filesystem.
  // e.g., /my/files/foo.xex will get a local fs at:
  // \\Device\\Harddisk0\\Partition1
  // and then get that symlinked to game:\, so
  // -> game:\foo.xex

  auto mount_path = "\\Device\\Harddisk0\\Partition1";

  // Register the local directory in the virtual filesystem.
  auto parent_path = path.parent_path();
  auto device =
      std::make_unique<vfs::HostPathDevice>(mount_path, parent_path, true);
  if (!device->Initialize()) {
    XELOGE("Unable to scan host path");
    return X_STATUS_NO_SUCH_FILE;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    XELOGE("Unable to register host path");
    return X_STATUS_NO_SUCH_FILE;
  }

  // Create symlinks to the device.
  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);
  // RB3 boot-to-menu experiment (cvar default OFF -> inert for DC3 and normal
  // runs): optionally mount update: at the disc dir so RB3 finds
  // update:\gen\patch_xbox.hdr instead of failing the open. Tests whether the
  // post-load clean shutdown on clean-TU5 _nodd is the title-update gate.
  if (cvars::rb3_mount_update) {
    file_system_->RegisterSymbolicLink("update:", mount_path);
    XELOGI("rb3_mount_update: update: -> {} (patch_xbox.* now visible)",
           mount_path);
  }
  // NOTE: intentionally do NOT symlink "update:" to the game mount. RB3 probes
  // update:\gen\patch_xbox.hdr for title-update content; if that content is
  // absent the game falls back to the base arks and boots further (retail
  // reaches the ESRB screen). Pointing update: at the disc dir exposes the
  // bundled patch_xbox.* which, in this content set, is a mismatched/placeholder
  // header ("LOLZ" magic, not the base's encrypted form) that the game rejects
  // with a "dirty disc" bail-out on BOTH retail and clean TU5. A real update:
  // mount belongs to a separate, matching title-update package, not the disc.

  // Get just the filename (foo.xex).
  auto file_name = path.filename();

  // Launch the game.
  auto fs_path = "game:\\" + xe::path_to_utf8(file_name);
  return CompleteLaunch(path, fs_path);
}

X_STATUS Emulator::LaunchDiscImage(const std::filesystem::path& path) {
  auto mount_path = "\\Device\\Cdrom0";

  // Register the disc image in the virtual filesystem.
  auto device = std::make_unique<vfs::DiscImageDevice>(mount_path, path);
  if (!device->Initialize()) {
    xe::FatalError("Unable to mount disc image; file not found or corrupt.");
    return X_STATUS_NO_SUCH_FILE;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    xe::FatalError("Unable to register disc image.");
    return X_STATUS_NO_SUCH_FILE;
  }

  // Create symlinks to the device.
  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);

  // Launch the game.
  auto module_path(FindLaunchModule());
  return CompleteLaunch(path, module_path);
}

X_STATUS Emulator::LaunchStfsContainer(const std::filesystem::path& path) {
  auto mount_path = "\\Device\\Cdrom0";

  // Register the container in the virtual filesystem.
  auto device = std::make_unique<vfs::StfsContainerDevice>(mount_path, path);
  if (!device->Initialize()) {
    xe::FatalError(
        "Unable to mount STFS container; file not found or corrupt.");
    return X_STATUS_NO_SUCH_FILE;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    xe::FatalError("Unable to register STFS container.");
    return X_STATUS_NO_SUCH_FILE;
  }

  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);

  // Launch the game.
  auto module_path(FindLaunchModule());
  return CompleteLaunch(path, module_path);
}

void Emulator::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Don't hold the lock on this (so any waits follow through)
  graphics_system_->Pause();
  audio_system_->Pause();

  auto lock = global_critical_region::AcquireDirect();
  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  auto current_thread = kernel::XThread::IsInThread()
                            ? kernel::XThread::GetCurrentThread()
                            : nullptr;
  for (auto thread : threads) {
    // Don't pause ourself or host threads.
    if (thread == current_thread || !thread->can_debugger_suspend()) {
      continue;
    }

    if (thread->is_running()) {
      thread->thread()->Suspend(nullptr);
    }
  }

  XELOGD("! EMULATOR PAUSED !");
}

void Emulator::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;
  XELOGD("! EMULATOR RESUMED !");

  graphics_system_->Resume();
  audio_system_->Resume();

  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  for (auto thread : threads) {
    if (!thread->can_debugger_suspend()) {
      // Don't pause host threads.
      continue;
    }

    if (thread->is_running()) {
      thread->thread()->Resume(nullptr);
    }
  }
}

bool Emulator::SaveToFile(const std::filesystem::path& path) {
  Pause();

  filesystem::CreateEmptyFile(path);
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite, 0, 2_GiB);
  if (!map) {
    return false;
  }

  // Save the emulator state to a file
  ByteStream stream(map->data(), map->size());
  stream.Write(kEmulatorSaveSignature);
  stream.Write(title_id_.has_value());
  if (title_id_.has_value()) {
    stream.Write(title_id_.value());
  }

  // It's important we don't hold the global lock here! XThreads need to step
  // forward (possibly through guarded regions) without worry!
  processor_->Save(&stream);
  graphics_system_->Save(&stream);
  audio_system_->Save(&stream);
  kernel_state_->Save(&stream);
  memory_->Save(&stream);
  map->Close(stream.offset());

  Resume();
  return true;
}

bool Emulator::RestoreFromFile(const std::filesystem::path& path) {
  // Restore the emulator state from a file
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite);
  if (!map) {
    return false;
  }

  restoring_ = true;

  // Terminate any loaded titles.
  Pause();
  kernel_state_->TerminateTitle();

  auto lock = global_critical_region::AcquireDirect();
  ByteStream stream(map->data(), map->size());
  if (stream.Read<uint32_t>() != kEmulatorSaveSignature) {
    return false;
  }

  auto has_title_id = stream.Read<bool>();
  std::optional<uint32_t> title_id;
  if (!has_title_id) {
    title_id = {};
  } else {
    title_id = stream.Read<uint32_t>();
  }
  if (title_id_.has_value() != title_id.has_value() ||
      title_id_.value() != title_id.value()) {
    // Swapping between titles is unsupported at the moment.
    assert_always();
    return false;
  }

  if (!processor_->Restore(&stream)) {
    XELOGE("Could not restore processor!");
    return false;
  }
  if (!graphics_system_->Restore(&stream)) {
    XELOGE("Could not restore graphics system!");
    return false;
  }
  if (!audio_system_->Restore(&stream)) {
    XELOGE("Could not restore audio system!");
    return false;
  }
  if (!kernel_state_->Restore(&stream)) {
    XELOGE("Could not restore kernel state!");
    return false;
  }
  if (!memory_->Restore(&stream)) {
    XELOGE("Could not restore memory!");
    return false;
  }

  // Update the main thread.
  auto threads =
      kernel_state_->object_table()->GetObjectsByType<kernel::XThread>();
  for (auto thread : threads) {
    if (thread->main_thread()) {
      main_thread_ = thread;
      break;
    }
  }

  Resume();

  restore_fence_.Signal();
  restoring_ = false;

  return true;
}

bool Emulator::TitleRequested() {
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");
  return xam->loader_data().launch_data_present;
}

void Emulator::LaunchNextTitle() {
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");
  auto next_title = xam->loader_data().launch_path;

  CompleteLaunch("", next_title);
}

bool Emulator::ExceptionCallbackThunk(Exception* ex, void* data) {
  return reinterpret_cast<Emulator*>(data)->ExceptionCallback(ex);
}

bool Emulator::ExceptionCallback(Exception* ex) {
  // Check to see if the exception occurred in guest code.
  auto code_cache = processor()->backend()->code_cache();
  auto code_base = code_cache->execute_base_address();
  auto code_end = code_base + code_cache->total_size();

  if (!processor()->is_debugger_attached() && debugging::IsDebuggerAttached()) {
    // If Xenia's debugger isn't attached but another one is, pass it to that
    // debugger.
    return false;
  } else if (processor()->is_debugger_attached()) {
    // Let the debugger handle this exception. It may decide to continue past it
    // (if it was a stepping breakpoint, etc).
    return processor()->OnUnhandledException(ex);
  }

  if (!(ex->pc() >= code_base && ex->pc() < code_end)) {
    // Didn't occur in guest code. Let it pass.
    return false;
  }

  // Within range. Pause the emulator and eat the exception.
  Pause();

  // Dump information into the log.
  auto current_thread = kernel::XThread::GetCurrentThread();
  assert_not_null(current_thread);

  auto guest_function = code_cache->LookupFunction(ex->pc());

  auto context = current_thread->thread_state()->context();

  XELOGE("==== CRASH DUMP ====");
  XELOGE("Thread ID (Host: 0x{:08X} / Guest: 0x{:08X})",
         current_thread->thread()->system_id(), current_thread->thread_id());
  XELOGE("Thread Handle: 0x{:08X}", current_thread->handle());
  if (guest_function) {
    XELOGE("PC: 0x{:08X}",
           guest_function->MapMachineCodeToGuestAddress(ex->pc()));
  } else {
    XELOGE("PC: <unknown guest function, host PC=0x{:016X}>", ex->pc());
  }
  XELOGE("Guest lr: 0x{:08X}", static_cast<uint32_t>(context->lr));
  XELOGE("Guest ctr: 0x{:08X}  CR=0x{:08X}  XER[CA/OV/SO]={}/{}/{}",
         static_cast<uint32_t>(context->ctr),
         static_cast<uint32_t>(context->cr()),
         static_cast<uint32_t>(context->xer_ca),
         static_cast<uint32_t>(context->xer_ov),
         static_cast<uint32_t>(context->xer_so));
  XELOGE("Registers:");
  for (int i = 0; i < 32; i++) {
    XELOGE(" r{:<3} = {:016X}", i, context->r[i]);
  }
  for (int i = 0; i < 32; i++) {
    XELOGE(" f{:<3} = {:016X} = (double){} = (float){}", i,
           *reinterpret_cast<uint64_t*>(&context->f[i]), context->f[i],
           *(float*)&context->f[i]);
  }
  for (int i = 0; i < 128; i++) {
    XELOGE(" v{:<3} = [0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}]", i,
           context->v[i].u32[0], context->v[i].u32[1], context->v[i].u32[2],
           context->v[i].u32[3]);
  }

  MaybeWriteDc3CrashSnapshotJson(this, ex, current_thread, guest_function, context);

  // Dump fault address details for access violations.
  if (ex->code() == Exception::Code::kAccessViolation) {
    uint64_t host_fault = ex->fault_address();
    uint64_t membase =
        reinterpret_cast<uintptr_t>(memory_->virtual_membase());
    uint32_t guest_fault = static_cast<uint32_t>(host_fault - membase);
    XELOGE("Fault address: host=0x{:016X} guest=0x{:08X} ({})", host_fault,
           guest_fault,
           ex->access_violation_operation() ==
                   Exception::AccessViolationOperation::kWrite
               ? "WRITE"
               : "READ");
  }

  // Dump guest function info and code around crash PC.
  if (guest_function) {
    uint32_t guest_pc = guest_function->MapMachineCodeToGuestAddress(ex->pc());
    XELOGE("Guest function: {} (0x{:08X})", guest_function->name(),
           guest_function->address());
    if (guest_pc >= 0x82000000 && guest_pc < 0x90000000) {
      uint32_t dump_start = (guest_pc > 0x40) ? (guest_pc - 0x40) : guest_pc;
      uint32_t dump_end = guest_pc + 0x40;
      XELOGE("Guest code near PC 0x{:08X}:", guest_pc);
      for (uint32_t addr = dump_start; addr < dump_end; addr += 4) {
        auto* mem_ptr = memory_->TranslateVirtual<uint8_t*>(addr);
        if (!mem_ptr) break;
        uint32_t instr = xe::load_and_swap<uint32_t>(mem_ptr);
        XELOGE("  0x{:08X}: {:08X}{}", addr, instr,
               addr == guest_pc ? "  <-- CRASH PC" : "");
      }
    }
  }

  // Walk the PPC stack to identify call chain (helps diagnose recursion).
  {
    uint32_t sp = static_cast<uint32_t>(context->r[1]);
    XELOGE("==== STACK WALK (SP=0x{:08X}) ====", sp);
    int frame = 0;
    uint32_t last_lr = 0;
    int repeat_count = 0;
    for (; frame < 20000 && sp >= 0x70000000 && sp < 0x78000000; frame++) {
      auto* host_ptr = memory_->TranslateVirtual<uint8_t*>(sp);
      if (!host_ptr) break;
      uint32_t back_chain = xe::load_and_swap<uint32_t>(host_ptr);
      // Try multiple LR save locations:
      uint32_t lr_sp4 = xe::load_and_swap<uint32_t>(host_ptr + 4);
      uint32_t lr_sp8 = xe::load_and_swap<uint32_t>(host_ptr + 8);
      // Also try __savegprlr convention: LR at back_chain - 8
      uint32_t lr_bc8 = 0;
      if (back_chain >= 0x70000000 && back_chain < 0x78000000) {
        auto* bc_ptr = memory_->TranslateVirtual<uint8_t*>(back_chain - 8);
        if (bc_ptr) lr_bc8 = xe::load_and_swap<uint32_t>(bc_ptr);
      }
      // Pick the most likely LR (first non-BEBEBEBE, non-zero, in code range)
      uint32_t best_lr = 0;
      if (lr_bc8 >= 0x82000000 && lr_bc8 < 0x8A000000) best_lr = lr_bc8;
      else if (lr_sp4 >= 0x82000000 && lr_sp4 < 0x8A000000) best_lr = lr_sp4;
      else if (lr_sp8 >= 0x82000000 && lr_sp8 < 0x8A000000) best_lr = lr_sp8;
      else best_lr = lr_sp4;  // fallback
      uint32_t frame_size = (back_chain > sp) ? (back_chain - sp) : 0;
      // Log with sampling: first 30 frames, then every 100th, last 10
      bool should_log = (frame < 30) || (frame % 100 == 0) ||
                         (best_lr != last_lr && best_lr != 0xBEBEBEBE);
      if (should_log) {
        if (repeat_count > 0) {
          XELOGE("  ... ({} identical frames skipped, lr=0x{:08X})",
                 repeat_count, last_lr);
          repeat_count = 0;
        }
        XELOGE("  [{}] sp=0x{:08X} sz={} lr_sp4=0x{:08X} lr_bc8=0x{:08X}",
               frame, sp, frame_size, lr_sp4, lr_bc8);
      } else {
        repeat_count++;
      }
      last_lr = best_lr;
      if (back_chain == 0 || back_chain == sp || back_chain < 0x70000000 ||
          back_chain >= 0x78000000)
        break;
      sp = back_chain;
    }
    if (repeat_count > 0) {
      XELOGE("  ... ({} identical frames skipped, lr=0x{:08X})",
             repeat_count, last_lr);
    }
    XELOGE("==== END STACK WALK ({} frames) ====", frame);
  }

  // Display a dialog telling the user the guest has crashed.
#ifndef XE_HEADLESS_BUILD
  if (display_window_ && imgui_drawer_) {
    display_window_->app_context().CallInUIThreadSynchronous([this]() {
      xe::ui::ImGuiDialog::ShowMessageBox(
          imgui_drawer_, "Uh-oh!",
          "The guest has crashed.\n\n"
          ""
          "Xenia has now paused itself.\n"
          "A crash dump has been written into the log.");
    });
  }
#else
  if (guest_function) {
    XELOGE("Guest crashed! PC: 0x{:08X}",
           guest_function->MapMachineCodeToGuestAddress(ex->pc()));
  } else {
    XELOGE("Guest crashed! host PC=0x{:016X} (guest function unknown)", ex->pc());
  }
#endif

  // Now suspend ourself (we should be a guest thread).
  current_thread->Suspend(nullptr);

  // We should not arrive here!
  assert_always();
  return false;
}

void Emulator::WaitUntilExit() {
  while (true) {
    if (main_thread_) {
      xe::threading::Wait(main_thread_->thread(), false);
    }

    if (restoring_) {
      restore_fence_.Wait();
    } else {
      // Not restoring and the thread exited. We're finished.
      break;
    }
  }

  on_exit();
}

void Emulator::AddGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  // Check if already added.
  if (std::find(game_config_load_callbacks_.cbegin(),
                game_config_load_callbacks_.cend(),
                callback) != game_config_load_callbacks_.cend()) {
    return;
  }
  game_config_load_callbacks_.push_back(callback);
}

void Emulator::RemoveGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  auto it = std::find(game_config_load_callbacks_.cbegin(),
                      game_config_load_callbacks_.cend(), callback);
  if (it == game_config_load_callbacks_.cend()) {
    return;
  }
  if (game_config_load_callback_loop_next_index_ != SIZE_MAX) {
    // Actualize the next callback index after the erasure from the vector.
    size_t existing_index =
        size_t(std::distance(game_config_load_callbacks_.cbegin(), it));
    if (game_config_load_callback_loop_next_index_ > existing_index) {
      --game_config_load_callback_loop_next_index_;
    }
  }
  game_config_load_callbacks_.erase(it);
}

std::string Emulator::FindLaunchModule() {
  std::string path("game:\\");

  if (!cvars::launch_module.empty()) {
    return path + cvars::launch_module;
  }

  std::string default_module("default.xex");

  auto gameinfo_entry(file_system_->ResolvePath(path + "GameInfo.bin"));
  if (gameinfo_entry) {
    vfs::File* file = nullptr;
    X_STATUS result =
        gameinfo_entry->Open(vfs::FileAccess::kGenericRead, &file);
    if (XSUCCEEDED(result)) {
      std::vector<uint8_t> buffer(gameinfo_entry->size());
      size_t bytes_read = 0;
      result = file->ReadSync(buffer.data(), buffer.size(), 0, &bytes_read);
      if (XSUCCEEDED(result)) {
        kernel::util::GameInfo info(buffer);
        if (info.is_valid()) {
          XELOGI("Found virtual title {}", info.virtual_title_id());

          const std::string xna_id("584E07D1");
          auto xna_id_entry(file_system_->ResolvePath(path + xna_id));
          if (xna_id_entry) {
            default_module = xna_id + "\\" + info.module_name();
          } else {
            XELOGE("Could not find fixed XNA path {}", xna_id);
          }
        }
      }
    }
  }

  return path + default_module;
}

static std::string format_version(xex2_version version) {
  // fmt::format doesn't like bit fields
  uint32_t major, minor, build, qfe;
  major = version.major;
  minor = version.minor;
  build = version.build;
  qfe = version.qfe;
  if (qfe) {
    return fmt::format("{}.{}.{}.{}", major, minor, build, qfe);
  }
  if (build) {
    return fmt::format("{}.{}.{}", major, minor, build);
  }
  return fmt::format("{}.{}", major, minor);
}

X_STATUS Emulator::CompleteLaunch(const std::filesystem::path& path,
                                  const std::string_view module_path) {
#ifndef XE_HEADLESS_BUILD
  // Making changes to the UI (setting the icon) and executing game config load
  // callbacks which expect to be called from the UI thread.
  assert_true(display_window_->app_context().IsInUIThread());
#else
  // Headless mode: no display window
  assert_true(display_window_ == nullptr);
#endif

  // Setup NullDevices for raw HDD partition accesses
  // Cache/STFC code baked into games tries reading/writing to these
  // By using a NullDevice that just returns success to all IO requests it
  // should allow games to believe cache/raw disk was accessed successfully

  // NOTE: this should probably be moved to xenia_main.cc, but right now we need
  // to register the \Device\Harddisk0\ NullDevice _after_ the
  // \Device\Harddisk0\Partition1 HostPathDevice, otherwise requests to
  // Partition1 will go to this. Registering during CompleteLaunch allows us to
  // make sure any HostPathDevices are ready beforehand.
  // (see comment above cache:\ device registration for more info about why)
  auto null_paths = {std::string("\\Partition0"), std::string("\\Cache0"),
                     std::string("\\Cache1")};
  auto null_device =
      std::make_unique<vfs::NullDevice>("\\Device\\Harddisk0", null_paths);
  if (null_device->Initialize()) {
    file_system_->RegisterDevice(std::move(null_device));
  }

  // Reset state.
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
#ifndef XE_HEADLESS_BUILD
  display_window_->SetIcon(nullptr, 0);
#endif

  // Allow xam to request module loads.
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");

  XELOGI("Launching module {}", module_path);
  auto module = kernel_state_->LoadUserModule(module_path);
  if (!module) {
    XELOGE("Failed to load user module {}", xe::path_to_utf8(path));
    return X_STATUS_NOT_FOUND;
  }

  // Grab the current title ID.
  xex2_opt_execution_info* info = nullptr;
  module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &info);

  if (!info) {
    title_id_ = 0;
  } else {
    title_id_ = info->title_id;
    auto title_version = info->version();
    if (title_version.value != 0) {
      title_version_ = format_version(title_version);
    }
  }

  // Try and load the resource database (xex only).
  if (module->title_id()) {
    auto title_id = fmt::format("{:08X}", module->title_id());

    // Load the per-game configuration file and make sure updates are handled by
    // the callbacks.
    config::LoadGameConfig(title_id);
    assert_true(game_config_load_callback_loop_next_index_ == SIZE_MAX);
    game_config_load_callback_loop_next_index_ = 0;
    while (game_config_load_callback_loop_next_index_ <
           game_config_load_callbacks_.size()) {
      game_config_load_callbacks_[game_config_load_callback_loop_next_index_++]
          ->PostGameConfigLoad();
    }
    game_config_load_callback_loop_next_index_ = SIZE_MAX;

    const kernel::util::XdbfGameData db = kernel_state_->module_xdbf(module);
    if (db.is_valid()) {
      XLanguage language =
          db.GetExistingLanguage(static_cast<XLanguage>(cvars::user_language));
      title_name_ = db.title(language);

      XELOGI("-------------------- ACHIEVEMENTS --------------------");
      const std::vector<kernel::util::XdbfAchievementTableEntry>
          achievement_list = db.GetAchievements();
      for (const kernel::util::XdbfAchievementTableEntry& entry :
           achievement_list) {
        std::string label = db.GetStringTableEntry(language, entry.label_id);
        std::string desc =
            db.GetStringTableEntry(language, entry.description_id);

        XELOGI("{} - {} - {} - {}", entry.id, label, desc, entry.gamerscore);
      }
      XELOGI("----------------- END OF ACHIEVEMENTS ----------------");

      auto icon_block = db.icon();
      if (icon_block) {
#ifndef XE_HEADLESS_BUILD
        display_window_->SetIcon(icon_block.buffer, icon_block.size);
#endif
      }
    }
  }

  // milo-trace (X-track) capture-session activation (X5). Driven purely by the
  // --milo_trace_* cvars so it works for ANY title (dc3 validation ground truth
  // AND rb3-xenon discovery), independent of the DC3-specific NUI-patch block
  // below. Opened here — after the module is loaded and title_id_ is known, but
  // before the title's main guest code runs — so the GuestFunction::Call
  // override-wrap hook (X2) is live for every traced call. MiloTraceBegin is a
  // no-op (leaves IsActive()==false) when out_path is empty, so the common
  // no-flag path costs nothing.
  if (cvars::milo_trace_enable && !cvars::milo_trace_out.empty()) {
    cpu::MiloTraceConfig milo_config;
    milo_config.title_id =
        title_id_.has_value() ? fmt::format("{:08X}", title_id_.value()) : "";
    milo_config.out_path = cvars::milo_trace_out;
    milo_config.manifest_path = cvars::milo_trace_manifest;
    milo_config.target_sha1 = "";  // provenance; filled in a later task
    milo_config.arch = 2;          // ppc-xenon
    milo_config.capture_method = 3;  // xenia_override (the X2 Call-hook path)
    milo_config.exact_mem = cvars::milo_trace_exact_mem;  // X6-capture-fix
    cpu::MiloTraceBegin(milo_config);
    if (cpu::MiloTraceIsActive()) {
      XELOGI("milo-trace: session active for title {} -> '{}'",
             milo_config.title_id, milo_config.out_path);
    } else {
      XELOGW("milo-trace: --milo_trace_enable set but session did not open "
             "(out='{}')", milo_config.out_path);
    }
  }

  // Initializing the shader storage in a blocking way so the user doesn't miss
  // the initial seconds - for instance, sound from an intro video may start
  // playing before the video can be seen if doing this in parallel with the
  // main thread.
  on_shader_storage_initialization(true);
  graphics_system_->InitializeShaderStorage(cache_root_, title_id_.value(),
                                            true);
  on_shader_storage_initialization(false);

  // DC3 title-specific guest code patches.
  // DC3 Title ID: 0x373307D9 (Dance Central 3)
  bool dc3_is_decomp_layout = false;
  std::optional<Dc3NuiPatchManifest> dc3_patch_manifest;
  if (title_id_.has_value() && title_id_.value() == 0x373307D9 &&
      dc3_is_decomp_layout) {
    Dc3MaybeCleanStaleContentCache(content_root_);

    // Load the patch manifest early so it's available for both NUI patching
    // and the hack pack (which runs independently of --stub_nui_functions).
    std::filesystem::path early_manifest_path;
    if (!cvars::dc3_nui_patch_manifest_path.empty()) {
      early_manifest_path = cvars::dc3_nui_patch_manifest_path;
    } else if (auto auto_path = Dc3AutoProbePatchManifestPath()) {
      early_manifest_path = *auto_path;
    }
    if (!early_manifest_path.empty()) {
      dc3_patch_manifest = Dc3LoadNuiPatchManifest(early_manifest_path);
      if (dc3_patch_manifest) {
        XELOGI("DC3: Loaded patch manifest '{}' (build_label={} targets={} "
               "crt={} hack_pack_stubs={})",
               xe::path_to_utf8(early_manifest_path),
               dc3_patch_manifest->build_label,
               dc3_patch_manifest->targets.size(),
               dc3_patch_manifest->crt_sentinels.size(),
               dc3_patch_manifest->hack_pack_stubs.size());
      } else {
        XELOGW("DC3: Failed to load patch manifest '{}'",
               xe::path_to_utf8(early_manifest_path));
      }
    }
  }

  //
  // The DC3 debug build statically links the Xbox 360 Kinect SDK (NUI).
  // NuiInitialize and related functions are PPC code embedded in the XEX,
  // NOT kernel imports. They fail because Kinect hardware doesn't exist
  // in xenia. The debug build's MILO_ASSERT halts on these failures.
  //
  // We patch the NUI functions in guest memory with PPC stubs before the
  // JIT compiles them. Each stub is 2 instructions (8 bytes):
  //   li r3, 0    (0x38600000) - return S_OK
  //   blr         (0x4E800020) - return
  //
  // This is the standard emulator approach for HLE of statically-linked
  // SDK functions (similar to Dolphin's OS HLE patches).
  if (title_id_.has_value() && title_id_.value() == 0x373307D9 &&
      cvars::stub_nui_functions) {
    XELOGI("DC3: Stubbing NUI (Kinect SDK) functions in guest memory");

    // PPC instructions (big-endian)
    const uint32_t kLiR3_0 = 0x38600000;    // li r3, 0  (return S_OK / 0)
    const uint32_t kLiR3_1 = 0x38600001;    // li r3, 1
    const uint32_t kLiR3_Neg1 = 0x3860FFFF; // li r3, -1 (return E_UNEXPECTED)
    const uint32_t kBlr = 0x4E800020;       // blr

    // NUI function addresses from the DC3 debug XEX (ham_xbox_r.exe).
    // These are guest virtual addresses for the statically-linked NUI SDK.
    // The functions are PPC code from the Xbox 360 Kinect SDK libraries.
    //
    // Addresses sourced from: dc3-decomp/config/373307D9/symbols.txt
    //
    // TODO: These addresses are specific to the DC3 debug build. A future
    // enhancement could detect the build variant (debug vs retail) by
    // checking function prologues before patching, or use a title-specific
    // patch config file.
    //
    // TODO: Consider implementing proper NUI emulation in xenia's kernel
    // layer instead of guest memory patches. This would involve:
    //   1. Adding NUI kernel module exports (NuiInitialize, NuiShutdown, etc.)
    //      similar to xam_nui.cc's XamNui* exports
    //   2. Registering them in the export resolver so import thunks work
    //   3. But core NUI functions are statically linked, NOT imported, so
    //      this approach would require either:
    //      a) Modifying x64_emitter.cc's Call() to check kExtern behavior
    //         (currently only CallExtern for sc2 checks it), or
    //      b) Adding a guest function override mechanism to Processor that
    //         intercepts calls by address (using ExternHandler + patching
    //         the indirection table entry)
    //   4. A proper implementation could provide skeleton data, depth maps,
    //      etc. for automated testing of gesture/dance gameplay
    // Functions that MUST return S_OK (game asserts on failure):
    //   - NuiInitialize: LiveCameraInput ctor line 129 MILO_ASSERT_FMT
    //   - NuiSkeletonTrackingEnable: LiveCameraInput ctor line 141
    //   - NuiImageStreamOpen: LiveCameraInput ctor lines 146, 160
    //     Returns S_OK but does NOT write output handle -> handle stays NULL
    //     -> NuiImageStreamGetNextFrame guarded by if(handle) -> never called
    //
    // Functions that can return failure (game handles gracefully):
    //   - NuiAudioCreate: wrapped in if(SUCCEEDED(...)), no assert
    //   - NuiImageStreamGetNextFrame: no assert, just skips frame processing
    //
    // Functions called from destructor (NuiShutdown always, audio only if
    // NuiAudioCreate succeeded):
    //   - NuiShutdown: called unconditionally
    //   - NuiAudioRelease: only if unk11d4 set (NuiAudioCreate success)
    //
    // The NUI SDK has ~50 functions statically linked. We stub all known
    // entry points to return S_OK (0). Functions that produce frame data
    // (GetNextFrame) return E_UNEXPECTED (-1) to signal "no data available".
    //
    // TODO: For more complete Kinect emulation, these stubs could be
    // replaced with C++ handlers that provide:
    //   - Synthetic skeleton data for automated gameplay testing
    //   - Pre-recorded depth/color streams for regression testing
    //   - Scripted gesture sequences for CI/CD integration
    //   This would require the Processor-level ExternHandler approach
    //   described above, since bl-called functions don't go through
    //   the CallExtern codepath.

    Dc3NuiPatchSpec patches[] = {
        // Core lifecycle
        {0x829D1200, kLiR3_0, kBlr, "NuiInitialize"},
        {0x829CEDA0, kLiR3_0, kBlr, "NuiShutdown"},

        // Skeleton tracking
        {0x829C25F0, kLiR3_0, kBlr, "NuiSkeletonTrackingEnable"},
        {0x829C1E18, kLiR3_0, kBlr, "NuiSkeletonTrackingDisable"},
        {0x829C1F90, kLiR3_0, kBlr, "NuiSkeletonSetTrackedSkeletons"},
        {0x829C2790, kLiR3_Neg1, kBlr, "NuiSkeletonGetNextFrame"},

        // Image streams - return S_OK but don't write output handle.
        // The handle pointer (r8) stays NULL -> GetNextFrame never called
        // because LiveCameraInput::PollTracking guards with if(curBuf.unk0).
        {0x829C9330, kLiR3_0, kBlr, "NuiImageStreamOpen"},
        {0x829C86F0, kLiR3_Neg1, kBlr, "NuiImageStreamGetNextFrame"},
        {0x829C8A18, kLiR3_0, kBlr, "NuiImageStreamReleaseFrame"},
        {0x829C91C8, kLiR3_0, kBlr, "NuiImageGetColorPixelCoordinatesFromDepthPixel"},

        // Audio - NuiAudioCreate returning failure (E_UNEXPECTED) prevents
        // the game from calling NuiAudioRegisterCallbacks (no assert).
        // LiveCameraInput sets unk11d4=0, destructor skips audio cleanup.
        //
        // TODO: NuiAudioCreate internally accesses global NUI state via
        // RtlEnterCriticalSection on a NUI mutex, allocates buffers with
        // XMemAlloc, calls XamVoiceGetMicArrayAudioEx for Kinect mic array,
        // creates 2 audio threads. Full audio emulation would require
        // implementing the NUIAUDIO subsystem with:
        //   - MEC (Microphone Echo Cancellation) stub
        //   - XamVoiceGetMicArrayAudioEx returning dummy audio streams
        //   - Audio processing thread stubs
        {0x82A0E028, kLiR3_Neg1, kBlr, "NuiAudioCreate"},
        {0x82A0DA48, kLiR3_Neg1, kBlr, "NuiAudioCreatePrivate"},
        {0x82A0D928, kLiR3_0, kBlr, "NuiAudioRegisterCallbacks"},
        {0x82A0D9A0, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacks"},
        {0x82A0C0A0, kLiR3_0, kBlr, "NuiAudioRegisterCallbacksPrivate"},
        {0x82A0C108, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacksPrivate"},
        {0x82A0D440, kLiR3_0, kBlr, "NuiAudioRelease"},

        // Camera properties - called at end of LiveCameraInput ctor
        // (SetColorCameraProperty) and in diagnostic/debug draw code.
        // Must not crash; accessing uninitialized NUI global state would
        // segfault without this stub.
        //
        // TODO: Camera property stubs could track set values in a map
        // and return them from Get calls, enabling camera config testing
        // without Kinect hardware.
        {0x829C7F48, kLiR3_0, kBlr, "NuiCameraSetProperty"},
        {0x829C7058, kLiR3_0, kBlr, "NuiCameraGetProperty"},
        {0x829C7068, kLiR3_0, kBlr, "NuiCameraGetPropertyF"},
        {0x829C7FA0, kLiR3_0, kBlr, "NuiCameraSetExposureRegionOfInterest"},
        {0x829C6868, kLiR3_0, kBlr, "NuiCameraGetExposureRegionOfInterest"},
        {0x829C3FE0, kLiR3_0, kBlr, "NuiCameraElevationSetAngle"},
        {0x829C3EF8, kLiR3_0, kBlr, "NuiCameraElevationGetAngle"},
        {0x829C4940, kLiR3_0, kBlr, "NuiCameraAdjustTilt"},
        {0x829C4E38, kLiR3_0, kBlr, "NuiCameraGetNormalToGravity"},

        // Identity (used by Skeleton.cpp for player identification)
        //
        // TODO: NuiIdentityIdentify takes a tracking ID, flags, callback,
        // and user data. A proper stub could invoke the callback with a
        // "no match" result to simulate identity processing completing.
        {0x829C36B0, kLiR3_0, kBlr, "NuiIdentityEnroll"},
        {0x829C3870, kLiR3_0, kBlr, "NuiIdentityIdentify"},
        {0x829C3998, kLiR3_0, kBlr, "NuiIdentityGetEnrollmentInformation"},
        {0x829C3BB0, kLiR3_0, kBlr, "NuiIdentityAbort"},

        // Fitness tracking (FitnessFilter.cpp)
        // Only called during fitness gameplay mode. All use MILO_NOTIFY
        // on failure (not MILO_ASSERT), so failure is safe.
        {0x829D1B68, kLiR3_Neg1, kBlr, "NuiFitnessStartTracking"},
        {0x829D1E30, kLiR3_Neg1, kBlr, "NuiFitnessPauseTracking"},
        {0x829D1F00, kLiR3_Neg1, kBlr, "NuiFitnessResumeTracking"},
        {0x829D1FD0, kLiR3_Neg1, kBlr, "NuiFitnessStopTracking"},
        {0x82E61690, kLiR3_Neg1, kBlr, "NuiFitnessGetCurrentFitnessData"},

        // Wave gesture (WaveToTurnOnLight.cpp)
        {0x829D1758, kLiR3_Neg1, kBlr, "NuiWaveSetEnabled"},
        {0x829D1668, kLiR3_Neg1, kBlr, "NuiWaveGetGestureOwnerProgress"},

        // Head tracking
        {0x829DA0C8, kLiR3_0, kBlr, "NuiHeadOrientationDisable"},
        {0x829DA598, kLiR3_0, kBlr, "NuiHeadPositionDisable"},

        // Speech recognition (SpeechMgr.cpp) - many have MILO_ASSERT_FMT.
        // SpeechMgr is only created if kinect.speech.enabled=1 in config.
        // If speech IS enabled, these must return S_OK to avoid asserts
        // in NuiSpeechCreateGrammar, NuiSpeechCommitGrammar, etc.
        //
        // TODO: Speech emulation could accept pre-scripted voice commands
        // for automated testing of menu navigation and gameplay triggers.
        // Would need to implement:
        //   - Grammar state management (rule tree in host memory)
        //   - Event queue with synthetic recognition events
        //   - NuiSpeechGetEvents returning scripted results
        {0x82A24B88, kLiR3_0, kBlr, "NuiSpeechEnable"},
        {0x82A23B70, kLiR3_0, kBlr, "NuiSpeechDisable"},
        {0x82A23BB0, kLiR3_0, kBlr, "NuiSpeechCreateGrammar"},
        {0x82A23B80, kLiR3_0, kBlr, "NuiSpeechLoadGrammar"},
        {0x82A23BA0, kLiR3_0, kBlr, "NuiSpeechUnloadGrammar"},
        {0x82A22A48, kLiR3_0, kBlr, "NuiSpeechCommitGrammar"},
        {0x82A21068, kLiR3_0, kBlr, "NuiSpeechStartRecognition"},
        {0x82A22978, kLiR3_0, kBlr, "NuiSpeechStopRecognition"},
        {0x82A21090, kLiR3_0, kBlr, "NuiSpeechSetEventInterest"},
        {0x82A21078, kLiR3_0, kBlr, "NuiSpeechSetGrammarState"},
        {0x82A22998, kLiR3_0, kBlr, "NuiSpeechSetRuleState"},
        {0x82A229B8, kLiR3_0, kBlr, "NuiSpeechCreateRule"},
        {0x82A229E0, kLiR3_0, kBlr, "NuiSpeechCreateState"},
        {0x82A22A00, kLiR3_0, kBlr, "NuiSpeechAddWordTransition"},
        {0x82A210A0, kLiR3_Neg1, kBlr, "NuiSpeechGetEvents"},
        {0x82A22988, kLiR3_0, kBlr, "NuiSpeechDestroyEvent"},
        // NOTE: 0x82A24A98 was previously stubbed as "NuiSpeech__E_init"
        // but MAP file reveals it's actually Object::sFactories static
        // initializer (Object.obj) — a critical game engine function.
        // Stubbing it broke the object factory system and caused hangs
        // in downstream initializers (gPropPaths at 0x82A24B28).
        //
        // The original JIT boundary issue (blr in EmulateRecognition stub
        // at 0x82A24AB0 getting scanned into the initializer) is avoided
        // by also not stubbing NuiSpeechEmulateRecognition — the original
        // function prologue doesn't have an early blr, so the JIT
        // boundary detection works correctly with the original code.
        //
        // NuiSpeechEmulateRecognition (0x82A24AB0) is never called
        // because all speech API entry points are already stubbed above.

        // Misc
        {0x82B57560, kLiR3_0, kBlr, "NuiMetaCpuEvent"},

        // Xbox SmartGlass (XBC) SDK - statically linked from XBC.lib
        // SmartGlassInit() calls XbcInitialize(), which calls
        // CXbcImpl::Initialize(). If Initialize returns failure, the game
        // prints "Failed to initialize Xbox SmartGlass library." and crashes.
        // The thunk functions (XbcInitialize, XbcDoWork, XbcSendJSON) are
        // only 4 bytes each (branch instructions), so we stub the real
        // CXbcImpl implementations instead.
        //
        // TODO: SmartGlass could be used for controller input automation
        // (e.g., sending menu selections from a test harness). Would need:
        //   - JSON message parsing/generation
        //   - Client connection state management
        //   - XLRC (Xbox Live Real-time Communication) stub layer
        {0x82606078, kLiR3_0, kBlr, "CXbcImpl::Initialize"},
        {0x82605960, kLiR3_0, kBlr, "CXbcImpl::DoWork"},
        {0x82605DF8, kLiR3_0, kBlr, "CXbcImpl::SendJSON"},
    };

    // Decomp XEX patch table: NUI functions at decomp MAP public symbol
    // addresses. Used when the original-address patches don't match the
    // loaded XEX layout. Addresses sourced from manifest generator
    // (generate_xenia_dc3_patch_manifest.py) which reads the MAP file.
    // When the manifest JSON is available, the resolver prefers manifest
    // addresses over these catalog entries.
    Dc3NuiPatchSpec decomp_patches[] = {
        // Core lifecycle (nuiruntime.obj)
        {0x835D8B0C, kLiR3_0, kBlr, "NuiInitialize"},
        {0x835D66D0, kLiR3_0, kBlr, "NuiShutdown"},
        {0x8373F0E0, kLiR3_0, kBlr, "NuiMetaCpuEvent"},

        // Skeleton tracking (nuiskeleton.obj)
        {0x835CA2D8, kLiR3_0, kBlr, "NuiSkeletonTrackingEnable"},
        {0x835C9B28, kLiR3_0, kBlr, "NuiSkeletonTrackingDisable"},
        {0x835C9C98, kLiR3_0, kBlr, "NuiSkeletonSetTrackedSkeletons"},
        {0x835CA478, kLiR3_Neg1, kBlr, "NuiSkeletonGetNextFrame"},

        // Image streams (nuiimagecamera.obj)
        {0x835D0F7C, kLiR3_0, kBlr, "NuiImageStreamOpen"},
        {0x835D0354, kLiR3_Neg1, kBlr, "NuiImageStreamGetNextFrame"},
        {0x835D067C, kLiR3_0, kBlr, "NuiImageStreamReleaseFrame"},
        {0x835D0E18, kLiR3_0, kBlr,
         "NuiImageGetColorPixelCoordinatesFromDepthPixel"},

        // Audio (nuiaudio.obj)
        {0x8360B778, kLiR3_Neg1, kBlr, "NuiAudioCreate"},
        {0x8360B198, kLiR3_Neg1, kBlr, "NuiAudioCreatePrivate"},
        {0x8360B07C, kLiR3_0, kBlr, "NuiAudioRegisterCallbacks"},
        {0x8360B0F4, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacks"},
        {0x83609818, kLiR3_0, kBlr, "NuiAudioRegisterCallbacksPrivate"},
        {0x83609880, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacksPrivate"},
        {0x8360AB98, kLiR3_0, kBlr, "NuiAudioRelease"},

        // Camera properties (nuidetroit.obj, nuiimagecameraproperties.obj)
        {0x835CFBC4, kLiR3_0, kBlr, "NuiCameraSetProperty"},
        {0x835CBBB4, kLiR3_0, kBlr, "NuiCameraElevationGetAngle"},
        {0x835CBC94, kLiR3_0, kBlr, "NuiCameraElevationSetAngle"},
        {0x835CC5E0, kLiR3_0, kBlr, "NuiCameraAdjustTilt"},
        {0x835CCAD0, kLiR3_0, kBlr, "NuiCameraGetNormalToGravity"},
        {0x835CFC1C, kLiR3_0, kBlr, "NuiCameraSetExposureRegionOfInterest"},
        {0x835CE4DC, kLiR3_0, kBlr, "NuiCameraGetExposureRegionOfInterest"},
        {0x835CECE0, kLiR3_0, kBlr, "NuiCameraGetProperty"},
        {0x835CECF0, kLiR3_0, kBlr, "NuiCameraGetPropertyF"},

        // Identity (identityapi.obj)
        {0x835CB384, kLiR3_0, kBlr, "NuiIdentityEnroll"},
        {0x835CB540, kLiR3_0, kBlr, "NuiIdentityIdentify"},
        {0x835CB668, kLiR3_0, kBlr, "NuiIdentityGetEnrollmentInformation"},
        {0x835CB87C, kLiR3_0, kBlr, "NuiIdentityAbort"},

        // Fitness (nuifitnessapi.obj, nuifitnessxam.obj)
        {0x835D9460, kLiR3_Neg1, kBlr, "NuiFitnessStartTracking"},
        {0x835D9728, kLiR3_Neg1, kBlr, "NuiFitnessPauseTracking"},
        {0x835D97F8, kLiR3_Neg1, kBlr, "NuiFitnessResumeTracking"},
        {0x835D98C8, kLiR3_Neg1, kBlr, "NuiFitnessStopTracking"},
        {0x83901464, kLiR3_Neg1, kBlr, "NuiFitnessGetCurrentFitnessData"},

        // Wave gesture (nuiwave.obj)
        {0x835D905C, kLiR3_Neg1, kBlr, "NuiWaveSetEnabled"},
        {0x835D8F6C, kLiR3_Neg1, kBlr, "NuiWaveGetGestureOwnerProgress"},

        // Head tracking (nuiheadposition.obj, nuiheadorientation.obj)
        {0x83293DE0, kLiR3_0, kBlr, "NuiHeadPositionDisable"},
        {0x835E18FC, kLiR3_0, kBlr, "NuiHeadOrientationDisable"},

        // Speech (xspeechapi.obj)
        {0x832C6B88, kLiR3_0, kBlr, "NuiSpeechEnable"},
        {0x832C5B90, kLiR3_0, kBlr, "NuiSpeechDisable"},
        {0x832C5BCC, kLiR3_0, kBlr, "NuiSpeechCreateGrammar"},
        {0x832C5B9C, kLiR3_0, kBlr, "NuiSpeechLoadGrammar"},
        {0x832C5BBC, kLiR3_0, kBlr, "NuiSpeechUnloadGrammar"},
        {0x832C4A7C, kLiR3_0, kBlr, "NuiSpeechCommitGrammar"},
        {0x832C30E8, kLiR3_0, kBlr, "NuiSpeechStartRecognition"},
        {0x832C49B8, kLiR3_0, kBlr, "NuiSpeechStopRecognition"},
        {0x832C3110, kLiR3_0, kBlr, "NuiSpeechSetEventInterest"},
        {0x832C30F8, kLiR3_0, kBlr, "NuiSpeechSetGrammarState"},
        {0x832C49D8, kLiR3_0, kBlr, "NuiSpeechSetRuleState"},
        {0x832C49F4, kLiR3_0, kBlr, "NuiSpeechCreateRule"},
        {0x832C4A18, kLiR3_0, kBlr, "NuiSpeechCreateState"},
        {0x832C4A34, kLiR3_0, kBlr, "NuiSpeechAddWordTransition"},
        {0x832C3120, kLiR3_Neg1, kBlr, "NuiSpeechGetEvents"},
        {0x832C49C8, kLiR3_0, kBlr, "NuiSpeechDestroyEvent"},
        {0x832C6AB4, kLiR3_0, kBlr, "NuiSpeechEmulateRecognition"},

        // SmartGlass (XBC) - (xbcimpl.obj)
        {0x8352C7A4, kLiR3_0, kBlr, "CXbcImpl::Initialize"},
        {0x8352C0AC, kLiR3_0, kBlr, "CXbcImpl::DoWork"},
        {0x8352C530, kLiR3_0, kBlr, "CXbcImpl::SendJSON"},

        // D3D NUI (nui.obj)
        {0x837948A8, kLiR3_0, kBlr, "D3DDevice_NuiInitialize"},
        {0x8378DC78, kLiR3_0, kBlr, "D3DDevice_NuiMetaData"},
        {0x83794920, kLiR3_0, kBlr, "D3DDevice_NuiStart"},
        {0x83794964, kLiR3_0, kBlr, "D3DDevice_NuiStop"},

        // Internal NUI (Nuip*) from manifest
        {0x835E2FE4, kLiR3_0, kBlr, "NuipBuildXamNuiFrameData"},
        {0x835CDF3C, kLiR3_0, kBlr, "NuipCameraGetExposureRegionOfInterest"},
        {0x835CE548, kLiR3_0, kBlr, "NuipCameraGetProperty"},
        {0x835CE850, kLiR3_0, kBlr, "NuipCameraGetPropertyF"},
        {0x8363A7B4, kLiR3_0, kBlr, "NuipCreateInstance"},
        {0x835D92EC, kLiR3_0, kBlr, "NuipFitnessInitialize"},
        {0x835D9B98, kLiR3_0, kBlr, "NuipFitnessNewSkeletalFrame"},
        {0x835D93C8, kLiR3_0, kBlr, "NuipFitnessShutdown"},
        {0x835D8120, kLiR3_0, kBlr, "NuipInitialize"},
        {0x83641870, kLiR3_0, kBlr, "NuipLoadRegistry"},
        {0x8363A844, kLiR3_0, kBlr, "NuipModuleInit"},
        {0x8363A9C0, kLiR3_0, kBlr, "NuipModuleTerm"},
        {0x83641650, kLiR3_0, kBlr, "NuipRegCreateKeyExW"},
        {0x83640328, kLiR3_0, kBlr, "NuipRegEnumKeyExW"},
        {0x83640420, kLiR3_0, kBlr, "NuipRegEnumValueW"},
        {0x836413E0, kLiR3_0, kBlr, "NuipRegOpenKeyExW"},
        {0x83640A58, kLiR3_0, kBlr, "NuipRegQueryValueExW"},
        {0x83641020, kLiR3_0, kBlr, "NuipRegSetValueExW"},
        {0x83640964, kLiR3_0, kBlr, "NuipUnloadRegistry"},
        {0x835D8DF8, kLiR3_0, kBlr, "NuipWaveInit"},
        {0x835D8E60, kLiR3_0, kBlr, "NuipWaveUpdate"},
    };

    // Log a stable .text fingerprint to support future resolver matching.
    Dc3TextSectionInfo text_info;
    if (auto* xex = module->xex_module()) {
      if (auto* text = xex->GetPESection(".text")) {
        auto* text_mem = memory_->TranslateVirtual<uint8_t*>(text->address);
        text_info.start = text->address;
        text_info.end = text->address + text->size;
        text_info.have_range = text->size != 0;
        if (text_mem && text->size) {
          uint64_t hash = UINT64_C(1469598103934665603);
          for (uint32_t i = 0; i < text->size; ++i) {
            hash ^= text_mem[i];
            hash *= UINT64_C(1099511628211);
          }
          text_info.fingerprint = hash;
          text_info.have_fingerprint = true;
          XELOGI("DC3: .text fingerprint addr={:08X} size=0x{:X} fnv1a64={:016X}",
                 text->address, text->size, hash);
        }
      }
    }

    // Two-pass approach: first check how many patch targets have zero-padding.
    // If many do, we're running a decomp/rebuilt XEX with different function
    // layout, and ALL patches use wrong addresses. In the original retail XEX,
    // no NUI function address would be zero-filled.
    int total_patches = static_cast<int>(sizeof(patches) / sizeof(patches[0]));
    int zero_count = 0;
    for (const auto& patch : patches) {
      auto* mem = memory_->TranslateVirtual<uint8_t*>(patch.address);
      if (mem && xe::load_and_swap<uint32_t>(mem) == 0x00000000) {
        zero_count++;
      }
    }

    bool is_decomp_layout = (zero_count > total_patches / 4);
    std::string_view layout_reason = "zero-padding heuristic";
    // Reuse manifest loaded earlier (before NUI block).
    auto& patch_manifest = dc3_patch_manifest;
    const bool explicit_patch_manifest_path =
        !cvars::dc3_nui_patch_manifest_path.empty();
    std::optional<Dc3FingerprintCache> fingerprint_cache;
    std::filesystem::path fingerprint_cache_path;
    if (!cvars::dc3_nui_layout_fingerprint_cache_path.empty()) {
      fingerprint_cache_path = cvars::dc3_nui_layout_fingerprint_cache_path;
    } else if (auto auto_cache_path = Dc3AutoProbeFingerprintCachePath()) {
      fingerprint_cache_path = *auto_cache_path;
    }
    if (!fingerprint_cache_path.empty()) {
      fingerprint_cache = Dc3LoadFingerprintCacheFile(fingerprint_cache_path);
      if (!fingerprint_cache) {
        XELOGW("DC3: Failed to load fingerprint cache file '{}'",
               xe::path_to_utf8(fingerprint_cache_path));
      } else {
        XELOGI("DC3: Loaded fingerprint cache '{}'",
               xe::path_to_utf8(fingerprint_cache_path));
      }
    }
    if (cvars::dc3_nui_patch_layout == "original") {
      is_decomp_layout = false;
      layout_reason = "forced by --dc3_nui_patch_layout=original";
    } else if (cvars::dc3_nui_patch_layout == "decomp") {
      is_decomp_layout = true;
      layout_reason = "forced by --dc3_nui_patch_layout=decomp";
    } else if (cvars::dc3_nui_patch_layout == "auto") {
      bool matched_manifest_layout = false;
      if (patch_manifest &&
          (patch_manifest->build_label == "decomp" ||
           patch_manifest->build_label == "original")) {
        const std::optional<uint64_t> manifest_runtime_fp =
            patch_manifest->runtime_text_fingerprint;
        const std::optional<uint64_t> manifest_compare_fp =
            manifest_runtime_fp.has_value() ? manifest_runtime_fp
                                            : patch_manifest->text_fingerprint;
        if (text_info.have_fingerprint && manifest_compare_fp.has_value()) {
          if (*manifest_compare_fp == text_info.fingerprint) {
            if (patch_manifest->build_label == "decomp") {
              is_decomp_layout = true;
              layout_reason =
                  "matched patch manifest fingerprint/build_label=decomp";
            } else {
              is_decomp_layout = false;
              layout_reason =
                  "matched patch manifest fingerprint/build_label=original";
            }
            matched_manifest_layout = true;
          } else if (explicit_patch_manifest_path) {
            XELOGW(
                "DC3: Patch manifest fingerprint {:016X} != runtime .text "
                "fingerprint {:016X}; trusting explicit manifest build_label={}",
                *manifest_compare_fp, text_info.fingerprint,
                patch_manifest->build_label);
            is_decomp_layout = patch_manifest->build_label == "decomp";
            layout_reason =
                "trusted explicit patch manifest build_label (fingerprint mismatch)";
            matched_manifest_layout = true;
          }
        } else if (explicit_patch_manifest_path) {
          XELOGW(
              "DC3: Patch manifest missing comparable fingerprint; trusting "
              "explicit manifest build_label={}",
              patch_manifest->build_label);
          is_decomp_layout = patch_manifest->build_label == "decomp";
          layout_reason =
              "trusted explicit patch manifest build_label (no fingerprint)";
          matched_manifest_layout = true;
        }
      }
      uint64_t fp_original = 0;
      uint64_t fp_decomp = 0;
      bool have_fp_original = Dc3TryParseHexU64(
          cvars::dc3_nui_layout_fingerprint_original, &fp_original);
      bool have_fp_decomp = Dc3TryParseHexU64(
          cvars::dc3_nui_layout_fingerprint_decomp, &fp_decomp);
      if (!have_fp_original && fingerprint_cache &&
          fingerprint_cache->original.has_value()) {
        fp_original = *fingerprint_cache->original;
        have_fp_original = true;
      }
      if (!have_fp_decomp && fingerprint_cache &&
          fingerprint_cache->decomp.has_value()) {
        fp_decomp = *fingerprint_cache->decomp;
        have_fp_decomp = true;
      }
      if (!matched_manifest_layout && text_info.have_fingerprint && have_fp_original &&
          text_info.fingerprint == fp_original) {
        is_decomp_layout = false;
        layout_reason = "matched --dc3_nui_layout_fingerprint_original";
      } else if (!matched_manifest_layout && text_info.have_fingerprint &&
                 have_fp_decomp &&
                 text_info.fingerprint == fp_decomp) {
        is_decomp_layout = true;
        layout_reason = "matched --dc3_nui_layout_fingerprint_decomp";
      } else if (!matched_manifest_layout && text_info.have_fingerprint &&
                 (have_fp_original || have_fp_decomp)) {
        const std::string fp_original_str = have_fp_original
                                                ? fmt::format("{:016X}", fp_original)
                                                : std::string("<unset>");
        const std::string fp_decomp_str = have_fp_decomp
                                              ? fmt::format("{:016X}", fp_decomp)
                                              : std::string("<unset>");
        XELOGI(
            "DC3: .text fingerprint {:016X} did not match configured layout "
            "fingerprints (original={} decomp={})",
            text_info.fingerprint, fp_original_str, fp_decomp_str);
      }
    } else {
      XELOGW(
          "DC3: Invalid --dc3_nui_patch_layout='{}' (expected auto|original|decomp); "
          "falling back to auto heuristic",
          cvars::dc3_nui_patch_layout);
    }
    XELOGI(
        "DC3: NUI patch layout={} reason={} (zero-padding {}/{})",
        is_decomp_layout ? "decomp" : "original", layout_reason, zero_count,
        total_patches);
    dc3_is_decomp_layout = is_decomp_layout;

    // Select the appropriate patch table based on XEX layout.
    const Dc3NuiPatchSpec* active_patches =
        is_decomp_layout ? decomp_patches : patches;
    int active_count = is_decomp_layout
                           ? static_cast<int>(sizeof(decomp_patches) /
                                              sizeof(decomp_patches[0]))
                           : total_patches;

    std::optional<Dc3NuiSymbolManifest> symbol_manifest;
    std::filesystem::path symbol_manifest_path;
    if (!cvars::dc3_nui_symbol_map_path.empty()) {
      symbol_manifest_path = cvars::dc3_nui_symbol_map_path;
    } else if (auto auto_path = Dc3AutoProbeNuiSymbolMapPath()) {
      symbol_manifest_path = *auto_path;
    }
    if (!symbol_manifest_path.empty()) {
      symbol_manifest = Dc3LoadNuiSymbolManifest(symbol_manifest_path);
      if (symbol_manifest) {
        XELOGI("DC3: Loaded NUI symbol manifest '{}' ({} .text symbols)",
               xe::path_to_utf8(symbol_manifest_path),
               symbol_manifest->text_symbols.size());
      } else {
        XELOGW("DC3: Failed to load NUI symbol manifest '{}'",
               xe::path_to_utf8(symbol_manifest_path));
      }
    }

    bool use_patch_manifest_targets = patch_manifest.has_value();
    if (patch_manifest && text_info.have_fingerprint) {
      const std::optional<uint64_t> manifest_runtime_fp =
          patch_manifest->runtime_text_fingerprint;
      const std::optional<uint64_t> manifest_compare_fp =
          manifest_runtime_fp.has_value() ? manifest_runtime_fp
                                          : patch_manifest->text_fingerprint;
      if (manifest_compare_fp.has_value() &&
          *manifest_compare_fp != text_info.fingerprint) {
        XELOGW(
            "DC3: Disabling patch manifest target resolution due fingerprint "
            "mismatch (manifest {:016X} != runtime {:016X}); "
            "falling back to symbol/signature/catalog",
            *manifest_compare_fp, text_info.fingerprint);
        use_patch_manifest_targets = false;
      }
    }

    std::string resolver_mode = cvars::dc3_nui_patch_resolver_mode;
    if (resolver_mode == "legacy") {
      XELOGW("DC3: --dc3_nui_patch_resolver_mode=legacy has been removed; "
             "using hybrid");
      resolver_mode = "hybrid";
    } else if (resolver_mode != "hybrid" && resolver_mode != "strict") {
      XELOGW("DC3: Unknown --dc3_nui_patch_resolver_mode='{}'; using hybrid",
             resolver_mode);
      resolver_mode = "hybrid";
    }
    Dc3RuntimeTelemetryConfig telemetry_config;
    telemetry_config.title_id = "373307D9";
    telemetry_config.build_kind = is_decomp_layout ? "decomp" : "original";
    telemetry_config.resolver_mode = resolver_mode;
    telemetry_config.signature_resolver = cvars::dc3_nui_enable_signature_resolver;
    telemetry_config.guest_overrides = true;
    Dc3RuntimeTelemetryBeginSession(telemetry_config);
    Dc3RuntimeTelemetryRecordBootMilestone("dc3_nui_patch_block_begin");

    std::vector<Dc3ResolvedNuiPatch> resolved_patches;
    resolved_patches.reserve(active_count);
    int resolved_by_manifest = 0;
    int resolved_by_symbol = 0;
    int resolved_by_signature = 0;
    int resolved_by_catalog = 0;
    int resolver_strict_rejects = 0;
    for (int i = 0; i < active_count; ++i) {
      auto resolved = Dc3ResolveNuiPatchTarget(active_patches[i], text_info,
                                               use_patch_manifest_targets
                                                   ? &*patch_manifest
                                                   : nullptr,
                                               symbol_manifest ? &*symbol_manifest
                                                               : nullptr,
                                               resolver_mode,
                                               memory_.get(),
                                               cvars::dc3_nui_enable_signature_resolver);
      if (!resolved.resolved && resolved.strict_rejected) {
        resolver_strict_rejects++;
      } else if (resolved.resolved) {
        if (resolved.resolve_method == Dc3PatchResolveMethod::kPatchManifest) {
          resolved_by_manifest++;
        } else if (resolved.resolve_method == Dc3PatchResolveMethod::kSymbolMap) {
          resolved_by_symbol++;
        } else if (resolved.resolve_method ==
                   Dc3PatchResolveMethod::kSignatureStub) {
          resolved_by_signature++;
        } else if (resolved.resolve_method ==
                   Dc3PatchResolveMethod::kCatalogAddress) {
          resolved_by_catalog++;
        }
      }
      resolved_patches.push_back(resolved);
    }
    XELOGI(
        "DC3: NUI resolver summary mode={} manifest_hits={} symbol_hits={} "
        "signature_hits={} catalog_hits={} strict_rejects={} total={}",
        resolver_mode, resolved_by_manifest,
        resolved_by_symbol, resolved_by_signature,
        resolved_by_catalog, resolver_strict_rejects, active_count);
    Dc3RuntimeTelemetryRecordNuiResolverSummary(
        resolver_mode, resolved_by_manifest, resolved_by_symbol,
        resolved_by_signature, resolved_by_catalog, resolver_strict_rejects,
        active_count);
    if (cvars::dc3_nui_signature_trace) {
      auto should_trace_signature_target = [](std::string_view name) {
        return !name.empty();
      };
      auto log_words = [&](const char* label, uint32_t address) {
        if (!Dc3PatchTargetInText(text_info, address, 4)) {
          XELOGI("DC3: SignatureTrace {} {:08X} (outside .text)", label, address);
          return;
        }
        auto* mem = memory_->TranslateVirtual<uint8_t*>(address);
        if (!mem) {
          XELOGI("DC3: SignatureTrace {} {:08X} (unmapped)", label, address);
          return;
        }
        std::string words;
        for (int j = 0; j < 12; ++j) {
          if (!Dc3PatchTargetInText(text_info, address + j * 4, 4)) {
            break;
          }
          const uint32_t w = xe::load_and_swap<uint32_t>(mem + j * 4);
          if (!words.empty()) {
            words.push_back(' ');
          }
          words += fmt::format("{:08X}", w);
        }
        XELOGI("DC3: SignatureTrace {} {:08X}: {}", label, address, words);
      };
      for (const auto& resolved_patch : resolved_patches) {
        const auto& patch = resolved_patch.spec;
        if (!should_trace_signature_target(patch.name)) {
          continue;
        }
        XELOGI("DC3: SignatureTrace target={} resolver={} resolved={} "
               "catalog={:08X} resolved_addr={:08X}",
               patch.name, Dc3PatchResolveMethodName(resolved_patch.resolve_method),
               resolved_patch.resolved ? 1 : 0, patch.address,
               resolved_patch.resolved ? resolved_patch.resolved_address : 0);
        log_words("catalog", patch.address);
        if (resolved_patch.resolved && resolved_patch.resolved_address != patch.address) {
          log_words("resolved", resolved_patch.resolved_address);
        }
      }
    }

    const bool requested_guest_overrides = cvars::dc3_guest_overrides;
    const bool enable_guest_overrides = true;
    if (!requested_guest_overrides) {
      XELOGW(
          "DC3: DC3 NUI/XBC legacy byte-patch path has been removed; "
          "forcing guest overrides on (rollback by reverting commit)");
    }
    XELOGI("DC3: NUI/XBC apply path guest_overrides={} resolver_mode={} "
           "signature_resolver={}",
           enable_guest_overrides ? 1 : 0, resolver_mode,
           cvars::dc3_nui_enable_signature_resolver ? 1 : 0);

    processor_->ClearGuestFunctionOverrides();
    auto guest_extern_handler_for_patch =
        [&](const Dc3NuiPatchSpec& patch) -> cpu::GuestFunction::ExternHandler {
      if (!enable_guest_overrides) {
        return nullptr;
      }
      // Preserve the original-layout fake skeleton path when enabled.
      if (cvars::fake_kinect_data && !is_decomp_layout &&
          std::string_view(patch.name) == "NuiSkeletonGetNextFrame") {
        return Dc3NuiSequencerExtern;
      }
      if (patch.insn1 != kBlr) {
        return nullptr;
      }
      if (patch.insn0 == kLiR3_0) {
        return Dc3NuiReturnOkExtern;
      }
      if (patch.insn0 == kLiR3_Neg1) {
        return Dc3NuiReturnNeg1Extern;
      }
      if (patch.insn0 == kLiR3_1) {
        return Dc3NuiReturn1Extern;
      }
      return nullptr;
    };
    auto patch_target_in_text = [&](uint32_t address) -> bool {
      return Dc3PatchTargetInText(text_info, address);
    };
    int override_registered = 0;
    int override_unsupported = 0;
    int override_register_failed = 0;
    int override_register_non_text = 0;
    int override_register_unresolved = 0;
    for (int i = 0; i < active_count; i++) {
      const auto& resolved_patch = resolved_patches[i];
      const auto& patch = resolved_patch.spec;
      if (!resolved_patch.resolved) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(unresolved target; resolver mode={})",
            patch.address, patch.name, resolver_mode);
        override_register_unresolved++;
        override_register_failed++;
        continue;
      }
      const uint32_t patch_addr = resolved_patch.resolved_address;
            if (std::string_view(patch.name) == "NuiSkeletonGetNextFrame") {
        auto* h = guest_extern_handler_for_patch(patch);
        if (h) {
          processor_->RegisterGuestFunctionOverride(patch_addr, h, patch.name);
          XELOGI("DC3: ULTRA FORCED registration of NUI sequencer at {:08X}", patch_addr);
          override_registered++;
          continue;
        }
      }
      auto handler = guest_extern_handler_for_patch(patch);
      if (!handler) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(unsupported patch shape for override; legacy byte patch path "
            "removed)",
            patch_addr, patch.name);
        override_unsupported++;
        override_register_failed++;
        continue;
      }
      if (!patch_target_in_text(patch_addr)) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(outside .text range {:08X}-{:08X})",
            patch_addr, patch.name, text_info.start, text_info.end);
        override_register_non_text++;
        override_register_failed++;
        continue;
      }
      auto* heap = memory_->LookupHeap(patch_addr);
      auto* mem = memory_->TranslateVirtual<uint8_t*>(patch_addr);
      if (!heap || !mem) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(invalid guest address)",
            patch_addr, patch.name);
        override_register_failed++;
        continue;
      }
      uint32_t existing0 = xe::load_and_swap<uint32_t>(mem + 0);
      if (existing0 == 0x00000000) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(zero-filled target)",
            patch_addr, patch.name);
        override_register_failed++;
        continue;
      }
      processor_->RegisterGuestFunctionOverride(patch_addr, handler,
                                                std::string(patch.name));
      XELOGI("DC3: Registered guest extern override {:08X}: {} (resolver={})",
             patch_addr, patch.name,
             Dc3PatchResolveMethodName(resolved_patch.resolve_method));
      Dc3RuntimeTelemetryRecordNuiOverrideRegistered(
          patch.name, patch_addr,
          Dc3PatchResolveMethodName(resolved_patch.resolve_method));
      override_registered++;
    }
    XELOGI(
        "DC3: Registered {} guest extern overrides from NUI patch table "
        "({} entries not overridden, {} registration failures, "
        "{} outside .text, {} unresolved)",
        override_registered, active_count - override_registered,
        override_register_failed, override_register_non_text,
        override_register_unresolved);

    const int patched = 0;
    const int overridden = override_registered;
    const int skipped = active_count - overridden;
    XELOGI(
        "DC3: NUI patch/override summary: patched={} overridden={} skipped={} "
        "total={} layout={} unsupported_override_entries={} "
        "override_registration_failures={} "
        "override_registration_non_text={} skipped_unresolved={} "
        "legacy_byte_patching_removed=1",
        patched, overridden, skipped, active_count,
        is_decomp_layout ? "decomp" : "original", override_unsupported,
        override_register_failed, override_register_non_text,
        override_register_unresolved);
    Dc3RuntimeTelemetryRecordNuiPatchSummary(
        patched, overridden, skipped, active_count,
        is_decomp_layout ? "decomp" : "original");
    Dc3RuntimeTelemetryRecordBootMilestone("dc3_nui_patch_apply_complete");

    // Fake Kinect skeleton data injection / SkeletonUpdate patches
    // are extracted into dc3_hack_pack for the same reason as the non-NUI
    // workarounds: keep emulator.cc orchestration-focused and preserve an
    // explicit retirement path.
    {
      Dc3HackContext dc3_skeleton_ctx;
      dc3_skeleton_ctx.memory = memory_.get();
      dc3_skeleton_ctx.processor = processor_.get();
      dc3_skeleton_ctx.module = module.get();
      dc3_skeleton_ctx.is_decomp_layout = is_decomp_layout;
#ifdef XE_HEADLESS_BUILD
      dc3_skeleton_ctx.is_headless = true;
#else
      dc3_skeleton_ctx.is_headless = false;
#endif
      auto skel_result = ApplyDc3SkeletonHackPack(dc3_skeleton_ctx);
      XELOGD("DC3: hack-pack category={} applied={} skipped={} failed={}",
             Dc3HackCategoryName(skel_result.category), skel_result.applied,
             skel_result.skipped, skel_result.failed);
    }
  }

  //
  // Fix CRT XapiCallThreadNotifyRoutines hang for the DC3 decomp XEX.
  //
  // The decomp's CRT has an uninitialized LIST_ENTRY at 0x83B14C3C
  // (XapiThreadNotifyRoutineList). On a real Xbox 360, this would be
  // statically initialized to point to itself (empty circular list). In
  // the decomp build, it contains garbage, causing
  // XapiCallThreadNotifyRoutines (0x82F51108) to iterate a corrupt list
  // and spin forever trying to call null callback pointers.
  //
  // Non-NUI DC3 workarounds (CRT/imports/debug/decomp runtime stopgaps) are
  // extracted into the DC3 hack pack module to keep emulator.cc orchestration-
  // only and make retirement tracking manageable.
  if (title_id_.has_value() && title_id_.value() == 0x373307D9 &&
      dc3_is_decomp_layout) {
    Dc3HackContext dc3_hack_ctx;
    dc3_hack_ctx.memory = memory_.get();
    dc3_hack_ctx.processor = processor_.get();
    dc3_hack_ctx.module = module.get();
    dc3_hack_ctx.is_decomp_layout = dc3_is_decomp_layout;
    if (dc3_patch_manifest.has_value()) {
      if (!dc3_patch_manifest->hack_pack_stubs.empty()) {
        dc3_hack_ctx.hack_pack_stubs = &dc3_patch_manifest->hack_pack_stubs;
        XELOGI("DC3: Passing {} hack-pack stub addresses from manifest",
               dc3_patch_manifest->hack_pack_stubs.size());
      }
      if (!dc3_patch_manifest->crt_sentinels.empty()) {
        dc3_hack_ctx.crt_sentinels = &dc3_patch_manifest->crt_sentinels;
        XELOGI("DC3: Passing {} CRT sentinel addresses from manifest",
               dc3_patch_manifest->crt_sentinels.size());
      }
      if (!dc3_patch_manifest->xdk_overrides.empty()) {
        dc3_hack_ctx.xdk_overrides = &dc3_patch_manifest->xdk_overrides;
        XELOGI("DC3: Passing {} XDK override addresses from manifest",
               dc3_patch_manifest->xdk_overrides.size());
      }
      if (!dc3_patch_manifest->xdk_code_ranges.empty()) {
        // Reinterpret-cast is safe: both CodeRange structs have identical
        // layout {uint32_t start; uint32_t end;}.
        dc3_hack_ctx.xdk_code_ranges = reinterpret_cast<
            const std::vector<Dc3HackContext::CodeRange>*>(
            &dc3_patch_manifest->xdk_code_ranges);
        XELOGI("DC3: Passing {} XDK code ranges for prologue scanning",
               dc3_patch_manifest->xdk_code_ranges.size());
      }
    }
    // Populate kAddr from manifest address catalog (before hack pack applies).
    if (dc3_patch_manifest.has_value() &&
        !dc3_patch_manifest->address_catalog.empty()) {
      Dc3PopulateAddressesFromCatalog(dc3_patch_manifest->address_catalog,
                                      dc3_patch_manifest->crt_sentinels);
    }
#ifdef XE_HEADLESS_BUILD
    dc3_hack_ctx.is_headless = true;
#else
    dc3_hack_ctx.is_headless = false;
#endif
    auto dc3_hack_summary = ApplyDc3HackPack(dc3_hack_ctx);
    for (const auto& result : dc3_hack_summary.results) {
      XELOGD("DC3: hack-pack category={} applied={} skipped={} failed={}",
             Dc3HackCategoryName(result.category), result.applied,
             result.skipped, result.failed);
    }
    if (cvars::dc3_ik_telemetry) {
      auto ik_result = ApplyDc3IKTelemetry(dc3_hack_ctx);
      XELOGI("DC3: IK telemetry: applied={} skipped={} failed={}",
             ik_result.applied, ik_result.skipped, ik_result.failed);
    }
    Dc3RuntimeTelemetryRecordBootMilestone("dc3_hack_pack_apply_complete");
  } else if (title_id_.has_value() && title_id_.value() == 0x373307D9) {
    XELOGI(
        "DC3: Skipping hack pack for original XEX (NUI overrides already applied)");
    XELOGI("DC3: Original-XEX boot patch staging begins "
           "(fake_kinect_data={} decomp_layout={})",
           cvars::fake_kinect_data, dc3_is_decomp_layout);
    auto with_patch_target =
        [&](const char* label, uint32_t addr, size_t size, auto&& apply) {
          auto* ptr = memory_->TranslateVirtual<uint8_t*>(addr);
          if (!ptr) {
            XELOGW("DC3: Patch lookup failed: {} at {:08X} "
                   "(TranslateVirtual returned null)",
                   label, addr);
            return false;
          }
          auto* heap = memory_->LookupHeap(addr);
          if (!heap) {
            XELOGW("DC3: Patch lookup failed: {} at {:08X} "
                   "(LookupHeap returned null)",
                   label, addr);
            return false;
          }
          heap->Protect(addr, size, kMemoryProtectRead | kMemoryProtectWrite);
          apply(ptr);
          return true;
        };

    constexpr uint32_t kSaveLoadManagerActivate = 0x82894A10;
    with_patch_target("SaveLoadManager::Activate", kSaveLoadManagerActivate, 4,
                      [&](uint8_t* sla_ptr) {
                        xe::store_and_swap<uint32_t>(sla_ptr, 0x4E800020);
                        XELOGI(
                            "DC3: Stubbed SaveLoadManager::Activate at {:08X} to blr",
                            kSaveLoadManagerActivate);
                      });

    constexpr uint32_t kHamPanelFocusComponent = 0x828EFE90;
    constexpr uint32_t kUIPanelFocusComponent = 0x827A6310;
    with_patch_target("HamPanel::FocusComponent", kHamPanelFocusComponent, 4,
                      [&](uint8_t* ptr) {
                        constexpr uint32_t kBranchMask = 0x03FFFFFC;
                        uint32_t branch =
                            0x48000000 |
                            ((kUIPanelFocusComponent - kHamPanelFocusComponent) &
                             kBranchMask);
                        xe::store_and_swap<uint32_t>(ptr, branch);
                        XELOGI("DC3: UI fix: redirected HamPanel::FocusComponent "
                               "at {:08X} to UIPanel::FocusComponent {:08X}",
                               kHamPanelFocusComponent, kUIPanelFocusComponent);
                      });

    constexpr uint32_t kHamScreenIsEventDialogOnTop = 0x829626D8;
    with_patch_target("HamScreen::IsEventDialogOnTop",
                      kHamScreenIsEventDialogOnTop, 8, [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr + 0, 0x38600000);
                        xe::store_and_swap<uint32_t>(ptr + 4, 0x4E800020);
                        XELOGI("DC3: UI fix: stubbed HamScreen::IsEventDialogOnTop "
                               "at {:08X} to return false",
                               kHamScreenIsEventDialogOnTop);
                      });

    constexpr uint32_t kCDReadDone = 0x826026E0;
    with_patch_target("CDReadDone", kCDReadDone, 8, [&](uint8_t* cdr_ptr) {
      xe::store_and_swap<uint32_t>(cdr_ptr + 0, 0x38600001);
      xe::store_and_swap<uint32_t>(cdr_ptr + 4, 0x4E800020);
      XELOGI("DC3: Stubbed CDReadDone at {:08X} to return true",
             kCDReadDone);
    });

    constexpr uint32_t kContentMgrRefreshDone = 0x825FEB48;
    with_patch_target("ContentMgr::RefreshDone", kContentMgrRefreshDone, 8,
                      [&](uint8_t* crd_ptr) {
                        xe::store_and_swap<uint32_t>(crd_ptr + 0, 0x38600001);
                        xe::store_and_swap<uint32_t>(crd_ptr + 4, 0x4E800020);
                        XELOGI("DC3: Stubbed ContentMgr::RefreshDone at {:08X} "
                               "to return true",
                               kContentMgrRefreshDone);
                      });

    constexpr uint32_t kSplashPrepareNext = 0x82554388;
    with_patch_target("Splash::PrepareNext", kSplashPrepareNext, 8,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr + 0, 0x38600000);
                        xe::store_and_swap<uint32_t>(ptr + 4, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::PrepareNext "
                               "at {:08X} to return false",
                               kSplashPrepareNext);
                      });

    constexpr uint32_t kSplashBeginSplasher = 0x825554C8;
    with_patch_target("Splash::BeginSplasher", kSplashBeginSplasher, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::BeginSplasher "
                               "at {:08X} to blr",
                               kSplashBeginSplasher);
                      });

    constexpr uint32_t kSplashSuspend = 0x82553BE0;
    with_patch_target("Splash::Suspend", kSplashSuspend, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::Suspend "
                               "at {:08X} to blr",
                               kSplashSuspend);
                      });

    constexpr uint32_t kSplashResume = 0x82553D68;
    with_patch_target("Splash::Resume", kSplashResume, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::Resume "
                               "at {:08X} to blr",
                               kSplashResume);
                      });

    constexpr uint32_t kSpeechGrammarUnload = 0x82439F38;
    with_patch_target("SpeechMgr::Grammar::Unload", kSpeechGrammarUnload, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Speech fix: stubbed "
                               "SpeechMgr::Grammar::Unload at {:08X} to blr",
                               kSpeechGrammarUnload);
                      });

    constexpr uint32_t kMovieInit = 0x82555678;
    // Intentionally NOT stubbed. Movie::Init() is just `{ TheMovieSys.Init(); }`,
    // the boot's entry into movie-system init. The old `blr` stub here was the
    // root of the regression: it skipped TheMovieSys.Init() entirely, so
    // BinkMovieSys::Init never ran, isInitalized stayed false, and the guest's
    // MILO_ASSERT(TheMovieSys.IsInitialized()) in Movie::BeginFromFile (line 220)
    // fired fatally on the attract movie. Letting Movie::Init run now dispatches
    // to BinkMovieSys::Init (patched below to set isInitalized=1 and return
    // before the hanging BinkStartAsyncThread), which is the whole point.
    XELOGI("DC3: Movie bypass: leaving Movie::Init at {:08X} intact so it "
           "calls TheMovieSys.Init() (BinkMovieSys::Init patched to set flag)",
           kMovieInit);

    constexpr uint32_t kBinkMovieSysInit = 0x82E214A8;
    // Set TheMovieSys.isInitalized = true instead of a bare blr. The old blr
    // stub skipped MovieSys::Init() entirely, so isInitalized stayed false and
    // the guest's MILO_ASSERT(TheMovieSys.IsInitialized()) (Movie.cpp:220)
    // fired fatally at boot, freezing the whole game on the debug error screen.
    // MovieSys layout: vptr @0, isInitalized(bool) @4; r3 == this (BinkMovieSys
    // base coincides with the MovieSys base). We still skip BinkStartAsyncThread
    // (the part that hangs headless and the reason Init was stubbed at all).
    with_patch_target("BinkMovieSys::Init", kBinkMovieSysInit, 12,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr + 0, 0x38000001);  // li  r0, 1
                        xe::store_and_swap<uint32_t>(ptr + 4, 0x98030004);  // stb r0, 4(r3)
                        xe::store_and_swap<uint32_t>(ptr + 8, 0x4E800020);  // blr
                        XELOGI("DC3: Movie bypass: BinkMovieSys::Init at {:08X} "
                               "now sets isInitalized=1 (was blr)",
                               kBinkMovieSysInit);
                      });

    if (cvars::fake_kinect_data) {
      XELOGI("DC3: Entering original-XEX fake Kinect patch block");

      constexpr uint32_t kSetPlayerPresentGuard = 0x8290834C;
      constexpr uint32_t kExpectedInsn = 0x4800001D;
      auto* guard_ptr =
          memory_->TranslateVirtual<uint8_t*>(kSetPlayerPresentGuard);
      if (guard_ptr) {
        uint32_t actual = xe::load_and_swap<uint32_t>(guard_ptr);
        if (actual == kExpectedInsn) {
          auto* heap = memory_->LookupHeap(kSetPlayerPresentGuard);
          if (heap) {
            heap->Protect(kSetPlayerPresentGuard, 4,
                          kMemoryProtectRead | kMemoryProtectWrite);
            xe::store_and_swap<uint32_t>(guard_ptr, 0x60000000);
            XELOGI("DC3: Calibration bypass: NOP'd IsTrackingAllSkeletons "
                   "guard in SetPlayerPresent at {:08X}",
                   kSetPlayerPresentGuard);
          }
        } else {
          XELOGW("DC3: Calibration bypass: unexpected insn at {:08X}: "
                 "{:08X} (expected {:08X})",
                 kSetPlayerPresentGuard, actual, kExpectedInsn);
        }
      }

      constexpr uint32_t kChoosePlayerSides = 0x82909968;
      with_patch_target("ChoosePlayerSides", kChoosePlayerSides, 4,
                        [&](uint8_t* cps_ptr) {
                          xe::store_and_swap<uint32_t>(cps_ptr, 0x4E800020);
                          XELOGI("DC3: Calibration bypass: stubbed "
                                 "ChoosePlayerSides at {:08X} to blr",
                                 kChoosePlayerSides);
                        });

      constexpr uint32_t kSetPlayerSkeletonWarningData = 0x82907880;
      with_patch_target("SetPlayerSkeletonWarningData",
                        kSetPlayerSkeletonWarningData, 4,
                        [&](uint8_t* spw_ptr) {
                          xe::store_and_swap<uint32_t>(spw_ptr, 0x4E800020);
                          XELOGI("DC3: Calibration bypass: stubbed "
                                 "SetPlayerSkeletonWarningData at {:08X} to blr",
                                 kSetPlayerSkeletonWarningData);
                        });

      constexpr uint32_t kSetPlayerSkeletonNavData = 0x82909340;
      constexpr uint32_t kSetPlayerPresent = 0x82908320;
      auto* nav_ptr =
          memory_->TranslateVirtual<uint8_t*>(kSetPlayerSkeletonNavData);
      if (nav_ptr) {
        auto* heap = memory_->LookupHeap(kSetPlayerSkeletonNavData);
        if (heap) {
          heap->Protect(kSetPlayerSkeletonNavData, 64,
                        kMemoryProtectRead | kMemoryProtectWrite);
          auto w = [nav_ptr](int idx, uint32_t insn) {
            xe::store_and_swap<uint32_t>(nav_ptr + idx * 4, insn);
          };
          int i = 0;
          w(i++, 0x7C0802A6);
          w(i++, 0x90010004);
          w(i++, 0x9421FFC0);
          w(i++, 0x38600000);
          w(i++, 0x38800001);
          w(i++, 0x48000001 | (kSetPlayerPresent - 0x82909350));
          w(i++, 0x38600001);
          w(i++, 0x38800001);
          w(i++, 0x48000001 | (kSetPlayerPresent - 0x82909358));
          w(i++, 0x38210040);
          w(i++, 0x80010004);
          w(i++, 0x7C0803A6);
          w(i++, 0x4E800020);
          XELOGI("DC3: Calibration bypass: replaced SetPlayerSkeletonNavData "
                 "at {:08X} with SetPlayerPresent stub ({} instructions)",
                 kSetPlayerSkeletonNavData, i);
        }
      }

      constexpr uint32_t kShouldWaitForRecovery = 0x82904CD0;
      with_patch_target("ShouldWaitForRecovery", kShouldWaitForRecovery, 8,
                        [&](uint8_t* swr_ptr) {
                          xe::store_and_swap<uint32_t>(swr_ptr + 0, 0x38600000);
                          xe::store_and_swap<uint32_t>(swr_ptr + 4, 0x4E800020);
                          XELOGI("DC3: Calibration bypass: stubbed "
                                 "ShouldWaitForRecovery at {:08X} to return false",
                                 kShouldWaitForRecovery);
                        });

      constexpr uint32_t kExitControllerMode = 0x82902748;
      with_patch_target("ExitControllerMode", kExitControllerMode, 4,
                        [&](uint8_t* ecm_ptr) {
                          xe::store_and_swap<uint32_t>(ecm_ptr, 0x4E800020);
                          XELOGI("DC3: Controller bypass: stubbed "
                                 "ExitControllerMode at {:08X} to blr",
                                 kExitControllerMode);
                        });

      // Blocker A (early auto-pause during gameplay): with --fake_kinect_data,
      // the synthetic skeleton is never registered as a "playing" player, so
      // Game::CheckForSkeletonLoss() sees numPlaying(0) < threshold(1) every
      // SkeletonUpdate and calls Game::PauseForSkeletonLoss() ~2s into the song
      // -> Handle(pause_game) -> perform_pause_screen, killing playback.
      // Confirmed root cause: PAUSE-ONSET DIAG showed the UIEventMgr dialog
      // queue EMPTY at the pause onset (qsize=0), ruling out GamePanel::Poll's
      // HasActiveDialogEvent() branch and pinning it on the skeleton-loss path.
      // Real Kinect would mark the player present and this never fires; under
      // fake input it's a false positive. Stub the void Game::PauseForSkeletonLoss
      // (private, non-virtual; 0x82866D50) to a bare blr so the song keeps
      // playing. Scoped to the fake-Kinect block since that's the only case that
      // produces the false skeleton loss. The player-count computation in
      // CheckForSkeletonLoss is left intact; only the pause action is removed.
      constexpr uint32_t kPauseForSkeletonLoss = 0x82866D50;
      with_patch_target("Game::PauseForSkeletonLoss", kPauseForSkeletonLoss, 4,
                        [&](uint8_t* pfsl_ptr) {
                          xe::store_and_swap<uint32_t>(pfsl_ptr, 0x4E800020);
                          XELOGI("DC3: Gameplay fix: stubbed "
                                 "Game::PauseForSkeletonLoss at {:08X} to blr "
                                 "(suppress fake-Kinect false skeleton-loss "
                                 "auto-pause)",
                                 kPauseForSkeletonLoss);
                        });

      constexpr uint32_t kMoviePoll = 0x82555CB8;
      with_patch_target("Movie::Poll", kMoviePoll, 8,
                        [&](uint8_t* mp_ptr) {
                          xe::store_and_swap<uint32_t>(mp_ptr + 0, 0x38600000);
                          xe::store_and_swap<uint32_t>(mp_ptr + 4, 0x4E800020);
                          XELOGI("DC3: Movie bypass: stubbed Movie::Poll at "
                                 "{:08X} to return false",
                                 kMoviePoll);
                        });

      auto patch4 = [&](uint32_t addr, uint32_t val, const char* desc) {
        auto* p = memory_->TranslateVirtual<uint8_t*>(addr);
        if (!p) {
          return;
        }
        auto* h = memory_->LookupHeap(addr);
        if (!h) {
          return;
        }
        h->Protect(addr, 4, kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(p, val);
        XELOGI("DC3: Audio fix: {} at {:08X}", desc, addr);
      };

      constexpr uint32_t kXMAHALAlloc = 0x82E77250;
      with_patch_target("XMAHALAllocateContexts", kXMAHALAlloc, 8,
                        [&](uint8_t* p) {
                          xe::store_and_swap<uint32_t>(p + 0, 0x38600000);
                          xe::store_and_swap<uint32_t>(p + 4, 0x4E800020);
                          XELOGI("DC3: Audio fix: stubbed XMAHALAllocateContexts "
                                 "at {:08X} to return S_OK",
                                 kXMAHALAlloc);
                        });

      patch4(0x82867288 + 0x90, 0x48000024,
             "HandleWait+0x90: bne 40820024 -> b 48000024");
      patch4(0x8252B9E0 + 0x70, 0x38600001,
             "HamAudio::IsReady+0x70: bctrl -> li r3,1");

      constexpr uint32_t kHamDirectorSongAnim = 0x82475578;
      with_patch_target("HamDirector::SongAnim", kHamDirectorSongAnim, 8,
                        [&](uint8_t* p) {
                          // SongAnim(playerIndex): force the pre-authored EXPERT
                          // song.anim (which has baked clip keyframes) instead of
                          // the routine-builder anim (empty headless — the
                          // remixer never runs). Mirrors the #ifdef HX_NATIVE
                          // fallback compiled out of debug.xex.
                          //   li r4,2 (kDifficultyExpert)
                          //   b  0x82473E58  (HamDirector::SongAnimByDifficulty)
                          // NOTE: branch MUST target the function ENTRY 0x82473E58
                          // (0x4BFFE8DC), NOT 0x82473E5C/+4 (0x4BFFE8E0). The +4
                          // target landed on the SongAnimByDifficulty survival
                          // patch's `blr`, skipping `li r3,0`, so SongAnim returned
                          // r3 unchanged == TheHamDirector -> ClipPlayer::Init then
                          // called GetKeys with this==HamDirector -> infinite hang.
                          // (Survival patch now removed; SongAnimByDifficulty runs
                          // its real `return mSongAnims[diff]` on the healthy map.)
                          xe::store_and_swap<uint32_t>(p + 0, 0x38800002);
                          xe::store_and_swap<uint32_t>(p + 4, 0x4BFFE8DC);
                          XELOGI("DC3: Anim fix: patched HamDirector::SongAnim "
                                 "at {:08X} to tail-call SongAnimByDifficulty"
                                 "(expert)",
                                 kHamDirectorSongAnim);
                        });
    }

    // Apply IK telemetry instrumentation to the original XEX if requested.
    if (cvars::dc3_ik_telemetry) {
      Dc3HackContext ik_ctx;
      ik_ctx.memory = memory_.get();
      ik_ctx.processor = processor_.get();
      ik_ctx.module = module.get();
      ik_ctx.is_decomp_layout = false;
      auto ik_result = ApplyDc3IKTelemetry(ik_ctx);
      XELOGI("DC3: IK telemetry (original XEX): applied={} skipped={} failed={}",
             ik_result.applied, ik_result.skipped, ik_result.failed);
    }
  }
  auto main_thread = kernel_state_->LaunchModule(module);
  if (!main_thread) {
    return X_STATUS_UNSUCCESSFUL;
  }
  main_thread_ = main_thread;
  on_launch(title_id_.value(), title_name_);

  return X_STATUS_SUCCESS;
}

}  // namespace xe
