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
#include <cstring>
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
DEFINE_bool(dc3_null_read_cache_stream, false,
            "DC3: override ReadCacheStream to return nullptr, preventing DTB "
            "loading. Was needed before DataReadFile was decomped; now that "
            "the decomp has ReadCacheStream, this should be false.",
            "DC3");
DEFINE_bool(dc3_debug_mempool_alloc_probe, false,
            "DC3: log-only probe for MemOrPoolAlloc. Captures caller LR, "
            "requested size, file/line/name args, and return value. "
            "Detailed logs only on failure or from known crash-path callers.",
            "DC3");
DEFINE_bool(dc3_ik_telemetry, false,
            "DC3: instrument HamIKEffector functions via PPC bytepatch to "
            "capture totalWeight (ApplyConstraints) and groundHeight "
            "(GetGroundHeight) return values. Logs to Xenia log every 60 "
            "frames. Use to diagnose foot-floor clipping divergence.",
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

// Guest extern handler that returns S_OK and fills a NUI_SKELETON_FRAME with
// one tracked skeleton in T-pose.  Used when --fake_kinect_data=true so the
// game's Kinect calibration screen can detect a "player" and proceed.
void Dc3NuiFakeSkeletonGetNextFrameExtern(
    cpu::ppc::PPCContext* ppc_context, kernel::KernelState* kernel_state) {
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }

  // r4 = guest pointer to NUI_SKELETON_FRAME output buffer.
  uint32_t frame_guest_addr = static_cast<uint32_t>(ppc_context->r[4]);
  if (!frame_guest_addr) {
    // Null pointer — return E_POINTER.
    ppc_context->r[3] = 0x80004003u;
    return;
  }

  Memory* memory = kernel_state->memory();
  auto* frame = memory->TranslateVirtual<uint8_t*>(frame_guest_addr);
  if (!frame) {
    ppc_context->r[3] = 0x80004005u;  // E_FAIL
    return;
  }

  // Helper lambdas for big-endian writes into guest memory.
  auto write_u32 = [frame](uint32_t offset, uint32_t value) {
    xe::store_and_swap<uint32_t>(frame + offset, value);
  };
  auto write_u64 = [frame](uint32_t offset, uint64_t value) {
    xe::store_and_swap<uint64_t>(frame + offset, value);
  };
  auto write_float = [frame](uint32_t offset, float value) {
    xe::store_and_swap<float>(frame + offset, value);
  };

  // Incrementing frame counter (static — persists across calls).
  static uint32_t s_fake_frame_number = 0;
  uint32_t frame_number = ++s_fake_frame_number;

  // --- NUI_SKELETON_FRAME layout ---
  // 0x00: LARGE_INTEGER liTimeStamp (8 bytes)
  // 0x08: DWORD dwFrameNumber (4 bytes)
  // 0x0C: DWORD dwFlags (4 bytes)
  // 0x10: Vector4 vFloorClipPlane (16 bytes)
  // 0x20: Vector4 vNormalToGravity (16 bytes)
  // 0x30: NUI_SKELETON_DATA SkeletonData[6] (each ~0x1B4 bytes)

  // Zero the entire frame first.  Size of NUI_SKELETON_FRAME:
  //   0x30 header + 6 * 0x1B4 skeletons = 0x30 + 0xA38 = 0xA68
  constexpr uint32_t kFrameSize = 0x30 + 6 * 0x1B4;
  std::memset(frame, 0, kFrameSize);

  // Timestamp — use frame number as a simple monotonic value.
  write_u64(0x00, static_cast<uint64_t>(frame_number) * 33333);
  // Frame number.
  write_u32(0x08, frame_number);
  // dwFlags = 0.
  // vFloorClipPlane: (0, 1, 0, 0) — Y-up floor at origin.
  write_float(0x10, 0.0f);
  write_float(0x14, 1.0f);
  write_float(0x18, 0.0f);
  write_float(0x1C, 0.0f);
  // vNormalToGravity: (0, 1, 0, 0).
  write_float(0x20, 0.0f);
  write_float(0x24, 1.0f);
  write_float(0x28, 0.0f);
  write_float(0x2C, 0.0f);

  // --- First skeleton (index 0) at offset 0x30 ---
  // NUI_SKELETON_DATA layout:
  //   0x00: eTrackingState (4)
  //   0x04: dwTrackingID (4)
  //   0x08: dwEnrollmentIndex (4)
  //   0x0C: dwUserIndex (4)
  //   0x10: Position (Vector4, 16)
  //   0x20: SkeletonPositions[20] (20 * 16 = 320)
  //   0x160: eSkeletonPositionTrackingState[20] (20 * 4 = 80)
  //   0x1B0: dwQualityFlags (4)
  constexpr uint32_t kSkel0 = 0x30;

  // eTrackingState = NUI_SKELETON_TRACKED (2).
  write_u32(kSkel0 + 0x00, 2);
  // dwTrackingID = 1.
  write_u32(kSkel0 + 0x04, 1);
  // dwEnrollmentIndex = 0.
  // dwUserIndex = 0.
  // Position (center of mass): (0, 0.9, 2, 0).
  write_float(kSkel0 + 0x10, 0.0f);
  write_float(kSkel0 + 0x14, 0.9f);
  write_float(kSkel0 + 0x18, 2.0f);
  write_float(kSkel0 + 0x1C, 0.0f);

  // T-pose joint positions (Y-up, meters from sensor).
  struct JointPos { float x, y, z; };
  static constexpr JointPos kJoints[20] = {
      { 0.00f, 0.90f, 2.0f},  // 0  HIP_CENTER
      { 0.00f, 1.10f, 2.0f},  // 1  SPINE
      { 0.00f, 1.40f, 2.0f},  // 2  SHOULDER_CENTER
      { 0.00f, 1.60f, 2.0f},  // 3  HEAD
      {-0.20f, 1.40f, 2.0f},  // 4  SHOULDER_LEFT
      {-0.30f, 1.10f, 2.0f},  // 5  ELBOW_LEFT (arms at sides)
      {-0.25f, 0.90f, 2.0f},  // 6  WRIST_LEFT (arms at sides)
      {-0.20f, 0.80f, 2.0f},  // 7  HAND_LEFT (arms at sides)
      { 0.20f, 1.40f, 2.0f},  // 8  SHOULDER_RIGHT
      { 0.30f, 1.10f, 2.0f},  // 9  ELBOW_RIGHT (arms at sides)
      { 0.25f, 0.90f, 2.0f},  // 10 WRIST_RIGHT (arms at sides)
      { 0.20f, 0.80f, 2.0f},  // 11 HAND_RIGHT (arms at sides)
      {-0.10f, 0.90f, 2.0f},  // 12 HIP_LEFT
      {-0.10f, 0.50f, 2.0f},  // 13 KNEE_LEFT
      {-0.10f, 0.10f, 2.0f},  // 14 ANKLE_LEFT
      {-0.10f, 0.00f, 2.0f},  // 15 FOOT_LEFT
      { 0.10f, 0.90f, 2.0f},  // 16 HIP_RIGHT
      { 0.10f, 0.50f, 2.0f},  // 17 KNEE_RIGHT
      { 0.10f, 0.10f, 2.0f},  // 18 ANKLE_RIGHT
      { 0.10f, 0.00f, 2.0f},  // 19 FOOT_RIGHT
  };
  constexpr uint32_t kJointsOff = kSkel0 + 0x20;
  for (int j = 0; j < 20; j++) {
    uint32_t off = kJointsOff + j * 16;
    write_float(off + 0, kJoints[j].x);
    write_float(off + 4, kJoints[j].y);
    write_float(off + 8, kJoints[j].z);
    write_float(off + 12, 0.0f);  // w
  }

  // All 20 position tracking states = NUI_SKELETON_POSITION_TRACKED (0).
  // Already zero from memset — nothing to do.

  // dwQualityFlags = 0 (no clipping).
  // Already zero from memset.

  // Remaining 5 skeletons: eTrackingState = NUI_SKELETON_NOT_TRACKED (0).
  // Already zero from memset.

  // Return S_OK.
  ppc_context->r[3] = 0;

  // --- Calibration bypass: continuously force-write tracking IDs ---
  // Every frame, write tracking ID 1/2 to both players'
  // HamPlayerData.mSkeletonTrackingID (offset 0x60).  This must be
  // continuous because the game's SetPlayerSkeletonNavData resets IDs
  // to -1 each frame when the filtered skeleton ID doesn't match.
  // Combined with the NOP'd IsTrackingAllSkeletons guard in
  // SetPlayerPresent, this makes the multiuser panel see both players
  // as present and fire enter_gameplay.
  static int s_skel_calls = 0;
  static bool s_logged = false;
  static bool s_nui_entry_logged = false;
  ++s_skel_calls;
  if (!s_nui_entry_logged) {
    XELOGI("DC3: NUI callback alive (original layout) frameBuf={:08X} "
           "fakeFrame={} nuiFrame={}",
           frame_guest_addr, frame_number, s_skel_calls);
    s_nui_entry_logged = true;
  }

  // --- Host-side timer advancement ---
  // The PPC __mftb() instruction returns values from Clock::QueryGuestTickCount()
  // which advances correctly on the host side. However, the guest Timer objects
  // (LiveInput::mTimer and TaskMgr::mTime) show frozen mCycles values, suggesting
  // their Split() calls are somehow not accumulating deltas.
  //
  // Fix: every NUI frame (~16ms), compute the guest tick delta since the last
  // frame and add it directly to both Timer objects' mCycles fields. Also update
  // mStart to the current low-32 guest tick so the guest's own Split() calls
  // compute near-zero deltas (rather than stale ones).
  {
    static uint64_t s_last_guest_tick = 0;
    static bool s_timer_advance_logged = false;
    uint64_t guest_tick = Clock::QueryGuestTickCount();

    if (s_last_guest_tick == 0) {
      s_last_guest_tick = guest_tick;
    }

    uint64_t tick_delta = guest_tick - s_last_guest_tick;
    s_last_guest_tick = guest_tick;
    uint32_t guest_tick_lo = static_cast<uint32_t>(guest_tick);

    // Advance TaskMgr.mTime timer (TheTaskMgr + 0x50)
    constexpr uint32_t kTheTaskMgr_ta = 0x82F64A58;
    auto* tm_ta = memory->TranslateVirtual<uint8_t*>(kTheTaskMgr_ta);
    if (tm_ta) {
      // Timer layout: +0x00=mStart(u32), +0x08=mCycles(u64), +0x24=mRunning(i32)
      int32_t running = xe::load_and_swap<int32_t>(tm_ta + 0x50 + 0x24);
      if (running > 0) {
        uint64_t cycles = xe::load_and_swap<uint64_t>(tm_ta + 0x50 + 0x08);
        cycles += tick_delta;
        xe::store_and_swap<uint64_t>(tm_ta + 0x50 + 0x08, cycles);
        xe::store_and_swap<uint32_t>(tm_ta + 0x50, guest_tick_lo);
      }
    }

    // Advance LiveInput.mTimer (Game->mGameInput->+0x10)
    constexpr uint32_t kTheGamePanel_ta = 0x83117410;
    auto* gp_ta = memory->TranslateVirtual<uint8_t*>(kTheGamePanel_ta);
    if (gp_ta) {
      uint32_t gp_addr = xe::load_and_swap<uint32_t>(gp_ta);
      if (gp_addr && gp_addr < 0xF0000000) {
        auto* gp = memory->TranslateVirtual<uint8_t*>(gp_addr);
        if (gp) {
          uint32_t game_addr = xe::load_and_swap<uint32_t>(gp + 0x38);
          if (game_addr && game_addr < 0xF0000000) {
            auto* game = memory->TranslateVirtual<uint8_t*>(game_addr);
            if (game) {
              uint32_t input_addr = xe::load_and_swap<uint32_t>(game + 0x54);
              if (input_addr && input_addr < 0xF0000000) {
                auto* input = memory->TranslateVirtual<uint8_t*>(input_addr);
                if (input) {
                  int32_t running = xe::load_and_swap<int32_t>(input + 0x10 + 0x24);
                  if (running > 0) {
                    uint64_t cycles = xe::load_and_swap<uint64_t>(input + 0x10 + 0x08);
                    cycles += tick_delta;
                    xe::store_and_swap<uint64_t>(input + 0x10 + 0x08, cycles);
                    xe::store_and_swap<uint32_t>(input + 0x10, guest_tick_lo);
                  }
                }
              }
            }
          }
        }
      }
    }

    if (!s_timer_advance_logged && s_skel_calls >= 300) {
      XELOGI("DC3: Timer advance active — tick_delta={} guest_tick={} "
             "guest_tick_lo={} nuiFrame={}",
             tick_delta, guest_tick, guest_tick_lo, s_skel_calls);
      s_timer_advance_logged = true;
    }
  }

  if (s_skel_calls >= 300) {  // Wait ~5s for game init
    constexpr uint32_t kTheGameData = 0x82F60034;
    auto* gd_ptr = memory->TranslateVirtual<uint8_t*>(kTheGameData);
    if (gd_ptr) {
      uint32_t gd_addr = xe::load_and_swap<uint32_t>(gd_ptr);
      if (gd_addr) {
        auto* players_ptr =
            memory->TranslateVirtual<uint8_t*>(gd_addr + 0x38);
        if (players_ptr) {
          uint32_t players_base = xe::load_and_swap<uint32_t>(players_ptr);
          if (players_base) {
            for (int i = 0; i < 2; i++) {
              auto* pp =
                  memory->TranslateVirtual<uint8_t*>(players_base + i * 4);
              if (!pp) continue;
              uint32_t pa = xe::load_and_swap<uint32_t>(pp);
              if (!pa) continue;
              auto* tid =
                  memory->TranslateVirtual<uint8_t*>(pa + 0x60);
              if (!tid) continue;
              xe::store_and_swap<uint32_t>(
                  tid, static_cast<uint32_t>(i + 1));
            }
            if (!s_logged) {
              XELOGI("DC3: Calibration bypass: forcing tracking IDs "
                     "continuously (players at {:08X})",
                     players_base);
              s_logged = true;
            }
          }
        }
      }
    }
  }

  // --- IK telemetry readout (piggyback on NUI handler, called every frame) ---
  if (cvars::dc3_ik_telemetry && (s_skel_calls % 60) == 0 && s_skel_calls >= 600) {
    // Read telemetry slots written by PPC code caves.
    constexpr uint32_t kIkSlotsAddr = 0x00038000;
    auto* slots = memory->TranslateVirtual<uint8_t*>(kIkSlotsAddr);
    if (slots) {
      float totalWeight = xe::load_and_swap<float>(slots + 0);
      float groundHeight = xe::load_and_swap<float>(slots + 4);
      uint32_t effectorType = xe::load_and_swap<uint32_t>(slots + 8);
      float posWeight = xe::load_and_swap<float>(slots + 12);
      uint32_t thisPtr = xe::load_and_swap<uint32_t>(slots + 16);
      float ikElbowZ = xe::load_and_swap<float>(slots + 20);
      float fancyWeight = xe::load_and_swap<float>(slots + 24);

      static const char* kTypeNames[] = {
        "none", "pelvis", "ankle", "hand", "forearm", "head"
      };
      const char* typeName = (effectorType < 6)
          ? kTypeNames[effectorType] : "?";

      // Read member data through the captured this pointer.
      // ObjPtr layout (Xbox 360 MSVC ABI):
      //   +0x00: vtable, +0x04: next, +0x08: prev, +0x0C: mObject (T*)
      constexpr uint32_t kObjPtrObj = 0x0C;
      // HamIKEffector member offsets:
      constexpr uint32_t kMEffector = 0x44;  // ObjPtr<RndTransformable>
      constexpr uint32_t kMGround = 0x6C;    // ObjPtr<RndTransformable>
      constexpr uint32_t kMMore = 0x80;       // ObjPtr<HamIKEffector>
      // RndTransformable offsets:
      constexpr uint32_t kWorldXfmV = 0x6C;  // mWorldXfm.v.x (Transform.m=0x24, then v)
      constexpr uint32_t kDirtyOff = 0xBD;   // mDirty

      uint32_t mEffector = 0, mGround = 0;
      float effWorldX = 0, effWorldY = 0, effWorldZ = 0;
      uint8_t effDirty = 0;
      if (thisPtr && thisPtr < 0xF0000000) {
        auto* obj = memory->TranslateVirtual<uint8_t*>(thisPtr);
        if (obj) {
          mEffector = xe::load_and_swap<uint32_t>(
              obj + kMEffector + kObjPtrObj);
          mGround = xe::load_and_swap<uint32_t>(
              obj + kMGround + kObjPtrObj);

          // Read effector bone WorldXfm position and dirty flag.
          if (mEffector && mEffector < 0xF0000000) {
            auto* eff = memory->TranslateVirtual<uint8_t*>(mEffector);
            if (eff) {
              effWorldX = xe::load_and_swap<float>(eff + kWorldXfmV);
              effWorldY = xe::load_and_swap<float>(eff + kWorldXfmV + 4);
              effWorldZ = xe::load_and_swap<float>(eff + kWorldXfmV + 8);
              effDirty = eff[kDirtyOff];
            }
          }
        }
      }

      if (totalWeight != 0.0f || thisPtr != 0) {
        XELOGI("DC3:IK [nuiFrame {}] type={} totalWeight={:.4f} "
               "groundHeight={:.4f} posWeight={:.4f} "
               "ikElbowZ={:.4f} fancyWeight={:.4f} this={:08X} "
               "effector={:08X} ground={:08X} "
               "effWorldPos=({:.4f},{:.4f},{:.4f}) effDirty={}",
               s_skel_calls, typeName, totalWeight, groundHeight,
               posWeight, ikElbowZ, fancyWeight, thisPtr,
               mEffector, mGround,
               effWorldX, effWorldY, effWorldZ, effDirty);
      } else if ((s_skel_calls % 600) == 0) {
        XELOGI("DC3:IK [nuiFrame {}] NO DATA — all slots zero "
               "(HamIKEffector not active)", s_skel_calls);
      }

      // Clear the slots after reading so we capture fresh data each sample.
      memset(slots, 0, 28);
    }
  }

  // --- Bone dirty probe (post-Character::Poll state) ---
  // Reads mDirty flags from ankle/knee/thigh/pelvis bones to determine
  // whether SetWorldXfm's dirty cascade overwrites IK corrections.
  // Runs every 120 frames (~2s) starting at frame 1200 (~20s).
  if ((s_skel_calls % 60) == 0 && s_skel_calls >= 1200) {
    static bool s_bone_probe_done = false;
    if (!s_bone_probe_done) {
      // Read TheHamWardrobe pointer.
      constexpr uint32_t kTheHamWardrobe = 0x82F60110;
      auto* hw_ptr_mem = memory->TranslateVirtual<uint8_t*>(kTheHamWardrobe);
      uint32_t hw_addr = hw_ptr_mem
          ? xe::load_and_swap<uint32_t>(hw_ptr_mem) : 0;

      if (hw_addr && hw_addr < 0xF0000000) {
        auto* hw = memory->TranslateVirtual<uint8_t*>(hw_addr);
        if (hw) {
          // HamWardrobe layout:
          //   0x00: vbptr
          //   0x04: mCrowdMembers (ObjPtrList, 0x14 bytes)
          //   0x18: mMainCharacters (ObjPtrVec<HamCharacter>, 0x1C bytes)
          //     +0x04: mNodes (std::vector<Node>)
          //       +0x00: _Myfirst (Node*)
          //       +0x04: _Mylast (Node*)
          // Each Node is 0x14 bytes; mObject at +0x0C.
          uint32_t nodes_first = xe::load_and_swap<uint32_t>(hw + 0x1C);
          uint32_t nodes_last = xe::load_and_swap<uint32_t>(hw + 0x20);

          int num_chars = 0;
          if (nodes_first && nodes_last && nodes_last >= nodes_first) {
            num_chars = (nodes_last - nodes_first) / 0x14;
          }

          XELOGI("DC3:BONE [nuiFrame {}] TheHamWardrobe={:08X} "
                 "mMainCharacters: {} chars (nodes {:08X}..{:08X})",
                 s_skel_calls, hw_addr, num_chars, nodes_first, nodes_last);

          // Process player 0's character (index 0).
          if (num_chars > 0 && nodes_first && nodes_first < 0xF0000000) {
            auto* node0 = memory->TranslateVirtual<uint8_t*>(nodes_first);
            uint32_t char_addr = node0
                ? xe::load_and_swap<uint32_t>(node0 + 0x0C) : 0;

            if (char_addr && char_addr < 0xF0000000) {
              auto* ch = memory->TranslateVirtual<uint8_t*>(char_addr);
              if (ch) {
                // ObjectDir::mHashTable at Character* + 0x8
                // KeylessHash layout:
                //   +0x00: mEntries (Entry*)
                //   +0x04: mSize (int)
                //   +0x0C: mNumEntries (int)
                // Entry is 8 bytes: {const char* name, Hmx::Object* obj}
                uint32_t entries_addr = xe::load_and_swap<uint32_t>(ch + 0x8);
                int table_size = xe::load_and_swap<int32_t>(ch + 0xC);
                int num_entries = xe::load_and_swap<int32_t>(ch + 0x14);

                XELOGI("DC3:BONE [nuiFrame {}] Character[0]={:08X} "
                       "hashTable: entries={:08X} size={} used={}",
                       s_skel_calls, char_addr, entries_addr,
                       table_size, num_entries);

                // Target bone names to search for.
                static const char* kBoneNames[] = {
                    "bone_pelvis.mesh",
                    "bone_L-thigh.mesh",
                    "bone_L-knee.mesh",
                    "bone_L-ankle.mesh",
                    "bone_R-thigh.mesh",
                    "bone_R-knee.mesh",
                    "bone_R-ankle.mesh",
                };
                constexpr int kNumBones = 7;
                struct BoneResult {
                  const char* name;
                  uint32_t obj_addr;     // Hmx::Object* from hash table
                  uint32_t complete_addr; // complete object via COL
                  uint8_t dirty;
                  float world_z;
                  uint32_t parent_trans;  // RndTransformable* mParent
                  uint8_t parent_dirty;
                  bool found;
                };
                BoneResult results[kNumBones] = {};
                for (int b = 0; b < kNumBones; b++) {
                  results[b].name = kBoneNames[b];
                  results[b].found = false;
                }

                {
                    // Also search subdirectories for bones.
                    // ObjectDir::mSubDirs at +0x50 from Character*
                    // std::vector<ObjDirPtr<ObjectDir>> — each element 0x14
                    // bytes, mObject at +0x0C within each ObjDirPtr.
                    uint32_t subdirs_first = xe::load_and_swap<uint32_t>(
                        ch + 0x50);
                    uint32_t subdirs_last = xe::load_and_swap<uint32_t>(
                        ch + 0x54);
                    int num_subdirs = 0;
                    if (subdirs_first && subdirs_last &&
                        subdirs_last >= subdirs_first) {
                      num_subdirs = (subdirs_last - subdirs_first) / 0x14;
                    }
                    XELOGI("DC3:BONE [nuiFrame {}] {} subdirs "
                           "(first={:08X} last={:08X})",
                           s_skel_calls, num_subdirs,
                           subdirs_first, subdirs_last);

                    // Collect up to 8 hash tables to search
                    // (main + subdirs)
                    struct HashTableInfo {
                      uint32_t entries_addr;
                      int table_size;
                      const char* label;
                    };
                    HashTableInfo hash_tables[8];
                    int num_tables = 0;
                    if (entries_addr && entries_addr < 0xF0000000 &&
                        table_size > 0 && table_size < 100000) {
                      hash_tables[num_tables++] = {
                          entries_addr, table_size, "main"};
                    }

                    if (subdirs_first && subdirs_first < 0xF0000000 &&
                        num_subdirs > 0 && num_subdirs < 20) {
                      auto* sd_mem = memory->TranslateVirtual<uint8_t*>(
                          subdirs_first);
                      if (sd_mem) {
                        for (int s = 0; s < num_subdirs && num_tables < 8;
                             s++) {
                          uint32_t dir_obj =
                              xe::load_and_swap<uint32_t>(
                                  sd_mem + s * 0x14 + 0x0C);
                          if (dir_obj && dir_obj < 0xF0000000) {
                            auto* dir_mem =
                                memory->TranslateVirtual<uint8_t*>(dir_obj);
                            if (dir_mem) {
                              uint32_t sd_entries =
                                  xe::load_and_swap<uint32_t>(dir_mem + 0x8);
                              int sd_size =
                                  xe::load_and_swap<int32_t>(dir_mem + 0xC);
                              int sd_used =
                                  xe::load_and_swap<int32_t>(dir_mem + 0x14);
                              XELOGI("DC3:BONE   subdir[{}]={:08X} "
                                     "hashTable: entries={:08X} size={} "
                                     "used={}",
                                     s, dir_obj, sd_entries,
                                     sd_size, sd_used);
                              if (sd_entries && sd_entries < 0xF0000000 &&
                                  sd_size > 0 && sd_size < 100000) {
                                hash_tables[num_tables++] = {
                                    sd_entries, sd_size, "subdir"};
                              }
                            }
                          }
                        }
                      }
                    }

                    // Search all hash tables for target bones.
                    for (int t = 0; t < num_tables; t++) {
                      auto* cur_entries =
                        memory->TranslateVirtual<uint8_t*>(
                            hash_tables[t].entries_addr);
                    if (!cur_entries) continue;

                    // Debug: on first table, dump some bone-like names
                    static int s_table_dumps = 0;
                    if (s_table_dumps < num_tables && t < 3) {
                      s_table_dumps++;
                      int dumped = 0;
                      for (int i = 0;
                           i < hash_tables[t].table_size && dumped < 10;
                           i++) {
                        uint32_t np = xe::load_and_swap<uint32_t>(
                            cur_entries + i * 8);
                        uint32_t op = xe::load_and_swap<uint32_t>(
                            cur_entries + i * 8 + 4);
                        if (np == 0 && op == 0) continue;
                        auto* nm = (np && np < 0xF0000000)
                            ? memory->TranslateVirtual<const char*>(np)
                            : nullptr;
                        if (!nm) continue;
                        // Only dump bone_ entries
                        if (nm[0] == 'b' && nm[1] == 'o' &&
                            nm[2] == 'n' && nm[3] == 'e') {
                          char safe_name[41] = {};
                          for (int c = 0; c < 40; c++) {
                            safe_name[c] = nm[c];
                            if (nm[c] == '\0') break;
                          }
                          XELOGI("DC3:BONE   {}[{}] '{}' obj={:08X}",
                                 hash_tables[t].label, i, safe_name, op);
                          dumped++;
                        }
                      }
                    }

                    for (int i = 0; i < hash_tables[t].table_size; i++) {
                      uint32_t name_ptr = xe::load_and_swap<uint32_t>(
                          cur_entries + i * 8);
                      uint32_t obj_ptr = xe::load_and_swap<uint32_t>(
                          cur_entries + i * 8 + 4);
                      if (name_ptr == 0 || obj_ptr == 0) continue;
                      if (name_ptr >= 0xF0000000 || obj_ptr >= 0xF0000000)
                          continue;

                      auto* name_mem = memory->TranslateVirtual<const char*>(
                          name_ptr);
                      if (!name_mem) continue;

                      // Compare with target bone names.
                      for (int b = 0; b < kNumBones; b++) {
                        if (results[b].found) continue;
                        // Safe string compare (max 32 chars).
                        bool match = true;
                        for (int c = 0; c < 32; c++) {
                          if (name_mem[c] != kBoneNames[b][c]) {
                            match = false;
                            break;
                          }
                          if (name_mem[c] == '\0') break;
                        }
                        if (!match) continue;

                        results[b].found = true;
                        results[b].obj_addr = obj_ptr;

                        // Use RTTI Complete Object Locator to find the
                        // complete object from the Hmx::Object* vbase.
                        // In MSVC: instance vfptr points to first vtable
                        // entry; COL pointer is at vfptr[-1].
                        // COL+4 = offset of this vftable from complete obj.
                        auto* obj_mem = memory->TranslateVirtual<uint8_t*>(
                            obj_ptr);
                        if (!obj_mem) break;

                        uint32_t vftable = xe::load_and_swap<uint32_t>(
                            obj_mem);
                        if (!vftable || vftable >= 0xF0000000) break;

                        auto* vft_mem = memory->TranslateVirtual<uint8_t*>(
                            vftable);
                        if (!vft_mem) break;

                        uint32_t col_ptr = xe::load_and_swap<uint32_t>(
                            vft_mem - 4);
                        if (!col_ptr || col_ptr >= 0xF0000000) {
                          XELOGI("DC3:BONE   {} vftable={:08X} "
                                 "COL candidate {:08X} invalid",
                                 kBoneNames[b], vftable, col_ptr);
                          break;
                        }

                        auto* col_mem = memory->TranslateVirtual<uint8_t*>(
                            col_ptr);
                        if (!col_mem) break;

                        uint32_t col_sig = xe::load_and_swap<uint32_t>(
                            col_mem);
                        uint32_t vf_offset = xe::load_and_swap<uint32_t>(
                            col_mem + 4);
                        uint32_t cd_offset = xe::load_and_swap<uint32_t>(
                            col_mem + 8);

                        XELOGI("DC3:BONE   {} vftable={:08X} COL={:08X} "
                               "sig={} vfOff=0x{:X} cdOff=0x{:X}",
                               kBoneNames[b], vftable, col_ptr,
                               col_sig, vf_offset, cd_offset);

                        uint32_t complete_addr = obj_ptr - vf_offset;
                        results[b].complete_addr = complete_addr;

                        auto* complete = memory->TranslateVirtual<uint8_t*>(
                            complete_addr);
                        if (!complete) break;

                        // mDirty at complete_obj + 0xBD (RndTransformable)
                        // This offset is verified for standalone
                        // RndTransformable. For RndMesh (which bones are),
                        // the actual offset depends on MI layout.
                        // Read at 0xBD and also dump nearby bytes for
                        // diagnostic verification.
                        results[b].dirty = complete[0xBD];

                        // mWorldXfm.v.z: for standalone RndTransformable,
                        // mWorldXfm is at +0x48, v.z at +0x74.
                        results[b].world_z = xe::load_and_swap<float>(
                            complete + 0x74);

                        // Dump 16 bytes around 0xBD for diagnostics
                        // (helps verify the mDirty offset).
                        XELOGI("DC3:BONE   {} complete={:08X} "
                               "bytes[0xB0..0xBF]={:02X} {:02X} {:02X} {:02X} "
                               "{:02X} {:02X} {:02X} {:02X} "
                               "{:02X} {:02X} {:02X} {:02X} "
                               "{:02X} {:02X} {:02X} {:02X}",
                               kBoneNames[b], complete_addr,
                               complete[0xB0], complete[0xB1],
                               complete[0xB2], complete[0xB3],
                               complete[0xB4], complete[0xB5],
                               complete[0xB6], complete[0xB7],
                               complete[0xB8], complete[0xB9],
                               complete[0xBA], complete[0xBB],
                               complete[0xBC], complete[0xBD],
                               complete[0xBE], complete[0xBF]);

                        // mParent.mObject at complete_obj + 0x94
                        // (ObjOwnerPtr<RndTransformable> at +0x88,
                        //  .mObject at +0x0C within ObjOwnerPtr)
                        uint32_t parent_trans = xe::load_and_swap<uint32_t>(
                            complete + 0x94);
                        results[b].parent_trans = parent_trans;

                        // For parent: the mParent ObjOwnerPtr stores a
                        // RndTransformable*. Use COL on the
                        // RndTransformable vtable to get the parent's
                        // complete object, then read mDirty at +0xBD.
                        if (parent_trans && parent_trans < 0xF0000000) {
                          auto* parent_mem =
                              memory->TranslateVirtual<uint8_t*>(parent_trans);
                          if (parent_mem) {
                            uint32_t p_vft = xe::load_and_swap<uint32_t>(
                                parent_mem);
                            if (p_vft && p_vft < 0xF0000000) {
                              auto* p_vft_mem =
                                  memory->TranslateVirtual<uint8_t*>(p_vft);
                              if (p_vft_mem) {
                                uint32_t p_col = xe::load_and_swap<uint32_t>(
                                    p_vft_mem - 4);
                                if (p_col && p_col < 0xF0000000) {
                                  auto* p_col_mem =
                                      memory->TranslateVirtual<uint8_t*>(
                                          p_col);
                                  if (p_col_mem) {
                                    uint32_t p_off =
                                        xe::load_and_swap<uint32_t>(
                                            p_col_mem + 4);
                                    uint32_t p_complete =
                                        parent_trans - p_off;
                                    auto* pc =
                                        memory->TranslateVirtual<uint8_t*>(
                                            p_complete);
                                    if (pc) {
                                      results[b].parent_dirty = pc[0xBD];
                                    }
                                  }
                                }
                              }
                            }
                          }
                        }
                        break;  // found this bone
                      }
                    }
                  }
                }  // end for (int t = 0; t < num_tables; t++)

                // Log results.
                for (int b = 0; b < kNumBones; b++) {
                  if (results[b].found) {
                    XELOGI("DC3:BONE [nuiFrame {}] {} obj={:08X} "
                           "complete={:08X} dirty={} worldZ={:.4f} "
                           "parentTrans={:08X} parentDirty={}",
                           s_skel_calls, results[b].name,
                           results[b].obj_addr, results[b].complete_addr,
                           results[b].dirty, results[b].world_z,
                           results[b].parent_trans, results[b].parent_dirty);
                  } else {
                    XELOGI("DC3:BONE [nuiFrame {}] {} NOT FOUND",
                           s_skel_calls, results[b].name);
                  }
                }

                // Run probe up to 30 times to catch animation state.
                static int s_bone_probe_count = 0;
                bool any_found = false;
                for (int b = 0; b < kNumBones; b++) {
                  if (results[b].found) any_found = true;
                }
                if (any_found) {
                  s_bone_probe_count++;
                  if (s_bone_probe_count >= 30) {
                    s_bone_probe_done = true;
                    XELOGI("DC3:BONE Probe complete — {} samples captured",
                           s_bone_probe_count);
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  // Shared state between beat monitor and beat drive blocks.
  static bool s_game_force_unpaused = false;
  static uint32_t s_last_screen = 0;
  static int s_screen_stable_count = 0;

  // --- Early audio bypass: vtable patch + force-unpause ---
  // This runs every NUI frame starting from frame 600 (~10s) to unblock the
  // Game::IsLoaded() → HamAudio::IsReady() gate as early as possible.
  // Without this, the GamePanel never finishes loading and Game::Poll is never
  // called, freezing ALL game logic (not just timers).
  {
    static bool s_early_vtable_patched = false;
    static bool s_early_force_unpaused = false;

    if (!s_early_vtable_patched && s_skel_calls >= 600) {
      // Read HamAudio.mSongStream to check if it exists yet
      constexpr uint32_t kTheMaster_ev = 0x82F61D54;
      auto* master_ptr_ev = memory->TranslateVirtual<uint8_t*>(kTheMaster_ev);
      if (master_ptr_ev) {
        uint32_t master_addr_ev = xe::load_and_swap<uint32_t>(master_ptr_ev);
        if (master_addr_ev && master_addr_ev < 0xF0000000) {
          auto* master_ev = memory->TranslateVirtual<uint8_t*>(master_addr_ev);
          if (master_ev) {
            uint32_t audio_addr_ev = xe::load_and_swap<uint32_t>(master_ev + 0x34);
            if (audio_addr_ev && audio_addr_ev < 0xF0000000) {
              auto* audio_ev = memory->TranslateVirtual<uint8_t*>(audio_addr_ev);
              if (audio_ev) {
                // Unblock FileLoader if it's stuck but has a buffer
                uint32_t loader_addr = xe::load_and_swap<uint32_t>(audio_ev + 0x30);
                if (loader_addr && loader_addr < 0xF0000000) {
                  auto* loader = memory->TranslateVirtual<uint8_t*>(loader_addr);
                  if (loader) {
                    uint32_t buffer = xe::load_and_swap<uint32_t>(loader + 0x24);
                    uint32_t state = xe::load_and_swap<uint32_t>(loader + 0x4C);
                    if (buffer && buffer < 0xF0000000 && state != 0x823E3B70) {
                      xe::store_and_swap<uint32_t>(loader + 0x4C, 0x823E3B70);
                      XELOGI("DC3: Force-unblocked FileLoader {:08X} (buffer={:08X}, state={:08X}, nuiFrame={})",
                             loader_addr, buffer, state, s_skel_calls);
                    }
                  }
                }

                uint32_t stream_addr = xe::load_and_swap<uint32_t>(audio_ev + 0x40);
                if (stream_addr && stream_addr < 0xF0000000) {
                  // Stream exists: FinishLoad already created it.
                  // Patch HamAudio::IsReady function body directly
                  // to return true.  The vtable patch on Stream::IsReady
                  // is unreliable (JIT may not re-read patched vtable),
                  // so we patch the caller instead.
                  //
                  // HamAudio::IsReady (0x8252B9E0):
                  //   if (!mSongStream && !mRawBuffer) { FinishLoad(); ... }
                  //   mReady = mSongStream && mSongStream->IsReady();
                  //   return mReady;
                  //
                  // Since mSongStream is now non-null, FinishLoad won't
                  // be called anymore.  We replace the first 2 instructions
                  // with: li r3, 1; blr  (always return true).
                  constexpr uint32_t kHamAudioIsReady = 0x8252B9E0;
                  auto* fn = memory->TranslateVirtual<uint8_t*>(kHamAudioIsReady);
                  if (fn) {
                    auto* heap = memory->LookupHeap(kHamAudioIsReady);
                    if (heap) {
                      heap->Protect(kHamAudioIsReady, 8,
                                    kMemoryProtectRead | kMemoryProtectWrite);
                      xe::store_and_swap<uint32_t>(fn + 0, 0x38600001);  // li r3, 1
                      xe::store_and_swap<uint32_t>(fn + 4, 0x4E800020);  // blr
                      XELOGI("DC3: Patched HamAudio::IsReady at {:08X} to "
                             "return true (stream {:08X} exists, nuiFrame={})",
                             kHamAudioIsReady, stream_addr, s_skel_calls);
                      s_early_vtable_patched = true;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

    // Monitor Game state: detect when HandleWait has naturally completed
    // (mWaitState=0 and mPaused=false), OR apply safety-net intervention
    // if the wait state machine is stuck after a long time.
    // OLD CODE forced mWaitState=0 at frame 900 which bypassed HandleWait
    // case 5 (SetupAnims + SetPollEnabled), breaking dance animation.
    // Now we let HandleWait run naturally since the vtable patch (IsReady)
    // unblocks the audio gate.
    if (!s_early_force_unpaused && s_early_vtable_patched && s_skel_calls >= 900) {
      constexpr uint32_t kTheGamePanel_ef = 0x83117410;
      constexpr uint32_t kTheHamDirector_ef = 0x82F603A0;
      auto* gp_ef = memory->TranslateVirtual<uint8_t*>(kTheGamePanel_ef);
      if (gp_ef) {
        uint32_t gp_addr_ef = xe::load_and_swap<uint32_t>(gp_ef);
        if (gp_addr_ef && gp_addr_ef < 0xF0000000) {
          auto* gp_obj_ef = memory->TranslateVirtual<uint8_t*>(gp_addr_ef);
          if (gp_obj_ef) {
            uint32_t game_addr_ef = xe::load_and_swap<uint32_t>(gp_obj_ef + 0x38);
            if (game_addr_ef && game_addr_ef < 0xF0000000) {
              auto* game_ef = memory->TranslateVirtual<uint8_t*>(game_addr_ef);
              if (game_ef) {
                int32_t wait_ef = xe::load_and_swap<int32_t>(game_ef + 0xA4);
                uint8_t paused_ef = game_ef[0x5E];

                // Clear mGameStartHold so StartGame() can fire in GamePanel::Poll
                auto* hd_ptr = memory->TranslateVirtual<uint8_t*>(kTheHamDirector_ef);
                if (hd_ptr) {
                  uint32_t hd_addr = xe::load_and_swap<uint32_t>(hd_ptr);
                  if (hd_addr && hd_addr < 0xF0000000) {
                    auto* hd = memory->TranslateVirtual<uint8_t*>(hd_addr);
                    if (hd) {
                      hd[0x33d] = 0;  // mGameStartHold = false
                    }
                  }
                }

                // Diagnostic: dump HandleWait instructions around the IsReady check
                if (s_skel_calls == 1200) {
                  constexpr uint32_t kHandleWait_diag = 0x82867288;
                  auto* hw = memory->TranslateVirtual<uint8_t*>(kHandleWait_diag);
                  if (hw) {
                    // Dump first 0x100 bytes of HandleWait looking for
                    // the audio->IsReady() call and subsequent branch
                    for (uint32_t off = 0; off < 0x100; off += 4) {
                      uint32_t instr = xe::load_and_swap<uint32_t>(hw + off);
                      // Log bctrl, bl (opcode 18), beq/bne (opcode 16 bc)
                      uint32_t opcode = instr >> 26;
                      if (instr == 0x4E800421 || opcode == 18 || opcode == 16) {
                        XELOGI("DC3: DIAG HandleWait+0x{:03X}: {:08X} "
                               "(opcode={})",
                               off, instr, opcode);
                      }
                    }
                  }
                }

                // Force mHasIntro=false every frame while in wait state 3.
                // HandleWait state 3's intro check
                // (mHasIntro && Seconds(kRealTime) < 0) blocks because
                // the guest overwrites the host's beat timeline with a
                // negative intro countdown value.  Clearing mHasIntro
                // skips this check.  Must be done every frame because
                // SetIntroRealTime re-sets mHasIntro on each call.
                if (wait_ef == 3) {
                  game_ef[0x62] = 0;  // mHasIntro = false (continuous)
                  static bool s_intro_logged = false;
                  if (!s_intro_logged) {
                    XELOGI("DC3: Forcing mHasIntro=false continuously "
                           "(unblocking HandleWait state 3) at {:08X} "
                           "(nuiFrame={})", game_addr_ef, s_skel_calls);
                    s_intro_logged = true;
                  }
                }

                // Detect natural completion: HandleWait reached state 0
                if (wait_ef == 0 && paused_ef == 0) {
                  XELOGI("DC3: HandleWait completed naturally: "
                         "mWaitState=0 mPaused=false at {:08X} (nuiFrame={})",
                         game_addr_ef, s_skel_calls);
                  s_early_force_unpaused = true;
                  s_game_force_unpaused = true;
                }
                // Safety net: if HandleWait is stuck after 1500 frames,
                // force-complete it AND fix mPollEnabled on HamDirector
                else if (s_skel_calls >= 1500) {
                  game_ef[0x5E] = 0;  // mPaused = false
                  xe::store_and_swap<int32_t>(game_ef + 0xA4, 0);  // mWaitState = 0
                  // Force mPollEnabled=true on HamDirector (offset 0x2ac)
                  if (hd_ptr) {
                    uint32_t hd_addr = xe::load_and_swap<uint32_t>(hd_ptr);
                    if (hd_addr && hd_addr < 0xF0000000) {
                      auto* hd = memory->TranslateVirtual<uint8_t*>(hd_addr);
                      if (hd) {
                        hd[0x2ac] = 1;  // mPollEnabled = true
                      }
                    }
                  }
                  XELOGI("DC3: Safety-net intervention: forced Game "
                         "mPaused=false mWaitState=0 mPollEnabled=true "
                         "at {:08X} (nuiFrame={}, was wait={} paused={})",
                         game_addr_ef, s_skel_calls, wait_ef, paused_ef);
                  s_early_force_unpaused = true;
                  s_game_force_unpaused = true;
                }
              }
            }
          }
        }
      }
    }
  }

  // --- Beat advancement: monitor + host-driven fallback ---
  // The PPC wall-clock timer (__mftb()) doesn't advance under Xenia's
  // guest thread execution model (timerCycles stays constant across frames).
  // This means Game::Poll's CurrentMs(mRealTime=true) returns the same
  // value forever, and the beat never advances naturally.
  //
  // Strategy:
  //  1. Let HandleWait complete naturally (vtable patch makes IsReady=true,
  //     which unblocks the audio gate and lets case 5 fire SetupAnims +
  //     SetPollEnabled). Safety-net force at frame 3000 if truly stuck.
  //  2. Write beat/seconds directly to TheTaskMgr each NUI frame
  //     (bypasses broken __mftb timer)
  {
    constexpr uint32_t kTheTaskMgr = 0x82F64A58;
    constexpr uint32_t kTheGamePanel = 0x83117410;
    if (s_skel_calls >= 1800 && (s_skel_calls % 600) == 0) {
      auto* tm = memory->TranslateVirtual<uint8_t*>(kTheTaskMgr);
      if (tm) {
        uint32_t tl_addr = xe::load_and_swap<uint32_t>(tm + 0x2C);
        if (tl_addr && tl_addr < 0xF0000000) {
          auto* tl = memory->TranslateVirtual<uint8_t*>(tl_addr);
          if (tl) {
            float sec = xe::load_and_swap<float>(tl + 0x10);
            float beat = xe::load_and_swap<float>(tl + 0x2C);
            // Also read Game state
            uint8_t paused = 0xFF, realtime = 0xFF;
            int32_t waitState = -1;
            auto* gp_ptr = memory->TranslateVirtual<uint8_t*>(kTheGamePanel);
            if (gp_ptr) {
              uint32_t gp_addr = xe::load_and_swap<uint32_t>(gp_ptr);
              if (gp_addr && gp_addr < 0xF0000000) {
                auto* gp = memory->TranslateVirtual<uint8_t*>(gp_addr);
                if (gp) {
                  uint32_t game_addr = xe::load_and_swap<uint32_t>(gp + 0x38);
                  if (game_addr && game_addr < 0xF0000000) {
                    auto* game = memory->TranslateVirtual<uint8_t*>(game_addr);
                    if (game) {
                      paused = game[0x5E];
                      realtime = game[0x60];
                      waitState = xe::load_and_swap<int32_t>(game + 0xA4);
                    }
                  }
                }
              }
            }
            // Also read HamAudio state from TheMaster
            constexpr uint32_t kTheMaster = 0x82F61D54;
            uint32_t audio_fileLoader = 0, audio_rawBuffer = 0;
            uint32_t audio_songStream = 0;
            uint8_t audio_ready = 0xFF;
            auto* master_ptr = memory->TranslateVirtual<uint8_t*>(kTheMaster);
            if (master_ptr) {
              uint32_t master_addr = xe::load_and_swap<uint32_t>(master_ptr);
              if (master_addr && master_addr < 0xF0000000) {
                auto* master = memory->TranslateVirtual<uint8_t*>(master_addr);
                if (master) {
                  uint32_t audio_addr = xe::load_and_swap<uint32_t>(master + 0x34);
                  if (audio_addr && audio_addr < 0xF0000000) {
                    auto* audio = memory->TranslateVirtual<uint8_t*>(audio_addr);
                    if (audio) {
                      audio_fileLoader = xe::load_and_swap<uint32_t>(audio + 0x30);
                      audio_rawBuffer = xe::load_and_swap<uint32_t>(audio + 0x34);
                      audio_songStream = xe::load_and_swap<uint32_t>(audio + 0x40);
                      audio_ready = audio[0x4c];
                    }
                  }
                }
              }
            }
            // Read LiveInput timer state
            float timeOffset = 0;
            uint64_t timerCycles = 0;
            uint32_t timerStart = 0;
            int32_t timerRunning = -1;
            uint8_t gameUnkf8 = 0xFF;
            // Read TaskMgr.mTime timer state
            uint64_t tmTimerCycles = 0;
            uint32_t tmTimerStart = 0;
            int32_t tmTimerRunning = -1;
            // Read from TaskMgr at +0x50 (mTime Timer)
            {
              auto* tm_diag = memory->TranslateVirtual<uint8_t*>(kTheTaskMgr);
              if (tm_diag) {
                tmTimerStart = xe::load_and_swap<uint32_t>(tm_diag + 0x50);
                tmTimerCycles = xe::load_and_swap<uint64_t>(tm_diag + 0x50 + 0x08);
                tmTimerRunning = xe::load_and_swap<int32_t>(tm_diag + 0x50 + 0x24);
              }
            }
            if (paused != 0xFF) {
              // We found the game object — read more
              auto* gp_ptr3 = memory->TranslateVirtual<uint8_t*>(kTheGamePanel);
              if (gp_ptr3) {
                uint32_t gp_addr3 = xe::load_and_swap<uint32_t>(gp_ptr3);
                if (gp_addr3 && gp_addr3 < 0xF0000000) {
                  auto* gp3 = memory->TranslateVirtual<uint8_t*>(gp_addr3);
                  if (gp3) {
                    gameUnkf8 = gp3[0xF8];
                    uint32_t game_addr3 = xe::load_and_swap<uint32_t>(gp3 + 0x38);
                    if (game_addr3 && game_addr3 < 0xF0000000) {
                      auto* game3 = memory->TranslateVirtual<uint8_t*>(game_addr3);
                      if (game3) {
                        uint32_t input_addr = xe::load_and_swap<uint32_t>(game3 + 0x54);
                        if (input_addr && input_addr < 0xF0000000) {
                          auto* input = memory->TranslateVirtual<uint8_t*>(input_addr);
                          if (input) {
                            timeOffset = xe::load_and_swap<float>(input + 0x08);
                            timerStart = xe::load_and_swap<uint32_t>(input + 0x10);
                            timerCycles = xe::load_and_swap<uint64_t>(input + 0x10 + 0x08);
                            timerRunning = xe::load_and_swap<int32_t>(input + 0x10 + 0x24);
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
            // Also read current guest clock for reference
            uint64_t guestTick = Clock::QueryGuestTickCount();
            // Read GamePanel state (mState at +0x80, mPollLoadState at +0x104)
            int32_t gpState = -1;
            int32_t gpPollLoadState = -1;
            {
              auto* gp_st = memory->TranslateVirtual<uint8_t*>(kTheGamePanel);
              if (gp_st) {
                uint32_t gp_st_addr = xe::load_and_swap<uint32_t>(gp_st);
                if (gp_st_addr && gp_st_addr < 0xF0000000) {
                  auto* gp_obj = memory->TranslateVirtual<uint8_t*>(gp_st_addr);
                  if (gp_obj) {
                    gpState = xe::load_and_swap<int32_t>(gp_obj + 0x80);
                    gpPollLoadState = xe::load_and_swap<int32_t>(gp_obj + 0x104);

                    // Safety net: Force mPollLoadState to 4 if stuck at 3
                    // for too long. With vtable patch IsReady=true, this
                    // should advance naturally; 2400 is a generous timeout.
                    if (gpPollLoadState == 3 && s_skel_calls >= 2400) {
                      xe::store_and_swap<int32_t>(gp_obj + 0x104, 4);
                      static bool s_force_logged = false;
                      if (!s_force_logged) {
                        XELOGI("DC3: Safety-net forced GamePanel.mPollLoadState "
                               "3→4 (audio-ready gate timeout)");
                        s_force_logged = true;
                      }
                    }

                    // Force kGameInIntro (1) → kGamePlaying (2) transition.
                    // The preferred path is to nudge the original gameplay
                    // startup functions directly instead of blindly scribbling
                    // GamePanel::mState, which leaves animation init missing.
                    //
                    // The PPC code at GamePanel::Poll checks:
                    //   mState == kGameInIntro
                    //   TheTaskMgr.Seconds(kRealTime) > -0.025f
                    //   !TheHamDirector->IsGameStartHold()
                    // Under Xenia, the guest thread often doesn't hit that
                    // gate at the right moment relative to host-driven beat
                    // writes.  Once the game is on game_screen and time is
                    // advancing, call the missing guest-side startup hooks:
                    //   HamDirector::SetupAnims
                    //   SongSequence::OnSongLoaded
                    //   GamePanel::StartGame
                    // Only fall back to writing gpState directly if the guest
                    // path still doesn't advance after a long timeout.
                    if (gpState == 1 && sec > 0.0f && s_skel_calls >= 2400) {
                      // Set Game.mHasIntro = false (but leave mWaitState alone
                      // so any remaining HandleWait logic can still complete).
                      uint32_t game_addr_fg = xe::load_and_swap<uint32_t>(
                          gp_obj + 0x38);
                      if (game_addr_fg && game_addr_fg < 0xF0000000) {
                        auto* game_fg = memory->TranslateVirtual<uint8_t*>(
                            game_addr_fg);
                        if (game_fg) {
                          game_fg[0x62] = 0;  // mHasIntro = false
                        }
                      }

                      // Clear HamDirector.mGameStartHold so StartGame's gate
                      // conditions match the original polling path.
                      constexpr uint32_t kTheHamDirector_fg = 0x82F603A0;
                      uint32_t hd_fg_addr = 0;
                      auto* hd_fg_ptr = memory->TranslateVirtual<uint8_t*>(
                          kTheHamDirector_fg);
                      if (hd_fg_ptr) {
                        hd_fg_addr = xe::load_and_swap<uint32_t>(hd_fg_ptr);
                        if (hd_fg_addr && hd_fg_addr < 0xF0000000) {
                          auto* hd_fg = memory->TranslateVirtual<uint8_t*>(
                              hd_fg_addr);
                          if (hd_fg) {
                            hd_fg[0x33d] = 0;  // mGameStartHold = false
                          }
                        }
                      }

                      static bool s_guest_startup_attempted = false;
                      static bool s_guest_startup_logged = false;
                      static uint32_t s_song_anim_drive_ptr = 0;
                      if (!s_guest_startup_attempted) {
                        constexpr uint32_t kHamDirectorSetupAnims = 0x82474868;
                        constexpr uint32_t kHamDirectorSongAnim = 0x82475578;
                        constexpr uint32_t kHamDirectorSongAnimByDifficulty =
                            0x82473E58;
                        constexpr uint32_t kSongSequenceOnSongLoaded =
                            0x8288BDF8;
                        constexpr uint32_t kGamePanelStartGame = 0x8287AE28;
                        constexpr uint32_t kTheSongSequence = 0x8311787C;
                        constexpr uint32_t kRndPropAnimStartAnim = 0x8267CC10;

                        auto* processor = kernel_state->processor();
                        auto* thread_state = ppc_context->thread_state;
                        if (processor && thread_state && gp_st_addr &&
                            hd_fg_addr && hd_fg_addr < 0xF0000000) {
                          auto exec_member = [&](uint32_t fn_addr, uint32_t this_ptr,
                                                 std::initializer_list<uint64_t> extra) {
                            uint64_t args[4] = {this_ptr, 0, 0, 0};
                            size_t arg_count = 1;
                            for (uint64_t value : extra) {
                              if (arg_count >= (sizeof(args) / sizeof(args[0]))) {
                                break;
                              }
                              args[arg_count++] = value;
                            }
                            return processor->Execute(thread_state, fn_addr, args,
                                                      arg_count);
                          };

                          uint64_t song_anim_before =
                              exec_member(kHamDirectorSongAnim, hd_fg_addr, {0});
                          uint64_t setup_result =
                              exec_member(kHamDirectorSetupAnims, hd_fg_addr, {});
                          uint64_t song_anim_after_setup =
                              exec_member(kHamDirectorSongAnim, hd_fg_addr, {0});
                          uint64_t song_loaded_args[1] = {kTheSongSequence};
                          uint64_t song_loaded_result = processor->Execute(
                              thread_state, kSongSequenceOnSongLoaded,
                              song_loaded_args, 1);
                          uint64_t song_anim_after_songload =
                              exec_member(kHamDirectorSongAnim, hd_fg_addr, {0});
                          uint64_t expert_anim = exec_member(
                              kHamDirectorSongAnimByDifficulty, hd_fg_addr, {2});
                          uint64_t start_anim_result = 0;
                          if (expert_anim && expert_anim < 0xF0000000) {
                            start_anim_result = exec_member(
                                kRndPropAnimStartAnim,
                                static_cast<uint32_t>(expert_anim), {});
                            s_song_anim_drive_ptr =
                                static_cast<uint32_t>(expert_anim);
                          }
                          uint64_t start_game_result =
                              exec_member(kGamePanelStartGame, gp_st_addr, {});

                          gpState = xe::load_and_swap<int32_t>(gp_obj + 0x80);
                          if (!s_guest_startup_logged) {
                            XELOGI("DC3: Guest gameplay init: "
                                   "SongAnim(before)={:08X} SetupAnims={:08X} "
                                   "SongAnim(after_setup)={:08X} "
                                   "OnSongLoaded={:08X} "
                                   "SongAnim(after_songload)={:08X} "
                                   "ExpertAnim={:08X} StartAnim={:08X} "
                                   "StartGame={:08X} gpState={} "
                                   "(nuiFrame={} sec={:.2f} beat={:.2f})",
                                   static_cast<uint32_t>(song_anim_before),
                                   static_cast<uint32_t>(setup_result),
                                   static_cast<uint32_t>(song_anim_after_setup),
                                   static_cast<uint32_t>(song_loaded_result),
                                   static_cast<uint32_t>(song_anim_after_songload),
                                   static_cast<uint32_t>(expert_anim),
                                   static_cast<uint32_t>(start_anim_result),
                                   static_cast<uint32_t>(start_game_result),
                                   gpState, s_skel_calls, sec, beat);
                            s_guest_startup_logged = true;
                          }
                          s_guest_startup_attempted = true;
                          ppc_context->r[3] = 0;  // preserve S_OK for NUI call
                        }
                      }

                      // Last-resort fallback if the guest startup functions
                      // still haven't advanced gameplay after ~10 more seconds.
                      if (gpState == 1 && s_skel_calls >= 3000) {
                        xe::store_and_swap<int32_t>(gp_obj + 0x80, 2);
                        gpState = 2;
                        static bool s_startgame_force_logged = false;
                        if (!s_startgame_force_logged) {
                          XELOGI("DC3: Forced GamePanel kGameInIntro→"
                                 "kGamePlaying fallback at nuiFrame={} "
                                 "sec={:.2f} beat={:.2f}",
                                 s_skel_calls, sec, beat);
                          s_startgame_force_logged = true;
                        }
                      }
                    }
                  }
                }
              }
            }

            // Read HamDirector diagnostics (mGameStartHold + mPollEnabled)
            uint8_t gameStartHold = 0xFF;
            uint8_t pollEnabled = 0xFF;
            {
              constexpr uint32_t kTheHamDirector_diag = 0x82F603A0;
              auto* hd_diag_ptr = memory->TranslateVirtual<uint8_t*>(
                  kTheHamDirector_diag);
              if (hd_diag_ptr) {
                uint32_t hd_diag_addr = xe::load_and_swap<uint32_t>(
                    hd_diag_ptr);
                if (hd_diag_addr && hd_diag_addr < 0xF0000000) {
                  auto* hd_diag = memory->TranslateVirtual<uint8_t*>(
                      hd_diag_addr);
                  if (hd_diag) {
                    gameStartHold = hd_diag[0x33d];
                    pollEnabled = hd_diag[0x2ac];  // mPollEnabled
                  }
                }
              }
            }
            XELOGI("DC3: Beat monitor: sec={:.2f} beat={:.2f} "
                   "paused={} realtime={} waitState={} "
                   "audioFL={:08X} audioBuf={:08X} audioStream={:08X} "
                   "audioReady={} unkf8={} timeOffset={:.1f} "
                   "timerStart={} timerCycles={} timerRunning={} "
                   "tmTimerStart={} tmTimerCycles={} tmTimerRunning={} "
                   "guestTick={} gpState={} gpPollLoad={} "
                   "gameStartHold={} pollEnabled={} nuiFrame={}",
                   sec, beat, paused, realtime, waitState,
                   audio_fileLoader, audio_rawBuffer, audio_songStream,
                   audio_ready, gameUnkf8, timeOffset,
                   timerStart, timerCycles, timerRunning,
                   tmTimerStart, tmTimerCycles, tmTimerRunning,
                   guestTick, gpState, gpPollLoadState,
                   gameStartHold, pollEnabled, s_skel_calls);

            // NOTE: The vtable patch on Stream::IsReady was unreliable
            // (JIT indirect call may not pick up patched vtable entries).
            // HamAudio::IsReady is now patched directly at the function
            // level in the "Early audio bypass" block above.  This
            // replaces the first 2 instructions with li r3,1; blr once
            // mSongStream is detected as non-null.

            // Monitor natural HandleWait completion: detect when
            // mWaitState reaches 0 and mPaused=false (set by PostWaitStart).
            // OLD CODE forced mWaitState=0 here which bypassed HandleWait
            // case 5 (SetupAnims + SetPollEnabled), breaking dance animation.
            // With the vtable patch making IsReady=true, HandleWait should
            // complete naturally.  The safety-net at frame 3000 (above)
            // handles the case where it's truly stuck.
            if (!s_game_force_unpaused && paused != 0xFF) {
              if (waitState == 0 && paused == 0) {
                XELOGI("DC3: Beat monitor detected natural completion: "
                       "mWaitState=0 mPaused=false (nuiFrame={})",
                       s_skel_calls);
                s_game_force_unpaused = true;
              }
            }
          }
        }
      }
    }
  }

  // --- Host-driven beat advancement ---
  // Since __mftb() doesn't advance under Xenia's guest execution, drive
  // the beat directly by writing to TheTaskMgr's timeline memory.
  // This fires every NUI frame after the game has been force-unpaused.
  {
    static bool s_beat_drive_active = false;
    static uint32_t s_beat_start_frame = 0;
    constexpr uint32_t kTheTaskMgr_bd = 0x82F64A58;
    constexpr float kBpm_bd = 120.0f;
    constexpr float kMsPerFrame_bd = 33.333f;

    // Activate host-driven beat once we detect game_screen is loaded
    // (gpPollLoadState=4) and the game has entered kGameInIntro (gpState>=1).
    // This is needed BEFORE HandleWait completes, because HandleWait state 3
    // has an intro countdown check (Seconds(kRealTime) < 0.0) that blocks
    // until the timeline advances past zero.  Without beat drive, the
    // timeline stays at its initial negative value forever.
    if (!s_beat_drive_active && s_skel_calls >= 1200) {
      constexpr uint32_t kTheGamePanel_bd = 0x83117410;
      auto* gp_bd = memory->TranslateVirtual<uint8_t*>(kTheGamePanel_bd);
      if (gp_bd) {
        uint32_t gp_addr_bd = xe::load_and_swap<uint32_t>(gp_bd);
        if (gp_addr_bd && gp_addr_bd < 0xF0000000) {
          auto* gp_obj_bd = memory->TranslateVirtual<uint8_t*>(gp_addr_bd);
          if (gp_obj_bd) {
            int32_t gpState_bd = xe::load_and_swap<int32_t>(gp_obj_bd + 0x80);
            int32_t gpPollLoad_bd = xe::load_and_swap<int32_t>(gp_obj_bd + 0x104);
            if (gpPollLoad_bd == 4 && gpState_bd >= 1) {
              s_beat_drive_active = true;
              s_beat_start_frame = s_skel_calls;
              auto* tm_bd2 = memory->TranslateVirtual<uint8_t*>(kTheTaskMgr_bd);
              if (tm_bd2) tm_bd2[0x48] = 0;  // mAutoSecondsBeats = false
              XELOGI("DC3: Host-driven beat activated at nuiFrame={} "
                     "(gpState={} gpPollLoad={})",
                     s_skel_calls, gpState_bd, gpPollLoad_bd);
            }
          }
        }
      }
    }

    if (s_beat_drive_active) {
      auto* tm_bd = memory->TranslateVirtual<uint8_t*>(kTheTaskMgr_bd);
      if (tm_bd) {
        uint32_t tl_addr_bd = xe::load_and_swap<uint32_t>(tm_bd + 0x2C);
        if (tl_addr_bd && tl_addr_bd < 0xF0000000) {
          auto* tl_bd = memory->TranslateVirtual<uint8_t*>(tl_addr_bd);
          if (tl_bd) {
            uint32_t frames = s_skel_calls - s_beat_start_frame;
            float songMs = frames * kMsPerFrame_bd;
            float seconds = songMs / 1000.0f;
            float beat = songMs * (kBpm_bd / 60000.0f);

            // Write kTaskSeconds timeline
            float prev_sec_bd = xe::load_and_swap<float>(tl_bd + 0x10);
            xe::store_and_swap<float>(tl_bd + 0x14, prev_sec_bd);
            xe::store_and_swap<float>(tl_bd + 0x10, seconds);

            // Write kTaskBeats timeline
            float prev_beat_bd = xe::load_and_swap<float>(tl_bd + 0x2C);
            xe::store_and_swap<float>(tl_bd + 0x30, prev_beat_bd);
            xe::store_and_swap<float>(tl_bd + 0x2C, beat);

            // Also write kTaskUISeconds (index 2) for task scheduling
            float prev_ui_bd = xe::load_and_swap<float>(tl_bd + 0x1C*2 + 0x10);
            xe::store_and_swap<float>(tl_bd + 0x1C*2 + 0x14, prev_ui_bd);
            xe::store_and_swap<float>(tl_bd + 0x1C*2 + 0x10, seconds);

            tm_bd[0x48] = 0;  // keep mAutoSecondsBeats off

            // Gameplay bootstrap fallback: if the original world-side
            // select_camera path never advances the song anim under Xenia,
            // drive the preauthored expert song.anim directly from the host
            // beat timeline. This mirrors the native fallback used when the
            // routine-builder choreography path is stalled.
            {
              static uint32_t s_song_anim_drive_ptr = 0;
              static bool s_song_anim_drive_logged = false;
              static bool s_song_gate_scrub_logged = false;
              static bool s_song_anim_alias_logged = false;
              if (!s_song_anim_drive_ptr && s_skel_calls >= 2400) {
                constexpr uint32_t kTheHamDirector_anim = 0x82F603A0;
                constexpr uint32_t kHamDirectorSongAnimByDifficulty =
                    0x82473E58;
                auto* hd_ptr_anim =
                    memory->TranslateVirtual<uint8_t*>(kTheHamDirector_anim);
                auto* processor = kernel_state->processor();
                auto* thread_state = ppc_context->thread_state;
                if (hd_ptr_anim && processor && thread_state) {
                  uint32_t hd_addr_anim =
                      xe::load_and_swap<uint32_t>(hd_ptr_anim);
                  if (hd_addr_anim && hd_addr_anim < 0xF0000000) {
                    uint64_t args[2] = {hd_addr_anim, 2};
                    uint64_t expert_anim = processor->Execute(
                        thread_state, kHamDirectorSongAnimByDifficulty, args, 2);
                    if (expert_anim && expert_anim < 0xF0000000) {
                      s_song_anim_drive_ptr = static_cast<uint32_t>(expert_anim);
                    }
                    ppc_context->r[3] = 0;  // preserve S_OK for NUI call
                  }
                }
              }

              if (s_song_anim_drive_ptr) {
                constexpr uint32_t kRndPropAnimSetFrame = 0x8267CA50;
                auto* processor = kernel_state->processor();
                auto* thread_state = ppc_context->thread_state;
                if (processor && thread_state) {
                  uint64_t saved_r3 = ppc_context->r[3];
                  double saved_f1 = ppc_context->f[1];
                  double saved_f2 = ppc_context->f[2];
                  ppc_context->r[3] = s_song_anim_drive_ptr;
                  ppc_context->f[1] = static_cast<double>(seconds * 30.0f);
                  ppc_context->f[2] = 1.0;
                  processor->Execute(thread_state, kRndPropAnimSetFrame);
                  ppc_context->r[3] = 0;
                  ppc_context->f[1] = saved_f1;
                  ppc_context->f[2] = saved_f2;
                  if (!s_song_anim_drive_logged) {
                    XELOGI("DC3: Host song.anim SetFrame drive active: "
                           "anim={:08X} at nuiFrame={}",
                           s_song_anim_drive_ptr, s_skel_calls);
                    s_song_anim_drive_logged = true;
                  }
                  (void)saved_r3;
                }
              }

              // Original HamDirector::SongAnim(player) prefers the per-player
              // routine-builder anim when merge_moves=1. On native, there is
              // an extra fallback to the expert/master anim if the routine
              // builder never populated clip keys. Mirror that here by
              // aliasing both routine-builder anim slots to the resolved
              // expert anim once gameplay is active.
              if (s_song_anim_drive_ptr && s_skel_calls >= 2400) {
                constexpr uint32_t kTheHamDirector_alias = 0x82F603A0;
                constexpr uint32_t kObjRefConcreteObjectOffset = 0x0C;
                constexpr uint32_t kHamDirectorMasterClipAnimOffset =
                    0x8C + kObjRefConcreteObjectOffset;
                constexpr uint32_t kHamDirectorPlayer1RoutineAnimOffset =
                    0xA0 + kObjRefConcreteObjectOffset;
                constexpr uint32_t kHamDirectorPlayer2RoutineAnimOffset =
                    0xB4 + kObjRefConcreteObjectOffset;
                auto* hd_ptr_alias =
                    memory->TranslateVirtual<uint8_t*>(kTheHamDirector_alias);
                if (hd_ptr_alias) {
                  uint32_t hd_addr_alias =
                      xe::load_and_swap<uint32_t>(hd_ptr_alias);
                  if (hd_addr_alias && hd_addr_alias < 0xF0000000) {
                    auto* hd_obj_alias =
                        memory->TranslateVirtual<uint8_t*>(hd_addr_alias);
                    if (hd_obj_alias) {
                      xe::store_and_swap<uint32_t>(
                          hd_obj_alias + kHamDirectorMasterClipAnimOffset,
                          s_song_anim_drive_ptr);
                      xe::store_and_swap<uint32_t>(
                          hd_obj_alias + kHamDirectorPlayer1RoutineAnimOffset,
                          s_song_anim_drive_ptr);
                      xe::store_and_swap<uint32_t>(
                          hd_obj_alias + kHamDirectorPlayer2RoutineAnimOffset,
                          s_song_anim_drive_ptr);
                      if (!s_song_anim_alias_logged) {
                        XELOGI("DC3: Aliased routine-builder anims to expert "
                               "song.anim={:08X} at nuiFrame={}",
                               s_song_anim_drive_ptr, s_skel_calls);
                        s_song_anim_alias_logged = true;
                      }
                    }
                  }
                }
              }

              // Under Xenia, both characters can enter gameplay with a stale
              // normal-driver idle clip still latched in CharDriver::mFirst.
              // HamCharacter::SongAnimation() treats any non-null driver clip
              // as "not using song animation" and returns -1, which prevents
              // HamDirector::Poll() from ever reaching ClipPlayer::Init().
              // Scrub the stale driver clip once gameplay is active so the
              // original song-driver path can repopulate itself.
              if (s_skel_calls >= 2400) {
                constexpr uint32_t kTheHamDirector_gatefix = 0x82F603A0;
                constexpr uint32_t kHamDirectorGetCharacter = 0x824666B8;
                constexpr uint32_t kCharacterDriverOffset = 0x24C;
                constexpr uint32_t kCharDriverFirstOffset = 0x58;
                constexpr uint32_t kCharDriverDefaultPlayStarvedOffset = 0x98;
                auto* hd_ptr_gatefix =
                    memory->TranslateVirtual<uint8_t*>(kTheHamDirector_gatefix);
                auto* processor = kernel_state->processor();
                auto* thread_state = ppc_context->thread_state;
                if (hd_ptr_gatefix && processor && thread_state) {
                  uint32_t hd_addr_gatefix =
                      xe::load_and_swap<uint32_t>(hd_ptr_gatefix);
                  if (hd_addr_gatefix && hd_addr_gatefix < 0xF0000000) {
                    auto scrub_driver_clip = [&](uint32_t player_index) -> bool {
                      uint64_t args[2] = {hd_addr_gatefix, player_index};
                      uint32_t character_ptr = static_cast<uint32_t>(
                          processor->Execute(thread_state,
                                             kHamDirectorGetCharacter, args, 2));
                      if (!character_ptr || character_ptr >= 0xF0000000) {
                        return false;
                      }
                      auto* character_obj =
                          memory->TranslateVirtual<uint8_t*>(character_ptr);
                      if (!character_obj) {
                        return false;
                      }
                      uint32_t driver_ptr = xe::load_and_swap<uint32_t>(
                          character_obj + kCharacterDriverOffset);
                      if (!driver_ptr || driver_ptr >= 0xF0000000) {
                        return false;
                      }
                      auto* driver_obj =
                          memory->TranslateVirtual<uint8_t*>(driver_ptr);
                      if (!driver_obj) {
                        return false;
                      }
                      uint32_t first_clip = xe::load_and_swap<uint32_t>(
                          driver_obj + kCharDriverFirstOffset);
                      if (!first_clip || first_clip >= 0xF0000000) {
                        return false;
                      }
                      xe::store_and_swap<uint32_t>(
                          driver_obj + kCharDriverFirstOffset, 0);
                      driver_obj[kCharDriverDefaultPlayStarvedOffset] = 0;
                      return true;
                    };

                    bool scrubbed_p0 = scrub_driver_clip(0);
                    bool scrubbed_p1 = scrub_driver_clip(1);
                    if ((scrubbed_p0 || scrubbed_p1) &&
                        !s_song_gate_scrub_logged) {
                      XELOGI("DC3: Scrubbed stale CharDriver clips "
                             "(p0={} p1={}) at nuiFrame={}",
                             scrubbed_p0, scrubbed_p1, s_skel_calls);
                      s_song_gate_scrub_logged = true;
                    }
                    ppc_context->r[3] = 0;  // preserve S_OK for NUI call
                  }
                }
              }
            }

            if ((s_skel_calls % 60) == 0 && s_skel_calls >= 2400) {
              static bool s_song_playanims_probe_logged = false;
              constexpr uint32_t kTheHamDirector_probe = 0x82F603A0;
              constexpr uint32_t kSymbolCtor = 0x827D37C8;
              constexpr uint32_t kClipPlayerInitInt = 0x8251E328;
              constexpr uint32_t kClipPlayerPlayAnims = 0x8251FA20;
              constexpr uint32_t kHamDirectorGetCharacter = 0x824666B8;
              constexpr uint32_t kHamDirectorSetMasterClipAnim = 0x8246BEE8;
              constexpr uint32_t kHamDirectorGetPropKeysByPlayer = 0x82478B80;
              constexpr uint32_t kHamDirectorGetMasterKeys = 0x8246E448;
              constexpr uint32_t kHamDirectorSongAnimation = 0x82467C28;
              constexpr uint32_t kHamDirectorSongAnim = 0x82475578;
              constexpr uint32_t kHamCharacterSongDriver = 0x8248D838;
              constexpr uint32_t kHamCharacterSongAnimation = 0x8248E2F0;
              constexpr uint32_t kCharDriverFirstClip = 0x823654D0;
              constexpr uint32_t kHamDriverFirstClip = 0x824BB4A0;
              constexpr uint32_t kObjRefConcreteObjectOffset = 0x0C;
              constexpr uint32_t kHamDirectorMasterClipAnimOffset =
                  0x8C + kObjRefConcreteObjectOffset;
              constexpr uint32_t kHamDirectorMergerOffset =
                  0xD4 + kObjRefConcreteObjectOffset;
              constexpr uint32_t kHamDirectorClipDirOffset =
                  0x308 + kObjRefConcreteObjectOffset;
              constexpr uint32_t kHamDirectorMoveDirOffset =
                  0x31C + kObjRefConcreteObjectOffset;
              constexpr uint32_t kHamDirectorPrevSongFrameOffset = 0x2E4;
              constexpr uint32_t kHamDirectorBlendDebugOffset = 0x2E8;
              constexpr uint32_t kCharacterDriverOffset = 0x24C;
              constexpr uint32_t kHamCharacterUseCameraSkeletonOffset = 0x350;

              struct SongGateProbe {
                uint32_t character = 0;
                uint32_t driver = 0;
                uint32_t driver_first_clip = 0;
                uint32_t song_driver = 0;
                uint32_t song_driver_first_clip = 0;
                int32_t song_animation = -999;
                uint8_t use_camera_skeleton = 0xFF;
              };

              auto* hd_ptr_probe =
                  memory->TranslateVirtual<uint8_t*>(kTheHamDirector_probe);
              auto* processor = kernel_state->processor();
              auto* thread_state = ppc_context->thread_state;
              if (hd_ptr_probe && processor && thread_state) {
                uint32_t hd_addr_probe =
                    xe::load_and_swap<uint32_t>(hd_ptr_probe);
                if (hd_addr_probe && hd_addr_probe < 0xF0000000) {
                  auto exec_member =
                      [&](uint32_t fn_addr, uint32_t this_ptr,
                          std::initializer_list<uint64_t> extra = {})
                      -> uint64_t {
                    uint64_t args[4] = {this_ptr, 0, 0, 0};
                    size_t arg_count = 1;
                    for (uint64_t value : extra) {
                      if (arg_count >= (sizeof(args) / sizeof(args[0]))) {
                        break;
                      }
                      args[arg_count++] = value;
                    }
                    return processor->Execute(thread_state, fn_addr, args,
                                              arg_count);
                  };

                  auto probe_character = [&](uint32_t character_ptr)
                      -> SongGateProbe {
                    SongGateProbe probe;
                    if (!character_ptr || character_ptr >= 0xF0000000) {
                      return probe;
                    }

                    probe.character = character_ptr;
                    auto* character_obj =
                        memory->TranslateVirtual<uint8_t*>(character_ptr);
                    if (!character_obj) {
                      return probe;
                    }

                    probe.use_camera_skeleton =
                        character_obj[kHamCharacterUseCameraSkeletonOffset];
                    probe.driver =
                        xe::load_and_swap<uint32_t>(character_obj +
                                                    kCharacterDriverOffset);
                    if (probe.driver && probe.driver < 0xF0000000) {
                      probe.driver_first_clip = static_cast<uint32_t>(
                          exec_member(kCharDriverFirstClip, probe.driver));
                    }
                    probe.song_driver = static_cast<uint32_t>(exec_member(
                        kHamCharacterSongDriver, character_ptr));
                    if (probe.song_driver && probe.song_driver < 0xF0000000) {
                      probe.song_driver_first_clip = static_cast<uint32_t>(
                          exec_member(kHamDriverFirstClip, probe.song_driver));
                    }
                    probe.song_animation = static_cast<int32_t>(exec_member(
                        kHamCharacterSongAnimation, character_ptr));
                    return probe;
                  };

                  uint32_t song_anim_0 = static_cast<uint32_t>(
                      exec_member(kHamDirectorSongAnim, hd_addr_probe, {0}));
                  uint32_t song_anim_1 = static_cast<uint32_t>(
                      exec_member(kHamDirectorSongAnim, hd_addr_probe, {1}));
                  int32_t do_song_anim = static_cast<int32_t>(
                      exec_member(kHamDirectorSongAnimation, hd_addr_probe));
                  uint32_t player0_character = static_cast<uint32_t>(
                      exec_member(kHamDirectorGetCharacter, hd_addr_probe, {0}));
                  uint32_t player1_character = static_cast<uint32_t>(
                      exec_member(kHamDirectorGetCharacter, hd_addr_probe, {1}));
                  auto* hd_obj_probe =
                      memory->TranslateVirtual<uint8_t*>(hd_addr_probe);
                  uint32_t merger_ptr = 0;
                  uint32_t clip_dir_before = 0;
                  uint32_t move_dir_before = 0;
                  uint32_t master_clip_before = 0;
                  uint32_t master_clip_after = 0;
                  uint32_t master_clip_keys = 0;
                  uint32_t player0_clip_keys = 0;
                  uint32_t player1_clip_keys = 0;
                  uint32_t clip_player0_init = 0;
                  uint32_t clip_player1_init = 0;
                  uint32_t clip_player0_clip_keys = 0;
                  uint32_t clip_player0_master_keys = 0;
                  uint32_t clip_player0_clip_dir = 0;
                  uint32_t clip_player1_clip_keys = 0;
                  uint32_t clip_player1_master_keys = 0;
                  uint32_t clip_player1_clip_dir = 0;
                  uint32_t play_probe_before_p0 = 0;
                  uint32_t play_probe_after_p0 = 0;
                  uint32_t play_probe_before_p1 = 0;
                  uint32_t play_probe_after_p1 = 0;
                  float play_probe_frame = 0.0f;
                  float play_probe_prev_frame = 0.0f;
                  int32_t play_probe_blend_debug = 0;
                  if (hd_obj_probe) {
                    merger_ptr = xe::load_and_swap<uint32_t>(
                        hd_obj_probe + kHamDirectorMergerOffset);
                    clip_dir_before = xe::load_and_swap<uint32_t>(
                        hd_obj_probe + kHamDirectorClipDirOffset);
                    move_dir_before = xe::load_and_swap<uint32_t>(
                        hd_obj_probe + kHamDirectorMoveDirOffset);
                    master_clip_before = xe::load_and_swap<uint32_t>(
                        hd_obj_probe + kHamDirectorMasterClipAnimOffset);
                    exec_member(kHamDirectorSetMasterClipAnim, hd_addr_probe);
                    master_clip_after = xe::load_and_swap<uint32_t>(
                        hd_obj_probe + kHamDirectorMasterClipAnimOffset);

                    // Probe the actual clip-key lookup path used by
                    // ClipPlayer::Init via a temporary guest Symbol("clip").
                    uint32_t guest_sp = static_cast<uint32_t>(ppc_context->r[1]);
                    uint32_t clip_str_addr = guest_sp - 0x180;
                    uint32_t clip_sym_addr = guest_sp - 0x160;
                    auto* clip_str =
                        memory->TranslateVirtual<char*>(clip_str_addr);
                    auto* clip_sym =
                        memory->TranslateVirtual<uint8_t*>(clip_sym_addr);
                    if (clip_str && clip_sym) {
                      std::memcpy(clip_str, "clip", 5);
                      uint64_t ctor_args[2] = {clip_sym_addr, clip_str_addr};
                      processor->Execute(thread_state, kSymbolCtor, ctor_args, 2);
                      uint32_t clip_sym_value =
                          xe::load_and_swap<uint32_t>(clip_sym);
                      master_clip_keys = static_cast<uint32_t>(exec_member(
                          kHamDirectorGetMasterKeys, hd_addr_probe,
                          {clip_sym_value}));
                      player0_clip_keys = static_cast<uint32_t>(exec_member(
                          kHamDirectorGetPropKeysByPlayer, hd_addr_probe,
                          {0, clip_sym_value}));
                      player1_clip_keys = static_cast<uint32_t>(exec_member(
                          kHamDirectorGetPropKeysByPlayer, hd_addr_probe,
                          {1, clip_sym_value}));

                      // Directly probe ClipPlayer::Init(int) on a temporary
                      // guest-side ClipPlayer instance to see whether its
                      // internal null-gate is what prevents PlayAnims.
                      auto init_clip_player = [&](uint32_t clip_player_addr,
                                                  uint32_t player_index,
                                                  uint32_t& out_init,
                                                  uint32_t& out_clip_keys,
                                                  uint32_t& out_master_keys,
                                                  uint32_t& out_clip_dir) {
                        auto* clip_player =
                            memory->TranslateVirtual<uint8_t*>(clip_player_addr);
                        if (!clip_player) {
                          return;
                        }
                        std::memset(clip_player, 0, 0x54);
                        xe::store_and_swap<float>(clip_player + 0x20, -1.0e30f);
                        xe::store_and_swap<float>(clip_player + 0x24, 1.0e30f);
                        uint64_t init_args[2] = {clip_player_addr, player_index};
                        out_init = static_cast<uint32_t>(processor->Execute(
                            thread_state, kClipPlayerInitInt, init_args, 2));
                        out_clip_keys =
                            xe::load_and_swap<uint32_t>(clip_player + 0x00);
                        out_master_keys =
                            xe::load_and_swap<uint32_t>(clip_player + 0x08);
                        out_clip_dir =
                            xe::load_and_swap<uint32_t>(clip_player + 0x18);
                      };

                      init_clip_player(guest_sp - 0x140, 0, clip_player0_init,
                                       clip_player0_clip_keys,
                                       clip_player0_master_keys,
                                       clip_player0_clip_dir);
                      init_clip_player(guest_sp - 0x1C0, 1, clip_player1_init,
                                       clip_player1_clip_keys,
                                       clip_player1_master_keys,
                                       clip_player1_clip_dir);

                      if (!s_song_playanims_probe_logged &&
                          (clip_player0_init || clip_player1_init)) {
                        play_probe_frame = seconds * 30.0f;
                        play_probe_prev_frame = xe::load_and_swap<float>(
                            hd_obj_probe + kHamDirectorPrevSongFrameOffset);
                        play_probe_blend_debug = xe::load_and_swap<int32_t>(
                            hd_obj_probe + kHamDirectorBlendDebugOffset);

                        auto play_clip_player =
                            [&](uint32_t clip_player_addr,
                                uint32_t character_ptr,
                                uint32_t& out_before,
                                uint32_t& out_after) {
                              if (!character_ptr ||
                                  character_ptr >= 0xF0000000) {
                                return;
                              }
                              uint32_t song_driver =
                                  static_cast<uint32_t>(exec_member(
                                      kHamCharacterSongDriver, character_ptr));
                              if (!song_driver ||
                                  song_driver >= 0xF0000000) {
                                return;
                              }
                              out_before = static_cast<uint32_t>(exec_member(
                                  kHamDriverFirstClip, song_driver));

                              uint64_t saved_r3 = ppc_context->r[3];
                              uint64_t saved_r4 = ppc_context->r[4];
                              uint64_t saved_r5 = ppc_context->r[5];
                              double saved_f1 = ppc_context->f[1];
                              double saved_f2 = ppc_context->f[2];
                              ppc_context->r[3] = clip_player_addr;
                              ppc_context->r[4] = character_ptr;
                              ppc_context->r[5] =
                                  static_cast<uint64_t>(play_probe_blend_debug);
                              ppc_context->f[1] =
                                  static_cast<double>(play_probe_frame);
                              ppc_context->f[2] =
                                  static_cast<double>(play_probe_prev_frame);
                              processor->Execute(thread_state,
                                                 kClipPlayerPlayAnims);
                              ppc_context->r[3] = saved_r3;
                              ppc_context->r[4] = saved_r4;
                              ppc_context->r[5] = saved_r5;
                              ppc_context->f[1] = saved_f1;
                              ppc_context->f[2] = saved_f2;

                              out_after = static_cast<uint32_t>(exec_member(
                                  kHamDriverFirstClip, song_driver));
                            };

                        if (clip_player0_init) {
                          play_clip_player(guest_sp - 0x140, player0_character,
                                           play_probe_before_p0,
                                           play_probe_after_p0);
                        }
                        if (clip_player1_init) {
                          play_clip_player(guest_sp - 0x1C0, player1_character,
                                           play_probe_before_p1,
                                           play_probe_after_p1);
                        }
                        s_song_playanims_probe_logged = true;
                      }
                    }
                  }
                  SongGateProbe p0 = probe_character(player0_character);
                  SongGateProbe p1 = probe_character(player1_character);

                  XELOGI(
                      "DC3: Song gate: doSongAnim={} SongAnim0={:08X} "
                      "SongAnim1={:08X} merger={:08X} clipDir={:08X} "
                      "moveDir={:08X} masterClip(before)={:08X} "
                      "masterClip(after)={:08X} masterKeys={:08X} "
                      "p0Keys={:08X} p1Keys={:08X} "
                      "cp0Init={} cp0ClipKeys={:08X} cp0Master={:08X} "
                      "cp0ClipDir={:08X} cp1Init={} cp1ClipKeys={:08X} "
                      "cp1Master={:08X} cp1ClipDir={:08X} "
                      "playFrame={:.2f} prevFrame={:.2f} blend={} "
                      "playP0={:08X}->{:08X} playP1={:08X}->{:08X} "
                      "p0={:08X} drv={:08X} drvClip={:08X} "
                      "songDrv={:08X} songClip={:08X} songAnim={} camSkel={} "
                      "p1={:08X} drv={:08X} drvClip={:08X} songDrv={:08X} "
                      "songClip={:08X} songAnim={} camSkel={} nuiFrame={}",
                      do_song_anim, song_anim_0, song_anim_1, merger_ptr,
                      clip_dir_before, move_dir_before, master_clip_before,
                      master_clip_after, master_clip_keys, player0_clip_keys,
                      player1_clip_keys, clip_player0_init,
                      clip_player0_clip_keys, clip_player0_master_keys,
                      clip_player0_clip_dir, clip_player1_init,
                      clip_player1_clip_keys, clip_player1_master_keys,
                      clip_player1_clip_dir, play_probe_frame,
                      play_probe_prev_frame, play_probe_blend_debug,
                      play_probe_before_p0, play_probe_after_p0,
                      play_probe_before_p1, play_probe_after_p1, p0.character,
                      p0.driver, p0.driver_first_clip, p0.song_driver,
                      p0.song_driver_first_clip, p0.song_animation,
                      p0.use_camera_skeleton, p1.character, p1.driver,
                      p1.driver_first_clip, p1.song_driver,
                      p1.song_driver_first_clip, p1.song_animation,
                      p1.use_camera_skeleton, s_skel_calls);
                  ppc_context->r[3] = 0;  // preserve S_OK for NUI call
                }
              }
            }

            if ((s_skel_calls % 60) == 0) {
              XELOGI("DC3: Beat drive: sec={:.2f} beat={:.2f} songMs={:.0f} "
                     "frame={} nuiFrame={}",
                     seconds, beat, songMs, frames, s_skel_calls);
            }
          }
        }
      }
    }
  }

  // --- Host-side attract_screen bypass ---
  // MoviePanel::Poll checks !mMovie.Poll() to fire "movie_done" which
  // triggers the attract→autosave_warning→title screen flow.  With Bink
  // video stubbed (noop), mMovie.Poll() never returns "finished" so the
  // game stays stuck on attract_screen forever.
  //
  // Detect attract_screen by reading TheUI->mCurrentScreen->mName and
  // force a GotoScreen("title_screen") transition after a short delay.
  // We skip autosave_warning_screen since save is also stubbed.
  {
    static bool s_attract_bypassed = false;
    static int s_attract_stable = 0;

    if (!s_attract_bypassed && s_skel_calls >= 120) {
      constexpr uint32_t kTheUI_ab = 0x82F1A8E0;
      auto* ui_ptr_ab = memory->TranslateVirtual<uint8_t*>(kTheUI_ab);
      if (ui_ptr_ab) {
        uint32_t ui_addr_ab = xe::load_and_swap<uint32_t>(ui_ptr_ab);
        if (ui_addr_ab && ui_addr_ab < 0xF0000000) {
          auto* ui_obj_ab = memory->TranslateVirtual<uint8_t*>(ui_addr_ab);
          if (ui_obj_ab) {
            uint32_t cur_screen_ab = xe::load_and_swap<uint32_t>(ui_obj_ab + 0x48);
            if (cur_screen_ab && cur_screen_ab < 0xF0000000) {
              auto* scr_ab = memory->TranslateVirtual<uint8_t*>(cur_screen_ab);
              if (scr_ab) {
                uint32_t name_ptr_ab = xe::load_and_swap<uint32_t>(scr_ab + 0x20);
                if (name_ptr_ab && name_ptr_ab < 0xF0000000) {
                  auto* name_ab = memory->TranslateVirtual<char*>(name_ptr_ab);
                  if (name_ab && strncmp(name_ab, "attract_screen", 14) == 0 &&
                      name_ab[14] == '\0') {
                    s_attract_stable++;
                    // Wait 60 NUI frames (~1s) to let the screen fully enter
                    if (s_attract_stable >= 60) {
                      // Find "title_screen" string in .rdata
                      // We'll search near the known screen strings area.
                      // Use GotoScreen by writing mTransitionState + mTransitionScreen.
                      // But GotoScreen needs the UIScreen* pointer, not a string.
                      // Scan for a UIScreen named "title_screen" near cur_screen_ab.
                      uint32_t title_screen_obj = 0;
                      uint32_t scan_base_ab = (cur_screen_ab & 0xFFFF0000);
                      for (uint32_t addr = scan_base_ab;
                           addr < scan_base_ab + 0x40000 && !title_screen_obj;
                           addr += 4) {
                        auto* obj = memory->TranslateVirtual<uint8_t*>(addr);
                        if (!obj) continue;
                        uint32_t np = xe::load_and_swap<uint32_t>(obj + 0x20);
                        if (np < 0x82000000 || np > 0x83200000) continue;
                        auto* ns = memory->TranslateVirtual<char*>(np);
                        if (!ns) continue;
                        if (strncmp(ns, "title_screen", 12) == 0 &&
                            ns[12] == '\0') {
                          title_screen_obj = addr;
                        }
                      }

                      if (title_screen_obj) {
                        // Force GotoScreen transition:
                        // mTransitionState = kTransitionTo (1)
                        // mTransitionScreen = title_screen UIScreen*
                        xe::store_and_swap<uint32_t>(ui_obj_ab + 0x2c, 1);
                        xe::store_and_swap<uint32_t>(ui_obj_ab + 0x4c,
                                                     title_screen_obj);
                        XELOGI("DC3: Attract bypass: forced GotoScreen "
                               "transition attract_screen→title_screen "
                               "(UIScreen {:08X}) at nuiFrame={}",
                               title_screen_obj, s_skel_calls);
                        s_attract_bypassed = true;
                      } else {
                        XELOGI("DC3: Attract bypass: could not find "
                               "title_screen UIScreen object in scan range "
                               "{:08X}-{:08X}",
                               scan_base_ab, scan_base_ab + 0x40000);
                        s_attract_bypassed = true;  // Don't keep scanning
                      }
                    }
                  } else {
                    s_attract_stable = 0;  // Reset if not on attract_screen
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  // --- Host-side controller mode enforcement ---
  // ExitControllerMode is stubbed to blr so it can't turn OFF controller mode,
  // but we also need to ensure it gets turned ON.  EnterControllerMode is called
  // by ShellInput in response to "panel_navigated" messages, but under emulation
  // with fake Kinect data, the first EnterControllerMode call may not fire early
  // enough (or at all if the GestureMgr filtering pipeline rejects the fake
  // skeleton before ShellInput sees navigation events).
  //
  // Fix: every NUI frame after init, force-write mInControllerMode=true
  // in GestureMgr.  Offset 0x426D (from DWARF: GestureMgr+0x426D).
  if (s_skel_calls >= 300) {
    constexpr uint32_t kTheGestureMgr = 0x82F5F7B4;
    auto* gm_ptr = memory->TranslateVirtual<uint8_t*>(kTheGestureMgr);
    if (gm_ptr) {
      uint32_t gm_addr = xe::load_and_swap<uint32_t>(gm_ptr);
      if (gm_addr && gm_addr < 0xF0000000) {
        auto* gm_obj = memory->TranslateVirtual<uint8_t*>(gm_addr);
        if (gm_obj) {
          // mInControllerMode at GestureMgr+0x426D
          if (gm_obj[0x426D] == 0) {
            gm_obj[0x426D] = 1;
            static bool s_cm_logged = false;
            if (!s_cm_logged) {
              XELOGI("DC3: Controller mode enforcement: set "
                     "GestureMgr.mInControllerMode=true at {:08X}+0x426D "
                     "(nuiFrame={})", gm_addr, s_skel_calls);
              s_cm_logged = true;
            }
          }
        }
      }
    }
  }

  // --- Host-side navigator bypass ---
  if (s_skel_calls >= 300) {
    // Read current screen
    constexpr uint32_t kTheUI_host = 0x82F1A8E0;
    auto* ui_ptr_h = memory->TranslateVirtual<uint8_t*>(kTheUI_host);
    uint32_t ui_addr_h = ui_ptr_h ? xe::load_and_swap<uint32_t>(ui_ptr_h) : 0;
    if (ui_addr_h) {
      auto* ui_obj_h = memory->TranslateVirtual<uint8_t*>(ui_addr_h);
      if (ui_obj_h) {
        uint32_t cur_screen_h = xe::load_and_swap<uint32_t>(ui_obj_h + 0x48);
        uint32_t trans_state_h = xe::load_and_swap<uint32_t>(ui_obj_h + 0x2c);

        if (cur_screen_h != s_last_screen) {
          s_last_screen = cur_screen_h;
          s_screen_stable_count = 0;
        } else if (trans_state_h == 0) {
          s_screen_stable_count++;
        }

        // Get current screen name
        std::string cur_name = "unknown";
        auto* scr_obj = memory->TranslateVirtual<uint8_t*>(cur_screen_h);
        if (scr_obj) {
          uint32_t name_ptr = xe::load_and_swap<uint32_t>(scr_obj + 0x20);
          if (name_ptr >= 0x82000000 && name_ptr < 0x83500000) {
            auto* name_str = memory->TranslateVirtual<char*>(name_ptr);
            if (name_str) cur_name = name_str;
          }
        }

        if ((s_skel_calls % 60) == 0) {
          XELOGI("DC3: Nav diag: screen='{}' stable={} trans={} nuiFrame={}", 
                 cur_name, s_screen_stable_count, trans_state_h, s_skel_calls);
        }

        if (s_screen_stable_count >= 120 && trans_state_h == 0) {
          std::string target_name = "";
          if (cur_name == "attract_screen") target_name = "title_screen";
          else if (cur_name == "title_screen") target_name = "main_screen";
          else if (cur_name == "main_screen") target_name = "song_select";
          else if (cur_name == "song_select") target_name = "loading_screen";
          else if (cur_name == "multiuser_screen") target_name = "loading_screen";

          if (!target_name.empty()) {
            uint32_t found_screen = 0;
            // Scan much wider range for UIScreen objects
            for (uint32_t base = 0x40C00000; base < 0x41000000 && !found_screen; base += 0x10000) {
              for (uint32_t addr = base; addr < base + 0x10000 && !found_screen; addr += 4) {
                auto* obj = memory->TranslateVirtual<uint8_t*>(addr);
                if (!obj) continue;
                uint32_t np = xe::load_and_swap<uint32_t>(obj + 0x20);
                if (np < 0x82000000 || np > 0x83200000) continue;
                auto* ns = memory->TranslateVirtual<char*>(np);
                if (!ns) continue;
                if (target_name == ns) {
                  found_screen = addr;
                }
              }
            }

            if (found_screen) {
              XELOGI("DC3: Nav bypass: {} -> {} ({:08X}) at nuiFrame={}", 
                     cur_name, target_name, found_screen, s_skel_calls);
              xe::store_and_swap<uint32_t>(ui_obj_h + 0x2c, 1); // kTransitionTo
              xe::store_and_swap<uint32_t>(ui_obj_h + 0x4c, found_screen);
              s_screen_stable_count = 0;
            } else {
              if ((s_skel_calls % 60) == 0) {
                XELOGW("DC3: Nav bypass: FAILED to find target screen '{}'", target_name);
              }
            }
          }
        }
      }
    }
  }

  // --- Diagnostic: periodically log UIManager state ---
  // This helps debug the code cave's GotoScreen/PopScreen transition.
  // Read TheUI->mTransitionState (offset 0x2c) and mCurrentScreen (0x48)
  // every 600 NUI calls (~10 seconds).
  if ((s_skel_calls % 600) == 0) {
    constexpr uint32_t kTheUI_diag = 0x82F1A8E0;
    auto* ui_ptr_diag = memory->TranslateVirtual<uint8_t*>(kTheUI_diag);
    if (ui_ptr_diag) {
      uint32_t ui_addr_diag = xe::load_and_swap<uint32_t>(ui_ptr_diag);
      if (ui_addr_diag && ui_addr_diag < 0xF0000000) {
        auto* ui_obj_diag = memory->TranslateVirtual<uint8_t*>(ui_addr_diag);
        if (ui_obj_diag) {
          uint32_t trans_state_diag = xe::load_and_swap<uint32_t>(ui_obj_diag + 0x2c);
          uint32_t cur_screen_diag = xe::load_and_swap<uint32_t>(ui_obj_diag + 0x48);
          uint32_t trans_screen_diag = xe::load_and_swap<uint32_t>(ui_obj_diag + 0x4c);
          std::string screen_name_diag = "unknown";
          if (cur_screen_diag && cur_screen_diag < 0xF0000000) {
            auto* scr_diag = memory->TranslateVirtual<uint8_t*>(cur_screen_diag);
            if (scr_diag) {
              uint32_t name_ptr_diag = xe::load_and_swap<uint32_t>(scr_diag + 0x20);
              if (name_ptr_diag && name_ptr_diag < 0xF0000000) {
                auto* name_diag = memory->TranslateVirtual<char*>(name_ptr_diag);
                if (name_diag) screen_name_diag = name_diag;
              }
            }
          }
          constexpr uint32_t kCounterAddr_diag = 0x825EF610;
          auto* ctr_diag = memory->TranslateVirtual<uint8_t*>(kCounterAddr_diag);
          uint32_t cave_ctr_diag = ctr_diag ? xe::load_and_swap<uint32_t>(ctr_diag) : 0;

          // Read MultiUserGesturePanel::Poll first instruction to verify patch
          constexpr uint32_t kMultiUserPoll_diag = 0x82942EB8;
          auto* mup_ptr_diag = memory->TranslateVirtual<uint8_t*>(kMultiUserPoll_diag);
          uint32_t poll_insn_diag = mup_ptr_diag ? xe::load_and_swap<uint32_t>(mup_ptr_diag) : 0;

          XELOGI("DC3: UIManager diag: screen='{}' transState={} curScreen={:08X} "
                 "transScreen={:08X} caveCounter={} nuiCalls={} pollInsn={:08X}",
                 screen_name_diag, trans_state_diag, cur_screen_diag,
                 trans_screen_diag, cave_ctr_diag, s_skel_calls, poll_insn_diag);
        }
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
  kernel_state_->TerminateTitle();
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  on_terminate();
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::LaunchPath(const std::filesystem::path& path) {
  Dc3RuntimeTelemetryEndSession("launch_path_reset");
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
  if (title_id_.has_value() && title_id_.value() == 0x373307D9) {
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
    const uint32_t kLiR3_0 = 0x38600000;   // li r3, 0  (return S_OK / 0)
    const uint32_t kLiR3_Neg1 = 0x3860FFFF; // li r3, -1 (return E_UNEXPECTED)
    const uint32_t kLiR3_1 = 0x38600001;    // li r3, 1
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
      // Always pass the manifest when available — its target addresses come
      // from the MAP file and are reliable even when PE/XEX fingerprints differ.
      // The fingerprint mismatch flag only affects the warning, not resolution.
      auto resolved = Dc3ResolveNuiPatchTarget(active_patches[i], text_info,
                                               patch_manifest
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
      // Handle NUI fake skeleton when enabled.
      if (cvars::fake_kinect_data && !is_decomp_layout &&
          std::string_view(patch.name) == "NuiSkeletonGetNextFrame") {
        return Dc3NuiFakeSkeletonGetNextFrameExtern;
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
      // Safety: when no manifest is loaded at all, catalog-fallback
      // addresses are hardcoded defaults that may be from a different build.
      // Reject them.  When a manifest IS loaded, catalog addresses come from
      // MAP symbol resolution and are reliable even if the PE/XEX fingerprint
      // doesn't match (PE and XEX fingerprints always differ for decomp
      // builds due to section layout differences).
      if (!patch_manifest.has_value() &&
          resolved_patch.resolve_method ==
              Dc3PatchResolveMethod::kCatalogAddress) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(catalog fallback rejected — no manifest loaded; address "
            "likely stale)",
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
  if (title_id_.has_value() && title_id_.value() == 0x373307D9) {
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
    XELOGI("DC3: Skipping hack pack for original XEX (NUI overrides already applied)");
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
          heap->Protect(addr, size,
                        kMemoryProtectRead | kMemoryProtectWrite);
          apply(ptr);
          return true;
        };
    // Stub SaveLoadManager::Activate to a bare blr so the
    // wait_main_after_saveload_screen never triggers XContent cache
    // operations.  CacheXbox::ThreadGetFileSize has a bug where the success
    // path never stores the file size into mData, so mCacheFileSize stays
    // garbage.  A subsequent _MemAllocTemp(garbage_size) crashes with
    // "Allocation failure, heap 'main', want 536876708 bytes".
    // With mActivated=0 after the stub, IsIdle() immediately returns true
    // and saveload_complete fires with no XContent load attempt.
    constexpr uint32_t kSaveLoadManagerActivate = 0x82894A10;
    {
      with_patch_target("SaveLoadManager::Activate", kSaveLoadManagerActivate, 4,
                        [&](uint8_t* sla_ptr) {
                          xe::store_and_swap<uint32_t>(sla_ptr, 0x4E800020);  // blr
                          XELOGI(
                              "DC3: Stubbed SaveLoadManager::Activate at {:08X} to blr",
                              kSaveLoadManagerActivate);
                        });
    }
    // Stub CDReadDone to always return true.
    //
    // CDReadDone() calls GetOverlappedResult() on the global OVERLAPPED
    // used by CDRead for async ark file reads.  Xenia completes all
    // NtReadFile calls synchronously, but the XAPILIB ReadFile wrapper
    // sets OVERLAPPED.Internal to STATUS_PENDING before calling NtReadFile
    // and Xenia writes the result to a separate stack IO_STATUS_BLOCK —
    // so OVERLAPPED.Internal stays PENDING forever.  GetOverlappedResult
    // then returns ERROR_IO_PENDING and CDReadDone returns false, causing
    // BlockMgr::Poll to spin forever on pending async tasks.
    //
    // Without this fix, ArkFile::ReadAsync (used by FileLoader for .mogg
    // and other large files) never completes: mNumOutstandingTasks never
    // reaches 0, so HamAudio::IsReady() returns false forever, blocking
    // the entire song timeline (Game::HandleWait loops → beat frozen →
    // character animation frozen → no IK telemetry).
    //
    // Returning true is correct because Xenia's NtReadFile completes
    // synchronously — the data is already in the buffer when CDReadDone
    // is first called.
    //
    // Address from dc_symbols.txt: CDReadDone = 0x826026E0
    constexpr uint32_t kCDReadDone = 0x826026E0;
    {
      with_patch_target("CDReadDone", kCDReadDone, 8, [&](uint8_t* cdr_ptr) {
        xe::store_and_swap<uint32_t>(cdr_ptr + 0, 0x38600001);  // li r3, 1
        xe::store_and_swap<uint32_t>(cdr_ptr + 4, 0x4E800020);  // blr
        XELOGI(
            "DC3: Stubbed CDReadDone at {:08X} to return true "
            "(fixes async ark I/O for mogg loading on original XEX)",
            kCDReadDone);
      });
    }
    // Stub ContentMgr::RefreshDone to always return true.
    //
    // LoadingPanel::IsLoaded() gates on TheContentMgr.RefreshDone().
    // The ContentMgr starts in state kDone (0), and RefreshDone()
    // returns (mState == kDiscoveryEnumerating), i.e. state 1.
    // If StartRefresh() is never called (no XContent enumeration
    // under Xenia) or the discovery pipeline stalls, RefreshDone()
    // stays false forever and the loading screen never transitions
    // to game_screen.
    //
    // Address from symbols.txt: ContentMgr::RefreshDone = 0x825FEB48
    constexpr uint32_t kContentMgrRefreshDone = 0x825FEB48;
    {
      with_patch_target("ContentMgr::RefreshDone", kContentMgrRefreshDone, 8,
                        [&](uint8_t* crd_ptr) {
                          xe::store_and_swap<uint32_t>(crd_ptr + 0, 0x38600001);  // li r3, 1
                          xe::store_and_swap<uint32_t>(crd_ptr + 4, 0x4E800020);  // blr
                          XELOGI(
                              "DC3: Stubbed ContentMgr::RefreshDone at {:08X} to return "
                              "true (unblocks LoadingPanel::IsLoaded → game_screen)",
                              kContentMgrRefreshDone);
                        });
    }

    if (cvars::fake_kinect_data && !dc3_is_decomp_layout) {
      XELOGI("DC3: Entering original-XEX fake Kinect patch block");
      // Patch 1: NOP the IsTrackingAllSkeletons() guard in SetPlayerPresent.
      // This allows player_present to propagate even when no real skeletons
      // are detected (using fake tracking IDs written by NUI host handler).
      constexpr uint32_t kSetPlayerPresent_guard = 0x8290834C;
      constexpr uint32_t kExpectedInsn = 0x4800001D;  // bl IsTrackingAllSkeletons
      auto* insn_ptr =
          memory_->TranslateVirtual<uint8_t*>(kSetPlayerPresent_guard);
      if (insn_ptr) {
        uint32_t actual = xe::load_and_swap<uint32_t>(insn_ptr);
        if (actual == kExpectedInsn) {
          auto* heap = memory_->LookupHeap(kSetPlayerPresent_guard);
          if (heap) {
            heap->Protect(kSetPlayerPresent_guard, 4,
                          kMemoryProtectRead | kMemoryProtectWrite);
            xe::store_and_swap<uint32_t>(insn_ptr, 0x60000000);  // nop
            XELOGI("DC3: Calibration bypass: NOP'd IsTrackingAllSkeletons "
                   "guard in SetPlayerPresent at {:08X}",
                   kSetPlayerPresent_guard);
          }
        } else {
          XELOGW("DC3: Calibration bypass: unexpected insn at {:08X}: "
                  "{:08X} (expected {:08X})",
                  kSetPlayerPresent_guard, actual, kExpectedInsn);
        }
      } else {
        XELOGW("DC3: Patch lookup failed: SetPlayerPresent guard at {:08X} "
               "(TranslateVirtual returned null)",
               kSetPlayerPresent_guard);
      }

      // Patch 2: Stub ChoosePlayerSides to a bare blr.
      // The choose_player_sides_panel expects skeletal interaction to
      // assign players to left/right sides.  Stubbing it prevents the
      // panel from waiting for assignment input.
      constexpr uint32_t kChoosePlayerSides = 0x82909968;
      {
        with_patch_target("ChoosePlayerSides", kChoosePlayerSides, 4,
                          [&](uint8_t* cps_ptr) {
                            xe::store_and_swap<uint32_t>(cps_ptr, 0x4E800020);  // blr
                            XELOGI("DC3: Calibration bypass: stubbed ChoosePlayerSides "
                                   "at {:08X} to blr",
                                   kChoosePlayerSides);
                          });
      }

      // Patch 3: Stub SetPlayerSkeletonWarningData to a bare blr.
      // This function triggers the "Kinect can't see you" warnings
      // which block multiuser_panel flow.
      constexpr uint32_t kSetPlayerSkeletonWarningData = 0x82907880;
      {
        with_patch_target("SetPlayerSkeletonWarningData",
                          kSetPlayerSkeletonWarningData, 4,
                          [&](uint8_t* spw_ptr) {
                            xe::store_and_swap<uint32_t>(spw_ptr, 0x4E800020);  // blr
                            XELOGI("DC3: Calibration bypass: stubbed "
                                   "SetPlayerSkeletonWarningData at {:08X} to blr",
                                   kSetPlayerSkeletonWarningData);
                          });
      }

      // Patch 4b: Replace SetPlayerSkeletonNavData with a stub that always
      // calls SetPlayerPresent(0, true) and SetPlayerPresent(1, true).
      // This is THE critical fix: the original function calls
      // GetPlayerFilteredSkeletonID which returns -1 (no real skeletons),
      // then CLEARS the forced tracking IDs to -1 via AssignSkeleton.
      // This oscillation prevents player_present from stabilizing.
      constexpr uint32_t kSetPlayerSkeletonNavData = 0x82909340;
      constexpr uint32_t kSetPlayerPresent = 0x82908320;
      {
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
            w(i++, 0x7C0802A6);           // mflr r0
            w(i++, 0x90010004);           // stw r0, 4(r1)
            w(i++, 0x9421FFC0);           // stwu r1, -64(r1)
            w(i++, 0x38600000);           // li r3, 0 (p0)
            w(i++, 0x38800001);           // li r4, 1 (true)
            w(i++, 0x48000001 | (kSetPlayerPresent - 0x82909350)); // bl
            w(i++, 0x38600001);           // li r3, 1 (p1)
            w(i++, 0x38800001);           // li r4, 1 (true)
            w(i++, 0x48000001 | (kSetPlayerPresent - 0x82909358)); // bl
            w(i++, 0x38210040);           // addi r1, r1, 64
            w(i++, 0x80010004);           // lwz r0, 4(r1)
            w(i++, 0x7C0803A6);           // mtlr r0
            w(i++, 0x4E800020);           // blr
            XELOGI("DC3: Calibration bypass: replaced "
                   "SetPlayerSkeletonNavData at {:08X} with "
                   "SetPlayerPresent(0,true)+SetPlayerPresent(1,true) "
                   "stub ({} instructions)",
                   kSetPlayerSkeletonNavData, i);
          }
        }
      }

      // Patch 5: Stub ShouldWaitForRecovery to return false.
      // Prevents the Kinect "recovery" screen from blocking the multiuser panel.
      constexpr uint32_t kShouldWaitForRecovery = 0x82904CD0;
      {
        auto* swr_ptr =
            memory_->TranslateVirtual<uint8_t*>(kShouldWaitForRecovery);
        if (swr_ptr) {
          auto* heap = memory_->LookupHeap(kShouldWaitForRecovery);
          if (heap) {
            heap->Protect(kShouldWaitForRecovery, 8,
                          kMemoryProtectRead | kMemoryProtectWrite);
            xe::store_and_swap<uint32_t>(swr_ptr + 0, 0x38600000);  // li r3, 0
            xe::store_and_swap<uint32_t>(swr_ptr + 4, 0x4E800020);  // blr
            XELOGI("DC3: Calibration bypass: stubbed "
                   "ShouldWaitForRecovery at {:08X} to return false",
                   kShouldWaitForRecovery);
          }
        }
      }

      // Patch 6: Overwrite MultiUserGesturePanel::Poll IN-PLACE with code
      // that calls TexLoadPanel::Poll (parent), then after 400 frames
      // calls ForceLetterboxOffImmediate + GotoScreen("loading_screen").
      //
      // IMPORTANT: Code is written directly at Poll's address (0x82942EB8)
      // rather than in a remote dead zone, because Xenia's JIT compiles
      // per-function.  Writing inside the function body ensures the JIT
      // sees the new code when MultiUserGesturePanel::Poll is entered.
      //
      // Guest addresses (original XEX, verified in map file + Ghidra):
      //   MultiUserGesturePanel::Poll     = 0x82942EB8
      //   TexLoadPanel::Poll (vbase)      = 0x82926AA0
      //   TheUI (UIManager* global ptr)   = 0x82F1A8E0
      //   TheHamUI (HamUI global value)   = 0x831179E8
      //   UIManager::GotoScreen(char*,b,b)= 0x8277B378
      //   HamUI::ForceLetterboxOffImm.    = 0x8288FF40
      //   "loading_screen" string literal = 0x820F3798
      {
        constexpr uint32_t kMultiUserPoll = 0x82942EB8;
        constexpr uint32_t kMultiUserPollSize = 0x70;  // original function size
        constexpr uint32_t kCounterAddr = 0x825EF610;  // in ProtocolDebugString zone
        constexpr uint32_t kTexLoadPanelPoll = 0x82926AA0;
        constexpr uint32_t kGotoScreen = 0x8277B378;
        constexpr uint32_t kForceLetterboxOff = 0x8288FF40;
        constexpr uint32_t kTheUI = 0x82F1A8E0;
        constexpr uint32_t kTheHamUI = 0x831179E8;
        constexpr uint32_t kLoadingScreenStr = 0x820F3798;

        constexpr int kFrameSize = 64;
        constexpr int16_t kOff_SaveR30 = kFrameSize - 8;  // 56
        constexpr int16_t kOff_SaveR31 = kFrameSize - 4;  // 60
        constexpr int16_t kDelayFrames = 400;

        auto* heap = memory_->LookupHeap(kMultiUserPoll);
        auto* code = memory_->TranslateVirtual<uint8_t*>(kMultiUserPoll);
        auto* ctr_heap = memory_->LookupHeap(kCounterAddr);
        auto* ctr_ptr = memory_->TranslateVirtual<uint8_t*>(kCounterAddr);

        if (heap && code && ctr_heap && ctr_ptr) {
          heap->Protect(kMultiUserPoll, kMultiUserPollSize,
                        kMemoryProtectRead | kMemoryProtectWrite);
          ctr_heap->Protect(kCounterAddr, 4,
                            kMemoryProtectRead | kMemoryProtectWrite);

          // Zero the counter
          xe::store_and_swap<uint32_t>(ctr_ptr, 0);

          auto w = [code](int idx, uint32_t insn) {
            xe::store_and_swap<uint32_t>(code + idx * 4, insn);
          };
          auto code_addr = [](int idx) -> uint32_t {
            return kMultiUserPoll + idx * 4;
          };
          auto branch = [](uint32_t from, uint32_t to, bool linked) -> uint32_t {
            int32_t delta = static_cast<int32_t>(to - from);
            return 0x48000000 | (delta & 0x03FFFFFC) | (linked ? 1 : 0);
          };
          auto lwz = [](int rt, int16_t d, int ra) -> uint32_t {
            return 0x80000000 | (rt << 21) | (ra << 16) |
                   (static_cast<uint16_t>(d));
          };
          auto stw = [](int rs, int16_t d, int ra) -> uint32_t {
            return 0x90000000 | (rs << 21) | (ra << 16) |
                   (static_cast<uint16_t>(d));
          };
          auto li = [](int rt, int16_t imm) -> uint32_t {
            return 0x38000000 | (rt << 21) | (static_cast<uint16_t>(imm));
          };
          auto mr = [](int ra, int rs) -> uint32_t {
            return 0x7C000378 | (rs << 21) | (ra << 16) | (rs << 11);
          };
          auto addi = [](int rt, int ra, int16_t imm) -> uint32_t {
            return 0x38000000 | (rt << 21) | (ra << 16) |
                   (static_cast<uint16_t>(imm));
          };
          auto lis = [](int rt, int16_t imm) -> uint32_t {
            return 0x3C000000 | (rt << 21) | (static_cast<uint16_t>(imm));
          };
          auto cmpwi = [](int ra, int16_t imm) -> uint32_t {
            return 0x2C000000 | (ra << 16) | (static_cast<uint16_t>(imm));
          };
          auto addr_hi = [](uint32_t a) -> uint16_t {
            return (a >> 16) + ((a & 0x8000) ? 1 : 0);
          };
          auto addr_lo = [](uint32_t a) -> int16_t {
            return static_cast<int16_t>(a & 0xFFFF);
          };

          int i = 0;
          // Prologue
          w(i++, 0x7C0802A6);                    // mflr r0
          w(i++, stw(0, 4, 1));                  // stw r0, 4(r1)
          w(i++, 0x94210000 | (static_cast<uint16_t>(-kFrameSize))); // stwu r1, -64(r1)
          w(i++, stw(31, kOff_SaveR31, 1));      // stw r31, 60(r1)
          w(i++, stw(30, kOff_SaveR30, 1));      // stw r30, 56(r1)
          w(i++, mr(31, 3));                     // mr r31, r3 (save this)

          // Always call TexLoadPanel::Poll(this)
          w(i++, mr(3, 31));                     // mr r3, r31
          { uint32_t a = code_addr(i);
            w(i++, branch(a, kTexLoadPanelPoll, true)); }

          // Load and increment counter
          w(i++, lis(30, addr_hi(kCounterAddr)));
          w(i++, lwz(10, addr_lo(kCounterAddr), 30));
          w(i++, addi(10, 10, 1));
          w(i++, stw(10, addr_lo(kCounterAddr), 30));

          // if counter != kDelayFrames, skip to epilogue
          w(i++, cmpwi(10, kDelayFrames));
          int bne_idx = i;
          w(i++, 0x40820000);                    // bne (patch later)

          // === Call HamUI::ForceLetterboxOffImmediate() ===
          w(i++, lis(3, addr_hi(kTheHamUI)));
          w(i++, addi(3, 3, addr_lo(kTheHamUI)));
          { uint32_t a = code_addr(i);
            w(i++, branch(a, kForceLetterboxOff, true)); }

          // === Call UIManager::GotoScreen("loading_screen", 0, 0) ===
          w(i++, lis(11, addr_hi(kTheUI)));
          w(i++, lwz(3, addr_lo(kTheUI), 11));   // r3 = *TheUI
          w(i++, lis(11, addr_hi(kLoadingScreenStr)));
          w(i++, addi(4, 11, addr_lo(kLoadingScreenStr)));
          w(i++, li(5, 0));
          w(i++, li(6, 0));
          { uint32_t a = code_addr(i);
            w(i++, branch(a, kGotoScreen, true)); }

          // Patch bne target to epilogue
          int epilogue_idx = i;
          int32_t bne_delta = code_addr(epilogue_idx) - code_addr(bne_idx);
          xe::store_and_swap<uint32_t>(
              code + bne_idx * 4,
              0x40820000 | (bne_delta & 0xFFFC));

          // Epilogue
          w(i++, lwz(30, kOff_SaveR30, 1));
          w(i++, lwz(31, kOff_SaveR31, 1));
          w(i++, addi(1, 1, kFrameSize));
          w(i++, lwz(0, 4, 1));
          w(i++, 0x7C0803A6);                    // mtlr r0
          w(i++, 0x4E800020);                    // blr

          XELOGI("DC3: Calibration bypass: wrote {} PPC instructions "
                 "({} bytes) IN-PLACE at MultiUserGesturePanel::Poll "
                 "{:08X} (original size {}, overflow {})",
                 i, i * 4, kMultiUserPoll, kMultiUserPollSize,
                 (i * 4 > kMultiUserPollSize) ?
                     i * 4 - kMultiUserPollSize : 0);
        }
      }

      // Patch 7: Stub ExitControllerMode to a bare blr.
      // This function triggers "entering kinect mode" transitions which
      // clear the mInControllerMode flag and block menu navigation.
      constexpr uint32_t kExitControllerMode = 0x82902748;
      {
        auto* ecm_ptr = memory_->TranslateVirtual<uint8_t*>(kExitControllerMode);
        if (ecm_ptr) {
          auto* heap = memory_->LookupHeap(kExitControllerMode);
          if (heap) {
            heap->Protect(kExitControllerMode, 4,
                          kMemoryProtectRead | kMemoryProtectWrite);
            xe::store_and_swap<uint32_t>(ecm_ptr, 0x4E800020);  // blr
            XELOGI("DC3: Controller bypass: stubbed ExitControllerMode "
                   "at {:08X} to blr — controller input stays active",
                   kExitControllerMode);
          }
        }
      }

      // Patch 7b: Stub Movie::Poll to return false (movie done).
      // With Bink video decoding stubbed/noop'd, MovieImpl::Poll() always
      // returns true (movie still playing). This blocks MoviePanel::Poll
      // from ever firing "movie_done", trapping the game on attract_screen
      // for ~40s until host timer advancement coincidentally advances past
      // the movie duration. Stubbing Movie::Poll to return false makes all
      // movies immediately report "done", which is correct since no video
      // will ever actually play under emulation with Bink stubbed.
      constexpr uint32_t kMoviePoll = 0x82555CB8;
      {
        auto* mp_ptr = memory_->TranslateVirtual<uint8_t*>(kMoviePoll);
        if (mp_ptr) {
          auto* heap = memory_->LookupHeap(kMoviePoll);
          if (heap) {
            heap->Protect(kMoviePoll, 8,
                          kMemoryProtectRead | kMemoryProtectWrite);
            // li r3, 0; blr  → return false (movie done)
            xe::store_and_swap<uint32_t>(mp_ptr, 0x38600000);     // li r3, 0
            xe::store_and_swap<uint32_t>(mp_ptr + 4, 0x4E800020); // blr
            XELOGI("DC3: Movie bypass: stubbed Movie::Poll at {:08X} "
                   "to return false — movies report done immediately",
                   kMoviePoll);
          }
        }
      }

      // Patch 8: Audio pipeline fix — HandleWait + PostWaitStart.
      //
      // XMAHALAllocateContexts crashes (SIGSEGV) because the XDK's
      // statically-linked HAL code uses MmMapIoSpace for XMA hardware
      // context MMIO, which Xenia's minimal implementation doesn't fully
      // support. This stalls XAudio2, preventing StandardStream::IsReady()
      // from returning true. Game::HandleWait() loops forever.
      //
      // Song audio uses Vorbis (.mogg) via StreamNull (not XMA), so
      // fixing XMA allocation lets the stream pipeline complete normally.
      //
      // 8a: Stub XMAHALAllocateContexts to return E_FAIL (0x80004005).
      //     Prevents SIGSEGV, lets CreateSourceVoice fail gracefully for
      //     XMA SFX voices while preserving Vorbis song audio.
      // 8b: HandleWait: bne→b unconditional at IsReady check.
      //     Ensures dispatch even if audio subsystem is still slow.
      // 8c: PostWaitStart: NOP Fail() guard so body always runs.
      // 8d: Host-side: let HandleWait complete naturally (vtable patch
      //     unblocks audio gate), then activate host-driven beat.
      {
        auto patch4 = [&](uint32_t addr, uint32_t val, const char* desc) {
          auto* p = memory_->TranslateVirtual<uint8_t*>(addr);
          if (p) {
            auto* h = memory_->LookupHeap(addr);
            if (h) {
              h->Protect(addr, 4, kMemoryProtectRead | kMemoryProtectWrite);
              xe::store_and_swap<uint32_t>(p, val);
              XELOGI("DC3: Audio fix: {} at {:08X}", desc, addr);
            }
          }
        };

        // 8a: Stub XMAHALAllocateContexts (0x82E77250) to return S_OK.
        constexpr uint32_t kXMAHALAlloc = 0x82E77250;
        {
          auto* p = memory_->TranslateVirtual<uint8_t*>(kXMAHALAlloc);
          if (p) {
            auto* h = memory_->LookupHeap(kXMAHALAlloc);
            if (h) {
              h->Protect(kXMAHALAlloc, 8,
                         kMemoryProtectRead | kMemoryProtectWrite);
              // li r3, 0; blr  (return S_OK / 0)
              xe::store_and_swap<uint32_t>(p + 0, 0x38600000);  // li r3, 0
              xe::store_and_swap<uint32_t>(p + 4, 0x4E800020);  // blr
              XELOGI("DC3: Audio fix: stubbed XMAHALAllocateContexts at "
                     "{:08X} to return S_OK — prevents XMA SIGSEGV",
                     kXMAHALAlloc);
            }
          }
        }

        // 8b: HandleWait+0x90: bne 40820024 -> b 48000024 (always dispatch)
        patch4(0x82867288 + 0x90, 0x48000024, "HandleWait+0x90: bne->b");

        // 8c: PostWaitStart+0x34: lwz r3, 0x40(r31) (song stream)
        //     In some XEX layouts, if mSongStream is null, audio->Fail()
        //     returns true and PostWaitStart exits early.  NOP the guard.
        // Address: PostWaitStart = 0x82867550
        //   +0x10: bctrl (audio->Fail())
        //   +0x14: cmpwi r3, 0
        //   +0x18: beq +0x34 (if !fail, run body)
        // patch4(0x82867550 + 0x18, 0x48000034, "PostWaitStart: NOP Fail() guard");

        // 8e: Additional audio safety-nets
        // - HandleWait (0x82867288) at 0x90: bne 40820024 -> b 48000024
        //   Ensures wait-state 3 (audio ready) always transitions to state 4
        //   even if the audio subsystem reports not-ready.
        patch4(0x82867288 + 0x90, 0x48000024, "HandleWait+0x90: bne 40820024 → b 48000024 (always dispatch, bypass IsReady check)");

        // - HamAudio::IsReady (0x8252B9E0) at 0x70: bctrl (mSongStream->IsReady) -> li r3, 1
        //   Ensures HamAudio reports ready once the song stream exists.
        patch4(0x8252B9E0 + 0x70, 0x38600001, "HamAudio::IsReady+0x70: bctrl → li_r3_1 (always true)");

        // 8f: HamDirector::SongAnim(int) should fall back to the expert
        // anim under Xenia when the routine-builder path never populates.
        // Patch the entry to:
        //   li r4, 2
        //   b HamDirector::SongAnimByDifficulty
        constexpr uint32_t kHamDirectorSongAnim = 0x82475578;
        {
          auto* p = memory_->TranslateVirtual<uint8_t*>(kHamDirectorSongAnim);
          if (p) {
            auto* h = memory_->LookupHeap(kHamDirectorSongAnim);
            if (h) {
              h->Protect(kHamDirectorSongAnim, 8,
                         kMemoryProtectRead | kMemoryProtectWrite);
              xe::store_and_swap<uint32_t>(p + 0, 0x38800002);  // li r4, 2
              xe::store_and_swap<uint32_t>(p + 4, 0x4BFFE8E0);  // b 82473E58
              XELOGI("DC3: Anim fix 8f: patched HamDirector::SongAnim at "
                     "{:08X} to return SongAnimByDifficulty(expert)",
                     kHamDirectorSongAnim);
            }
          }
        }
      }
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
