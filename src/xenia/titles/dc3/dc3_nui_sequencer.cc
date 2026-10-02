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
#include "xenia/titles/dc3/dc3_hacks.h"
#include "xenia/titles/dc3/dc3_nui_patch_resolver.h"
#include "xenia/titles/dc3/dc3_runtime_telemetry.h"

namespace xe {

using namespace xe::literals;
using namespace xe::dc3;

namespace {

// True when TheUI->mCurrentScreen's name is `name` (original layout).
bool Dc3CurrentScreenNameIs(Memory* memory, const char* name) {
  auto readable = [&](uint32_t addr, uint32_t size) {
    if (!addr || addr >= 0xF0000000) return false;
    auto* heap = memory->LookupHeap(addr);
    return heap && heap->QueryRangeAccess(addr, addr + size - 1) !=
                       xe::memory::PageAccess::kNoAccess;
  };
  constexpr uint32_t kTheUI = 0x82F1A8E0;
  if (!readable(kTheUI, 4)) return false;
  uint32_t ui = xe::load_and_swap<uint32_t>(
      memory->TranslateVirtual<uint8_t*>(kTheUI));
  if (!readable(ui + 0x48, 4)) return false;
  uint32_t scr =
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(ui + 0x48));
  size_t len = std::strlen(name);
  for (uint32_t off : {0x1Cu, 0x20u}) {
    if (!readable(scr + off, 4)) continue;
    uint32_t p = xe::load_and_swap<uint32_t>(
        memory->TranslateVirtual<uint8_t*>(scr + off));
    if (!readable(p, static_cast<uint32_t>(len + 1))) continue;
    if (std::memcmp(memory->TranslateVirtual<char*>(p), name, len + 1) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

void Dc3NuiSequencerExtern(
    cpu::ppc::PPCContext* ppc_context, kernel::KernelState* kernel_state) {
  uint32_t frame_guest_addr = static_cast<uint32_t>(ppc_context->r[4]);
  Memory* memory = kernel_state->memory();
  static int s_skel_calls = 0;
  static uint32_t s_fake_frame_number = 0;
  static bool s_nui_entry_logged = false;

  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
    dc3::HackCountOverrideHit(static_cast<uint32_t>(ppc_context->scratch));
  }
  static const bool kHackControllerMode = dc3::HackGate(
      "seq.controller_mode", "GestureMgr mInControllerMode := 1 per NUI frame");

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

  // Force GestureMgr.mInControllerMode=true so the game processes
  // XInput button presses for menu navigation (DC3 normally uses Kinect).
  if (kHackControllerMode) {
    constexpr uint32_t kTheGestureMgr = 0x82F5F7B4;
    constexpr uint32_t kInControllerModeOff = 0x426D;
    auto* gm_slot = memory->TranslateVirtual<uint8_t*>(kTheGestureMgr);
    if (gm_slot) {
      uint32_t gm_addr = xe::load_and_swap<uint32_t>(gm_slot);
      if (gm_addr && gm_addr < 0xF0000000) {
        auto* gm = memory->TranslateVirtual<uint8_t*>(gm_addr);
        if (gm) {
          gm[kInControllerModeOff] = 1;
          dc3::HackFired("seq.controller_mode");
        }
      }
    }
  }

  // IK telemetry (--dc3_ik_telemetry): read the always-fresh IK scratch slots
  // + bone walk on game_screen, ~2/sec. Read-only; ReadDc3IKTelemetry guards
  // every guest read. (The UI automation that used to follow here runs on
  // the guest main thread now: dc3_autonav.cc.)
  if (cvars::dc3_ik_telemetry && (s_skel_calls % 30) == 0 &&
      Dc3CurrentScreenNameIs(memory, "game_screen")) {
    ReadDc3IKTelemetry(memory, static_cast<uint32_t>(s_skel_calls));
  }
}


}  // namespace xe
