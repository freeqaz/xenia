/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 IK telemetry caves and reader (--dc3_ik_telemetry) (NOT upstream).
 *
 * Moved verbatim out of decomp/dc3_hack_pack.cc: it is applied to the
 * original layout as well, so it does not belong to the decomp pack.
 ******************************************************************************
 */

#include "xenia/titles/dc3/dc3_ik_telemetry.h"


#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <set>
#include <string>

#include "xenia/config.h"

#if defined(__linux__)
#include <cerrno>
#include <sys/mman.h>
#endif

#include "xenia/base/atomic.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/user_module.h"
#include "xenia/memory.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/titles/dc3/dc3_addresses.h"

DECLARE_bool(dc3_ik_telemetry);

namespace xe {

// Guest address of IK telemetry data (allocated at runtime via SystemHeapAlloc).
static uint32_t g_ik_telemetry_guest_addr = 0;
// Slot layout — captures return values and entry args across the IK call chain.
// All multi-byte values are big-endian in guest memory (PPC native order).
constexpr uint32_t kIkSlotTotalWeight = 0;       // float — ApplyConstraints f1
constexpr uint32_t kIkSlotGroundHeight = 4;       // float — GetGroundHeight f1
constexpr uint32_t kIkSlotEffectorType = 8;       // u32   — GetType r3 (enum)
constexpr uint32_t kIkSlotPosWeight = 12;         // float — ApplyPosConstraints f1
constexpr uint32_t kIkSlotPollThisPtr = 16;       // u32   — Poll entry r3 (this*)
constexpr uint32_t kIkSlotIkElbowZ = 20;         // float — IKElbow entry v.z
constexpr uint32_t kIkSlotDoFancyWeight = 24;    // float — DoFancyElbow entry f1
// Ankle-clamp ground truth (HamIKEffector::Poll ankle branch, src lines 438-451).
// Captured by an in-function code cave at the clamp point (guest 0x824C24E4,
// `fmr f1, f29`, just before the Interp(neutralQ.v, effQ.v, clampFactor, q.v)):
//   neutralZ = neutralQ.v.z  (stack 0xC8(r1) — neutralQ.v base @ 0xC0)
//   effZ     = effQ.v.z      (stack 0x78(r1) — effQ.v base   @ 0x70)
//   clampFactor = final clamped factor in f29 at the patch point.
// These three are the disambiguator for the native feet-in-floor bug: native
// measures neutralZ==effZ==-4.02 (collapsed) -> Interp no-op -> foot sinks.
constexpr uint32_t kIkSlotClampNeutralZ = 28;    // float — ankle clamp neutralQ.v.z
constexpr uint32_t kIkSlotClampEffZ = 32;        // float — ankle clamp effQ.v.z
constexpr uint32_t kIkSlotClampFactor = 36;      // float — ankle clamp factor (f29)
// Ankle-clamp X/Y ground truth (HamIKEffector::Poll ankle branch).  Captured by
// the SAME in-function clamp cave (guest 0x824C24E4) — neutralQ.v base @ 0xC0(r1)
// and effQ.v base @ 0x70(r1).  These confirm/refute whether Xbox runs the ankle
// IK in CHARACTER-LOCAL space (X≈0) vs VENUE-WORLD space (X≈±55, native bug).
constexpr uint32_t kIkSlotClampNeutralX = 40;    // float — ankle clamp neutralQ.v.x (0xC0)
constexpr uint32_t kIkSlotClampNeutralY = 44;    // float — ankle clamp neutralQ.v.y (0xC4)
constexpr uint32_t kIkSlotClampEffX = 48;        // float — ankle clamp effQ.v.x (0x70)
constexpr uint32_t kIkSlotClampEffY = 52;        // float — ankle clamp effQ.v.y (0x74)
// Ankle IK FINAL OUTPUT (HamIKEffector::Poll, src line 755 `mEffector->
// SetWorldXfm(finalXfm)`).  Captured by an in-function code cave at the
// instruction just before the FINAL SetWorldXfm `bl` (guest 0x824C26C8,
// `addi r4, r1, 0xE0` — loading &finalXfm for the call).  finalXfm is the
// stack Transform at 0xE0(r1); its Vector3 v lives at +0x24, so:
//   finalXfm.v.x = 0x104(r1), .y = 0x108(r1), .z = 0x10C(r1).
// This is the ankle world position the IK actually writes — AFTER the Interp
// blend, the `q.v += remaining*effQ.v` accumulation, AND the finger->effector
// back-transform Multiply((effW∘fingerW⁻¹), finalXfm, finalXfm).  The native
// port's equivalent value EXPLODES (~60-348); this is the apples-to-apples
// Xbox ground truth for whether the back-transform produces a PLANTED
// (z≈0-1, sane X/Y) ankle on the same venue-world inputs.  The captured `this`
// (effector, r31 = adjusted HamIKEffector*) lets the reader resolve the
// effector name (bone_L-ankle.ikf vs bone_R-ankle.ikf).
constexpr uint32_t kIkSlotFinalXfmX = 56;        // float — finalXfm.v.x (0x104)
constexpr uint32_t kIkSlotFinalXfmY = 60;        // float — finalXfm.v.y (0x108)
constexpr uint32_t kIkSlotFinalXfmZ = 64;        // float — finalXfm.v.z (0x10C)
constexpr uint32_t kIkSlotFinalXfmThis = 68;     // u32   — effector this* (r31)
// ANKLE-ONLY finalXfm OUT.  The final SetWorldXfm is shared by every effector
// type, so the single slot above is overwritten by whichever effector polls
// last (in practice the HAND, type=3).  These slots are written by the SAME
// cave ONLY when the effector type (r27) == 2 (kEffectorTypeAnkle), so they
// reliably hold the ANKLE's finalXfm — the value we actually want to compare
// against the native port's exploding ankle output.
constexpr uint32_t kIkSlotAnkleOutX = 72;        // float — ankle finalXfm.v.x
constexpr uint32_t kIkSlotAnkleOutY = 76;        // float — ankle finalXfm.v.y
constexpr uint32_t kIkSlotAnkleOutZ = 80;        // float — ankle finalXfm.v.z
constexpr uint32_t kIkSlotAnkleOutThis = 84;     // u32   — ankle effector this*
constexpr uint32_t kIkTelemetrySize = 88;

// HamIKEffector guest member offsets (from HamIKEffector.h, verified via
// ghidra-struct + lookup_struct_offset 2026-06-03).
constexpr uint32_t kHikOffMEffector = 0x44;    // ObjPtr<RndTransformable>
constexpr uint32_t kHikOffMGround = 0x6C;      // ObjPtr<RndTransformable>
constexpr uint32_t kHikOffMMore = 0x80;         // ObjPtr<HamIKEffector>
constexpr uint32_t kHikOffMConstraints = 0xBC;  // ObjVector<Constraint>

// ObjPtr<T> sub-object pointer offset (ObjRefConcrete::mObject). The stored
// pointer is ALREADY adjusted to T's sub-object, so e.g. an
// ObjPtr<RndTransformable>::mObject can be read with RndTransformable-relative
// offsets directly.  Object.h: ObjRef{vptr@0,next@4,prev@8}, mObject@0xC.
constexpr uint32_t kObjPtrObjOff = 0x0C;

// RndTransformable sub-object member offsets (Trans.h; ghidra-struct OK).
// mWorldXfm is a Transform { Matrix3 m@0x0 (0x24); Vector3 v@0x24 }, so the
// world POSITION lives at mWorldXfm + 0x24 = 0x48 + 0x24 = 0x6C.
constexpr uint32_t kTransWorldPosX = 0x6C;  // mWorldXfm.v.x
constexpr uint32_t kTransWorldPosY = 0x70;  // mWorldXfm.v.y
constexpr uint32_t kTransWorldPosZ = 0x74;  // mWorldXfm.v.z
constexpr uint32_t kTransDirty = 0xBD;       // RndTransformable::mDirty

// RndMesh secondary-base offset of its RndTransformable sub-object.
// Ground truth from the RndMesh ctor (0x82640F70): the RndTransformable ctor is
// invoked with this = r30+0x40, and RndMesh::MakeWorldSphere reads !mDirty at
// (mesh+0xFD) and mWorldXfm at (mesh+0x88) — i.e. 0x40 + RndTransformable's
// 0x48/0xBD.  So an RndMesh* (base) reaches world position at
// 0x40 + 0x6C = 0xAC and dirty at 0x40 + 0xBD = 0xFD.
constexpr uint32_t kRndMeshTransSubObj = 0x40;

// Recovering the complete-object base from a stored Hmx::Object pointer.
// ObjectDir::Entry::obj is whatever SetName stored, i.e. `this` adjusted to the
// Hmx::Object VIRTUAL base sub-object — NOT the most-derived base.  MSVC stores,
// at vftable[-1], an RTTICompleteObjectLocator whose +4 field is the byte offset
// of that sub-object from the complete object top.  So:
//     vbptr   = *(obj)            // the Object@Hmx vftable pointer
//     col     = *(vbptr - 4)      // RTTICompleteObjectLocator
//     vbaseOff= *(col + 4)        // offset of this Hmx::Object within complete
//     base    = obj - vbaseOff    // most-derived object base
// Verified statically: HamIKEffector vbaseOff=0xE4, RndMesh vbaseOff=0x190
// (matches the class layouts).  This recovery is type-agnostic and lets us read
// HamIKEffector / RndMesh member offsets correctly off the entry obj.
constexpr uint32_t kColOffsetField = 0x04;   // RTTICompleteObjectLocator::offset
// Sanity clamp for the recovered vbase offset (no real class exceeds this).
constexpr uint32_t kMaxVbaseOffset = 0x1000;

// ObjectDir guest member offsets (Dir.h: ObjectDir : virtual Hmx::Object).
// These are relative to the ObjectDir sub-object base.
constexpr uint32_t kObjDirHashTableOff = 0x08;  // KeylessHash<const char*,Entry>
constexpr uint32_t kObjDirSubDirsOff = 0x50;    // std::vector<ObjDirPtr<ObjectDir>>
// KeylessHash layout: mEntries@0x0, mSize@0x4, mOwnEntries@0x8, mNumEntries@0xC.
constexpr uint32_t kKlhEntriesOff = 0x00;
constexpr uint32_t kKlhSizeOff = 0x04;
// ObjectDir::Entry { const char* name@0x0; Hmx::Object* obj@0x4 } = 8 bytes.
constexpr uint32_t kDirEntrySize = 0x08;
// ObjDirPtr<ObjectDir> element size + its mObject offset (ObjRefConcrete).
constexpr uint32_t kObjDirPtrSize = 0x14;
constexpr uint32_t kObjDirPtrObjOff = 0x0C;

// ============================================================================
// IK Telemetry Instrumentation (--dc3_ik_telemetry)
// ============================================================================
//
// Instruments the HamIKEffector call chain via PPC bytepatch to capture
// function return values and entry arguments into guest memory slots.
// A host override on HolmesClientPoll reads and logs the slots every 60
// frames, including derived data read through the captured `this` pointer.
//
// Instrumented functions (return-value capture via blr-patching):
//   ApplyConstraints  → f1 = totalWeight
//   GetGroundHeight   → f1 = groundHeight
//   GetType           → r3 = EffectorType enum
//   ApplyPosConstraints → f1 = position constraint weight
//
// Instrumented functions (entry capture via displaced-instruction):
//   Poll              → r3 = this (HamIKEffector*)
//   IKElbow           → r4+8 = v.z (ankle Z before elbow modifies parents)
//   DoFancyElbow      → f1 = handWeight (totalWeight for hand effectors)
//
// Code cave layout in protocol_debug_string area:
//   +0..+11   : gSystemConfig cave (existing)
//   +12..+27  : DataReadStream cave (existing)
//   +28..+39  : ApplyConstraints return cave  (lis/stfs/blr)
//   +40..+51  : GetGroundHeight return cave   (lis/stfs/blr)
//   +52..+63  : GetType return cave           (lis/stw/blr)
//   +64..+75  : ApplyPosConstraints ret cave  (lis/stfs/blr)
//   +76..+91  : Poll entry cave               (lis/stw/displaced/b-back)
//   +92..+111 : IKElbow entry cave            (lfs/lis/stfs/displaced/b-back)
//   +112..+127: DoFancyElbow entry cave       (lis/stfs/displaced/b-back)
//   +128..+215: ankle clamp cave (22 insns)   (7 stfs captures+displaced+b-back)
//   +216..+319: finalXfm OUT cave (26 insns)   (general OUT + ankle-gated OUT)

// Helper: build a 3-instruction PPC return cave that stores f1 (float) to
// a guest slot before returning.  Writes: lis r11,hi / stfs f1,lo(r11) / blr.
static void BuildReturnCaveF1(uint8_t* cave_mem, uint32_t slot_addr) {
  uint32_t hi = (slot_addr >> 16) & 0xFFFF;
  uint32_t lo = slot_addr & 0xFFFF;
  if (static_cast<int16_t>(lo) < 0) hi += 1;  // addis sign adjustment

  // lis r11, hi16(slot)
  xe::store_and_swap<uint32_t>(cave_mem + 0, 0x3D600000 | (hi & 0xFFFF));
  // stfs f1, lo16(slot)(r11)
  xe::store_and_swap<uint32_t>(cave_mem + 4, 0xD02B0000 | (lo & 0xFFFF));
  // blr
  xe::store_and_swap<uint32_t>(cave_mem + 8, 0x4E800020);
}

// Helper: build a 3-instruction PPC return cave that stores r3 (uint32) to
// a guest slot before returning.  Writes: lis r11,hi / stw r3,lo(r11) / blr.
static void BuildReturnCaveR3(uint8_t* cave_mem, uint32_t slot_addr) {
  uint32_t hi = (slot_addr >> 16) & 0xFFFF;
  uint32_t lo = slot_addr & 0xFFFF;
  if (static_cast<int16_t>(lo) < 0) hi += 1;

  // lis r11, hi16(slot)
  xe::store_and_swap<uint32_t>(cave_mem + 0, 0x3D600000 | (hi & 0xFFFF));
  // stw r3, lo16(slot)(r11)
  xe::store_and_swap<uint32_t>(cave_mem + 4, 0x906B0000 | (lo & 0xFFFF));
  // blr
  xe::store_and_swap<uint32_t>(cave_mem + 8, 0x4E800020);
}

// Helper: build a 4-instruction PPC entry cave that stores r3 (this pointer)
// to a guest slot, executes the displaced first instruction of the target
// function, and branches back to target+4.
// Writes: lis r11,hi / stw r3,lo(r11) / <displaced> / b target+4
static void BuildEntryCaveR3(uint8_t* cave_mem, uint32_t slot_addr,
                              uint32_t displaced_insn,
                              uint32_t cave_guest_addr,
                              uint32_t fn_addr) {
  uint32_t hi = (slot_addr >> 16) & 0xFFFF;
  uint32_t lo = slot_addr & 0xFFFF;
  if (static_cast<int16_t>(lo) < 0) hi += 1;

  // lis r11, hi16(slot)
  xe::store_and_swap<uint32_t>(cave_mem + 0, 0x3D600000 | (hi & 0xFFFF));
  // stw r3, lo16(slot)(r11)
  xe::store_and_swap<uint32_t>(cave_mem + 4, 0x906B0000 | (lo & 0xFFFF));
  // <displaced first instruction>
  xe::store_and_swap<uint32_t>(cave_mem + 8, displaced_insn);
  // b fn_addr+4 (branch back to second instruction of original function)
  int32_t delta = static_cast<int32_t>((fn_addr + 4) - (cave_guest_addr + 12));
  xe::store_and_swap<uint32_t>(cave_mem + 12,
                               0x48000000 | (delta & 0x03FFFFFC));
}

// Helper: build a 5-instruction PPC entry cave that loads v.z from the
// Vector3* in r4 (offset +8), stores it to a telemetry slot, executes the
// displaced first instruction, and branches back to target+4.
// Writes: lfs f0,8(r4) / lis r11,hi / stfs f0,lo(r11) / <displaced> / b fn+4
static void BuildEntryCaveF1R4Z(uint8_t* cave_mem, uint32_t slot_addr,
                                 uint32_t displaced_insn,
                                 uint32_t cave_guest_addr,
                                 uint32_t fn_addr) {
  uint32_t hi = (slot_addr >> 16) & 0xFFFF;
  uint32_t lo = slot_addr & 0xFFFF;
  if (static_cast<int16_t>(lo) < 0) hi += 1;  // addis sign adjustment

  // lfs f0, 8(r4)  — load v.z from Vector3* in r4
  xe::store_and_swap<uint32_t>(cave_mem + 0, 0xC0040008);
  // lis r11, hi16(slot)
  xe::store_and_swap<uint32_t>(cave_mem + 4, 0x3D600000 | (hi & 0xFFFF));
  // stfs f0, lo16(slot)(r11)
  xe::store_and_swap<uint32_t>(cave_mem + 8, 0xD00B0000 | (lo & 0xFFFF));
  // <displaced first instruction>
  xe::store_and_swap<uint32_t>(cave_mem + 12, displaced_insn);
  // b fn_addr+4 (branch back to second instruction of original function)
  int32_t delta = static_cast<int32_t>((fn_addr + 4) - (cave_guest_addr + 16));
  xe::store_and_swap<uint32_t>(cave_mem + 16,
                               0x48000000 | (delta & 0x03FFFFFC));
}

// Helper: build a 4-instruction PPC entry cave that stores f1 (float arg) to
// a telemetry slot, executes the displaced first instruction, and branches
// back to target+4.
// Writes: lis r11,hi / stfs f1,lo(r11) / <displaced> / b fn+4
static void BuildEntryCaveF1(uint8_t* cave_mem, uint32_t slot_addr,
                              uint32_t displaced_insn,
                              uint32_t cave_guest_addr,
                              uint32_t fn_addr) {
  uint32_t hi = (slot_addr >> 16) & 0xFFFF;
  uint32_t lo = slot_addr & 0xFFFF;
  if (static_cast<int16_t>(lo) < 0) hi += 1;  // addis sign adjustment

  // lis r11, hi16(slot)
  xe::store_and_swap<uint32_t>(cave_mem + 0, 0x3D600000 | (hi & 0xFFFF));
  // stfs f1, lo16(slot)(r11)
  xe::store_and_swap<uint32_t>(cave_mem + 4, 0xD02B0000 | (lo & 0xFFFF));
  // <displaced first instruction>
  xe::store_and_swap<uint32_t>(cave_mem + 8, displaced_insn);
  // b fn_addr+4 (branch back to second instruction of original function)
  int32_t delta = static_cast<int32_t>((fn_addr + 4) - (cave_guest_addr + 12));
  xe::store_and_swap<uint32_t>(cave_mem + 12,
                               0x48000000 | (delta & 0x03FFFFFC));
}

// Helper: build an in-function code cave for the HamIKEffector::Poll ankle
// clamp.  Patched at the clamp point (guest 0x824C24E4, `fmr f1, f29`, right
// before Interp(neutralQ.v, effQ.v, clampFactor, q.v)).  At that PC:
//   neutralQ.v base @ 0xC0(r1) → .x@0xC0 .y@0xC4 .z@0xC8
//   effQ.v     base @ 0x70(r1) → .x@0x70 .y@0x74 .z@0x78
//   final clamped clampFactor is in f29.  r1 (sp) is live & valid; r11 is a
//   caller-volatile scratch (the very next op is an Interp `bl` that clobbers
//   it anyway), and f0 is dead here (last written far above, reloaded after).
// Writes 22 insns = 88 bytes (7 stfs captures + displaced + branch-back):
//   neutralZ(0xC8), effZ(0x78), clampFactor(f29),
//   neutralX(0xC0), neutralY(0xC4), effX(0x70), effY(0x74)
// each via:  lfs f0, off(r1) / lis r11, hi(slot) / stfs f0, lo(slot)(r11)
// (clampFactor uses f29 directly instead of an lfs).
//   <displaced fmr f1,f29>
//   b    fn+4   (back to the instruction after the patched one)
static void BuildAnkleClampCave(uint8_t* cave_mem, uint32_t slot_neutral,
                                uint32_t slot_eff, uint32_t slot_factor,
                                uint32_t slot_neutral_x, uint32_t slot_neutral_y,
                                uint32_t slot_eff_x, uint32_t slot_eff_y,
                                uint32_t displaced_insn,
                                uint32_t cave_guest_addr,
                                uint32_t patch_addr) {
  auto lis_lo = [](uint32_t addr, uint32_t& hi, uint32_t& lo) {
    hi = (addr >> 16) & 0xFFFF;
    lo = addr & 0xFFFF;
    if (static_cast<int16_t>(lo) < 0) hi += 1;  // addis sign adjustment
  };
  uint32_t hi, lo;
  uint32_t off = 0;
  auto emit = [&](uint32_t insn) {
    xe::store_and_swap<uint32_t>(cave_mem + off, insn);
    off += 4;
  };
  // Capture a float at stack_off(r1) into telemetry slot via f0/r11 scratch.
  auto capture_stack = [&](uint32_t stack_off, uint32_t slot) {
    emit(0xC0010000 | (stack_off & 0xFFFF));      // lfs f0, stack_off(r1)
    lis_lo(slot, hi, lo);
    emit(0x3D600000 | (hi & 0xFFFF));             // lis r11, hi(slot)
    emit(0xD00B0000 | (lo & 0xFFFF));             // stfs f0, lo(slot)(r11)
  };

  // neutralZ (0xC8)
  capture_stack(0x00C8, slot_neutral);
  // effZ (0x78)
  capture_stack(0x0078, slot_eff);
  // clampFactor (f29)
  lis_lo(slot_factor, hi, lo);
  emit(0x3D600000 | (hi & 0xFFFF));               // lis r11, hi(slotC)
  emit(0xD3AB0000 | (lo & 0xFFFF));               // stfs f29, lo(slotC)(r11)
  // X/Y components (the disambiguator: char-local X≈0 vs venue-world X≈±55).
  capture_stack(0x00C0, slot_neutral_x);          // neutralQ.v.x
  capture_stack(0x00C4, slot_neutral_y);          // neutralQ.v.y
  capture_stack(0x0070, slot_eff_x);              // effQ.v.x
  capture_stack(0x0074, slot_eff_y);              // effQ.v.y
  // displaced original instruction
  emit(displaced_insn);
  // branch back to the instruction after the patched one
  int32_t delta =
      static_cast<int32_t>((patch_addr + 4) - (cave_guest_addr + off));
  emit(0x48000000 | (delta & 0x03FFFFFC));        // b patch_addr+4
}

// Helper: build an in-function code cave for the HamIKEffector::Poll FINAL
// SetWorldXfm OUTPUT.  Patched at the instruction just before the final
// SetWorldXfm `bl` (guest 0x824C26C8, `addi r4, r1, 0xE0` loading &finalXfm).
// At that PC:
//   finalXfm @ 0xE0(r1) (Transform { Matrix3 m@0x0; Vector3 v@0x24 })
//   → finalXfm.v.x @ 0x104(r1), .y @ 0x108(r1), .z @ 0x10C(r1)
//   r31 = the (vbase-adjusted) HamIKEffector* `this` (effector being polled).
//   r27 = the effector type (GetType result); 2 = kEffectorTypeAnkle.
//   r4 is about to be written by the displaced insn (dead here); r11 is a
//   caller-volatile scratch (last write was the IKElbow `bl` just above);
//   f0 is dead at this point; cr0 is free (not read before the displaced insn).
//   r1 (sp), r27, r31 are live & valid.
// The final SetWorldXfm is shared by ALL effector types, so the general slots
// are overwritten by whichever effector polls last (usually the HAND).  We
// therefore ALSO write ankle-only slots gated on r27==2 so the ankle's
// finalXfm survives the hand/pelvis overwrite.
// Writes 16 insns = 64 bytes:
//   [general]  finalXfm.v.x/.y/.z -> slot_x/y/z (lfs/lis/stfs ×3)
//              this (r31)         -> slot_this   (lis/stw)
//   cmpwi r27,2 / bne +ankle_block_len  (skip ankle slots if not ankle)
//   [ankle]    finalXfm.v.x/.y/.z -> ankle_x/y/z (lfs/lis/stfs ×3)
//              this (r31)         -> ankle_this  (lis/stw)
//   <displaced addi r4,r1,0xE0>
//   b patch_addr+4
static void BuildFinalXfmOutCave(uint8_t* cave_mem, uint32_t slot_x,
                                 uint32_t slot_y, uint32_t slot_z,
                                 uint32_t slot_this, uint32_t ankle_x,
                                 uint32_t ankle_y, uint32_t ankle_z,
                                 uint32_t ankle_this, uint32_t displaced_insn,
                                 uint32_t cave_guest_addr,
                                 uint32_t patch_addr) {
  auto lis_lo = [](uint32_t addr, uint32_t& hi, uint32_t& lo) {
    hi = (addr >> 16) & 0xFFFF;
    lo = addr & 0xFFFF;
    if (static_cast<int16_t>(lo) < 0) hi += 1;  // addis sign adjustment
  };
  uint32_t hi, lo;
  uint32_t off = 0;
  auto emit = [&](uint32_t insn) {
    xe::store_and_swap<uint32_t>(cave_mem + off, insn);
    off += 4;
  };
  // Capture a float at stack_off(r1) into a telemetry slot via f0/r11 scratch.
  auto capture_stack = [&](uint32_t stack_off, uint32_t slot) {
    emit(0xC0010000 | (stack_off & 0xFFFF));      // lfs f0, stack_off(r1)
    lis_lo(slot, hi, lo);
    emit(0x3D600000 | (hi & 0xFFFF));             // lis r11, hi(slot)
    emit(0xD00B0000 | (lo & 0xFFFF));             // stfs f0, lo(slot)(r11)
  };
  auto capture_this = [&](uint32_t slot) {
    lis_lo(slot, hi, lo);
    emit(0x3D600000 | (hi & 0xFFFF));             // lis r11, hi(slot)
    emit(0x93EB0000 | (lo & 0xFFFF));             // stw r31, lo(slot)(r11)
  };
  // --- general (last effector) OUT ---
  capture_stack(0x0104, slot_x);                  // finalXfm.v.x
  capture_stack(0x0108, slot_y);                  // finalXfm.v.y
  capture_stack(0x010C, slot_z);                  // finalXfm.v.z
  capture_this(slot_this);                         // this (r31)
  // --- ankle-only OUT: guard on r27 == 2 (kEffectorTypeAnkle) ---
  emit(0x2C1B0002);                                // cmpwi r27, 2 (cr0)
  // bne over the ankle block: 3 capture_stack (3 insns each = 9) + capture_this
  // (2) = 11 insns = 44 bytes.  The bc displacement is relative to the bne's
  // own address; we patch it after emitting the block so it is always exact.
  uint32_t bne_off = off;
  emit(0x40820000);                                // bne cr0, <patched below>
  capture_stack(0x0104, ankle_x);                  // ankle finalXfm.v.x
  capture_stack(0x0108, ankle_y);                  // ankle finalXfm.v.y
  capture_stack(0x010C, ankle_z);                  // ankle finalXfm.v.z
  capture_this(ankle_this);                         // ankle this (r31)
  // Patch the bne target to the instruction right after the ankle block (the
  // displaced insn we are about to emit).
  int32_t bne_delta = static_cast<int32_t>(off - bne_off);
  xe::store_and_swap<uint32_t>(cave_mem + bne_off,
                               0x40820000 | (bne_delta & 0xFFFC));
  // displaced original instruction (addi r4, r1, 0xE0)
  emit(displaced_insn);
  // branch back to the instruction after the patched one
  int32_t delta =
      static_cast<int32_t>((patch_addr + 4) - (cave_guest_addr + off));
  emit(0x48000000 | (delta & 0x03FFFFFC));        // b patch_addr+4
}

// Patch a single mid-function instruction at patch_addr to branch to cave_addr.
// Returns the displaced (original) instruction word.
static uint32_t PatchInstructionAt(Memory* memory, uint32_t patch_addr,
                                   uint32_t cave_addr) {
  auto* mem = memory->TranslateVirtual<uint8_t*>(patch_addr);
  auto* heap = memory->LookupHeap(patch_addr);
  if (!mem || !heap) return 0;
  uint32_t displaced = xe::load_and_swap<uint32_t>(mem);
  heap->Protect(patch_addr, 4, kMemoryProtectRead | kMemoryProtectWrite);
  int32_t delta = static_cast<int32_t>(cave_addr - patch_addr);
  xe::store_and_swap<uint32_t>(mem, 0x48000000 | (delta & 0x03FFFFFC));
  return displaced;
}

// Scan a guest function for blr (0x4E800020) instructions and patch each to
// branch to the given cave address.  Returns the number of patched sites.
static int PatchBlrInstructions(Memory* memory, uint32_t fn_addr,
                                uint32_t fn_size, uint32_t cave_addr) {
  auto* fn_mem = memory->TranslateVirtual<uint8_t*>(fn_addr);
  auto* heap = memory->LookupHeap(fn_addr);
  if (!fn_mem || !heap) return 0;

  heap->Protect(fn_addr, fn_size, kMemoryProtectRead | kMemoryProtectWrite);

  int patched = 0;
  for (uint32_t off = 0; off < fn_size; off += 4) {
    uint32_t insn = xe::load_and_swap<uint32_t>(fn_mem + off);
    if (insn == 0x4E800020) {  // blr
      uint32_t src = fn_addr + off;
      int32_t delta = static_cast<int32_t>(cave_addr - src);
      // b cave  (unconditional branch, AA=0, LK=0)
      xe::store_and_swap<uint32_t>(fn_mem + off,
                                   0x48000000 | (delta & 0x03FFFFFC));
      XELOGI("DC3:IK   patched blr at {:08X} -> b {:08X}", src, cave_addr);
      patched++;
    }
  }
  return patched;
}

// Patch the first instruction of a function to branch to an entry cave.
// Returns the displaced instruction (the original first instruction).
static uint32_t PatchFunctionEntry(Memory* memory, uint32_t fn_addr,
                                   uint32_t cave_addr) {
  auto* fn_mem = memory->TranslateVirtual<uint8_t*>(fn_addr);
  auto* heap = memory->LookupHeap(fn_addr);
  if (!fn_mem || !heap) return 0;

  uint32_t displaced = xe::load_and_swap<uint32_t>(fn_mem);

  heap->Protect(fn_addr, 4, kMemoryProtectRead | kMemoryProtectWrite);
  int32_t delta = static_cast<int32_t>(cave_addr - fn_addr);
  xe::store_and_swap<uint32_t>(fn_mem,
                               0x48000000 | (delta & 0x03FFFFFC));
  return displaced;
}

void ApplyDc3IKInstrumentation(const Dc3HackContext& ctx,
                               Dc3HackApplyResult& result) {
  if (!cvars::dc3_ik_telemetry) {
    result.skipped++;
    return;
  }
  if (!ctx.memory || !ctx.processor) {
    result.failed++;
    return;
  }
  Memory* memory = ctx.memory;

  // Step 1: Allocate guest memory for telemetry data slots.
  uint32_t tel_addr = memory->SystemHeapAlloc(kIkTelemetrySize, 16);
  if (!tel_addr) {
    XELOGW("DC3:IK Failed to allocate telemetry guest memory");
    result.failed++;
    return;
  }
  g_ik_telemetry_guest_addr = tel_addr;

  auto* tel_mem = memory->TranslateVirtual<uint8_t*>(tel_addr);
  if (tel_mem) {
    std::memset(tel_mem, 0, kIkTelemetrySize);
  }
  XELOGI("DC3:IK Telemetry slots at guest {:08X} ({} bytes)",
         tel_addr, kIkTelemetrySize);

  // Step 2: Build PPC code caves. Decomp layout: in the protocol_debug_string
  // area (a no-op'd function there; +0..+27 hold the decomp pack's own
  // caves). Original layout: that address is INSIDE UtilDrawPlane
  // (symbols.txt), so the caves go into the zero padding after the end of
  // .text instead, within .text's last page: executable (inside the module),
  // and referenced by nothing but our branches. Refuse if it is not there.
  uint32_t cave_base_addr = kAddr.protocol_debug_string;
  if (!ctx.is_decomp_layout) {
    cave_base_addr = 0;
    auto* xex = ctx.module ? ctx.module->xex_module() : nullptr;
    auto* text = xex ? xex->GetPESection(".text") : nullptr;
    if (text) {
      uint32_t start = (text->address + text->size + 15) & ~15u;
      uint32_t end = (text->address + text->size + 0xFFF) & ~0xFFFu;
      constexpr uint32_t kNeed = 320 - 28;
      auto* p = memory->TranslateVirtual<uint8_t*>(start);
      bool zero = p && end >= start + kNeed;
      for (uint32_t i = 0; zero && i < kNeed; ++i) {
        zero = p[i] == 0;
      }
      if (zero) {
        cave_base_addr = start - 28;  // caves use offsets +28..+319
        XELOGI("DC3:IK original layout: code caves at {:08X}-{:08X} (zero "
               "padding after .text end {:08X})",
               start, start + kNeed, text->address + text->size);
      }
    }
    if (!cave_base_addr) {
      XELOGW("DC3:IK original layout: no zero padding after .text for the "
             "code caves; IK telemetry NOT applied (it used to overwrite "
             "UtilDrawPlane)");
      result.skipped++;
      return;
    }
  }
  const uint32_t kCaveBase = cave_base_addr;
  constexpr uint32_t kCaveOffAc = 28;    // ApplyConstraints return
  constexpr uint32_t kCaveOffGh = 40;    // GetGroundHeight return
  constexpr uint32_t kCaveOffGt = 52;    // GetType return
  constexpr uint32_t kCaveOffPc = 64;    // ApplyPosConstraints return
  constexpr uint32_t kCaveOffPe = 76;    // Poll entry
  constexpr uint32_t kCaveOffIe = 92;    // IKElbow entry (5 insns = 20 bytes)
  constexpr uint32_t kCaveOffFe = 112;   // DoFancyElbow entry (4 insns = 16 bytes)
  constexpr uint32_t kCaveOffCl = 128;   // ankle clamp cave (22 insns = 88 bytes)
  constexpr uint32_t kCaveOffFx = 216;   // finalXfm OUT cave (26 insns = 104 bytes)
  constexpr uint32_t kCaveTotal = 320;   // total bytes used (216 + 104)

  auto* cave_heap = memory->LookupHeap(kCaveBase);
  auto* cave_mem = memory->TranslateVirtual<uint8_t*>(kCaveBase);
  if (!cave_heap || !cave_mem) {
    XELOGW("DC3:IK Failed to access code cave at {:08X}", kCaveBase);
    result.failed++;
    return;
  }

  cave_heap->Protect(kCaveBase + kCaveOffAc, kCaveTotal - kCaveOffAc,
                     kMemoryProtectRead | kMemoryProtectWrite);

  // --- Return caves (3 insns = 12 bytes each) ---

  // ApplyConstraints: store f1 (totalWeight)
  BuildReturnCaveF1(cave_mem + kCaveOffAc, tel_addr + kIkSlotTotalWeight);
  XELOGI("DC3:IK  cave +{}: ApplyConstraints return -> slot +{}",
         kCaveOffAc, kIkSlotTotalWeight);

  // GetGroundHeight: store f1 (groundHeight)
  BuildReturnCaveF1(cave_mem + kCaveOffGh, tel_addr + kIkSlotGroundHeight);
  XELOGI("DC3:IK  cave +{}: GetGroundHeight return -> slot +{}",
         kCaveOffGh, kIkSlotGroundHeight);

  // GetType: store r3 (EffectorType enum)
  BuildReturnCaveR3(cave_mem + kCaveOffGt, tel_addr + kIkSlotEffectorType);
  XELOGI("DC3:IK  cave +{}: GetType return -> slot +{}",
         kCaveOffGt, kIkSlotEffectorType);

  // ApplyPosConstraints: store f1 (position weight)
  BuildReturnCaveF1(cave_mem + kCaveOffPc, tel_addr + kIkSlotPosWeight);
  XELOGI("DC3:IK  cave +{}: ApplyPosConstraints return -> slot +{}",
         kCaveOffPc, kIkSlotPosWeight);

  // --- Entry caves ---

  // Poll: capture r3 (this*) at entry, then run displaced first insn + b back
  {
    const uint32_t poll_addr = kAddr.ham_ik_poll;
    const uint32_t cave_addr = kCaveBase + kCaveOffPe;
    auto* poll_mem = memory->TranslateVirtual<uint8_t*>(poll_addr);
    if (poll_mem) {
      uint32_t displaced = xe::load_and_swap<uint32_t>(poll_mem);
      BuildEntryCaveR3(cave_mem + kCaveOffPe, tel_addr + kIkSlotPollThisPtr,
                       displaced, cave_addr, poll_addr);
      PatchFunctionEntry(memory, poll_addr, cave_addr);
      XELOGI("DC3:IK  cave +{}: Poll entry (this*) -> slot +{}, "
             "displaced {:08X} from {:08X}",
             kCaveOffPe, kIkSlotPollThisPtr, displaced, poll_addr);
      result.applied++;
    }
  }

  // IKElbow: capture v.z (float at r4+8) at entry (5 insns = 20 bytes)
  {
    const uint32_t fn_addr = kAddr.ham_ik_elbow;
    const uint32_t cave_addr = kCaveBase + kCaveOffIe;
    auto* fn_mem = memory->TranslateVirtual<uint8_t*>(fn_addr);
    if (fn_mem) {
      uint32_t displaced = xe::load_and_swap<uint32_t>(fn_mem);
      BuildEntryCaveF1R4Z(cave_mem + kCaveOffIe, tel_addr + kIkSlotIkElbowZ,
                           displaced, cave_addr, fn_addr);
      PatchFunctionEntry(memory, fn_addr, cave_addr);
      XELOGI("DC3:IK  cave +{}: IKElbow entry (v.z) -> slot +{}, "
             "displaced {:08X} from {:08X}",
             kCaveOffIe, kIkSlotIkElbowZ, displaced, fn_addr);
      result.applied++;
    }
  }

  // DoFancyElbow: capture f1 (handWeight) at entry (4 insns = 16 bytes)
  {
    const uint32_t fn_addr = kAddr.ham_ik_do_fancy_elbow;
    const uint32_t cave_addr = kCaveBase + kCaveOffFe;
    auto* fn_mem = memory->TranslateVirtual<uint8_t*>(fn_addr);
    if (fn_mem) {
      uint32_t displaced = xe::load_and_swap<uint32_t>(fn_mem);
      BuildEntryCaveF1(cave_mem + kCaveOffFe, tel_addr + kIkSlotDoFancyWeight,
                        displaced, cave_addr, fn_addr);
      PatchFunctionEntry(memory, fn_addr, cave_addr);
      XELOGI("DC3:IK  cave +{}: DoFancyElbow entry (f1) -> slot +{}, "
             "displaced {:08X} from {:08X}",
             kCaveOffFe, kIkSlotDoFancyWeight, displaced, fn_addr);
      result.applied++;
    }
  }

  // Ankle clamp: capture neutralZ / effZ / clampFactor at the in-function clamp
  // point inside Poll (guest 0x824C24E4 `fmr f1,f29`, just before the Interp
  // that blends neutralQ.v -> effQ.v by clampFactor).  THIS is the disambiguator
  // for the native feet-in-floor bug.  10-insn cave = 40 bytes.
  {
    const uint32_t patch_addr = kAddr.ham_ik_poll_ankle_clamp;
    const uint32_t cave_addr = kCaveBase + kCaveOffCl;
    auto* patch_mem = memory->TranslateVirtual<uint8_t*>(patch_addr);
    if (patch_mem) {
      uint32_t displaced = xe::load_and_swap<uint32_t>(patch_mem);
      // Sanity: expect `fmr f1, f29` = 0xFC20E890.  Warn but still proceed if
      // the binary layout drifted, so the displaced insn is always faithfully
      // re-executed.
      if (displaced != 0xFC20E890u) {
        XELOGW("DC3:IK  ankle clamp patch point {:08X} has unexpected insn "
               "{:08X} (expected fmr f1,f29 = FC20E890) — proceeding anyway",
               patch_addr, displaced);
      }
      BuildAnkleClampCave(cave_mem + kCaveOffCl,
                          tel_addr + kIkSlotClampNeutralZ,
                          tel_addr + kIkSlotClampEffZ,
                          tel_addr + kIkSlotClampFactor,
                          tel_addr + kIkSlotClampNeutralX,
                          tel_addr + kIkSlotClampNeutralY,
                          tel_addr + kIkSlotClampEffX,
                          tel_addr + kIkSlotClampEffY,
                          displaced, cave_addr, patch_addr);
      PatchInstructionAt(memory, patch_addr, cave_addr);
      XELOGI("DC3:IK  cave +{}: Poll ankle clamp "
             "(neutralXYZ/effXYZ/clampFactor) -> slots "
             "+{}/+{}/+{} (Z) +{}/+{} (neutralXY) +{}/+{} (effXY), "
             "displaced {:08X} from {:08X}",
             kCaveOffCl, kIkSlotClampNeutralZ, kIkSlotClampEffZ,
             kIkSlotClampFactor, kIkSlotClampNeutralX, kIkSlotClampNeutralY,
             kIkSlotClampEffX, kIkSlotClampEffY, displaced, patch_addr);
      result.applied++;
    }
  }

  // finalXfm OUTPUT: capture the ankle IK's FINAL world position at the
  // instruction just before the FINAL SetWorldXfm `bl` inside Poll (guest
  // 0x824C26C8 `addi r4, r1, 0xE0`, loading &finalXfm@0xE0).  finalXfm.v lives
  // at 0xE0 + 0x24 = 0x104/0x108/0x10C(r1).  This is the value SetWorldXfm
  // writes — post Interp blend + post finger->effector back-transform — the
  // apples-to-apples OUTPUT to compare against the native port's exploding one.
  {
    const uint32_t patch_addr = kAddr.ham_ik_poll_final_xfm_out;
    const uint32_t cave_addr = kCaveBase + kCaveOffFx;
    auto* patch_mem = memory->TranslateVirtual<uint8_t*>(patch_addr);
    if (patch_mem) {
      uint32_t displaced = xe::load_and_swap<uint32_t>(patch_mem);
      // Sanity: expect `addi r4, r1, 0xE0` = 0x388100E0.  Warn but proceed if
      // the binary layout drifted (displaced insn is always re-executed).
      if (displaced != 0x388100E0u) {
        XELOGW("DC3:IK  finalXfm OUT patch point {:08X} has unexpected insn "
               "{:08X} (expected addi r4,r1,0xE0 = 388100E0) — proceeding anyway",
               patch_addr, displaced);
      }
      BuildFinalXfmOutCave(cave_mem + kCaveOffFx,
                           tel_addr + kIkSlotFinalXfmX,
                           tel_addr + kIkSlotFinalXfmY,
                           tel_addr + kIkSlotFinalXfmZ,
                           tel_addr + kIkSlotFinalXfmThis,
                           tel_addr + kIkSlotAnkleOutX,
                           tel_addr + kIkSlotAnkleOutY,
                           tel_addr + kIkSlotAnkleOutZ,
                           tel_addr + kIkSlotAnkleOutThis,
                           displaced, cave_addr, patch_addr);
      PatchInstructionAt(memory, patch_addr, cave_addr);
      XELOGI("DC3:IK  cave +{}: Poll finalXfm OUT "
             "(finalXfm.v.x/y/z + this; ankle-gated -> +{}/+{}/+{}/+{}) "
             "-> slots +{}/+{}/+{}/+{}, displaced {:08X} from {:08X}",
             kCaveOffFx, kIkSlotAnkleOutX, kIkSlotAnkleOutY, kIkSlotAnkleOutZ,
             kIkSlotAnkleOutThis, kIkSlotFinalXfmX, kIkSlotFinalXfmY,
             kIkSlotFinalXfmZ, kIkSlotFinalXfmThis, displaced, patch_addr);
      result.applied++;
    }
  }

  // Step 3: Scan and patch blr instructions in target functions.
  struct BlrTarget {
    uint32_t addr;
    uint32_t size;
    uint32_t cave_offset;
    const char* name;
  };
  const BlrTarget targets[] = {
    {kAddr.ham_ik_apply_constraints,     0x244, kCaveOffAc, "ApplyConstraints"},
    {kAddr.ham_ik_get_ground_height,     0x78,  kCaveOffGh, "GetGroundHeight"},
    {kAddr.ham_ik_get_type,              0x25C, kCaveOffGt, "GetType"},
    {kAddr.ham_ik_apply_pos_constraints, 0x1A8, kCaveOffPc, "ApplyPosConstraints"},
  };
  for (const auto& t : targets) {
    int n = PatchBlrInstructions(memory, t.addr, t.size,
                                 kCaveBase + t.cave_offset);
    XELOGI("DC3:IK Patched {} blr sites in {} ({:08X})", n, t.name, t.addr);
    if (n > 0) result.applied++;
    else result.skipped++;
  }

  // Step 4: the slots are READ by ReadDc3IKTelemetry() from the NUI frame
  // callback (dc3_nui_sequencer.cc). The old HolmesClientPoll override that
  // was "left as a dead hook" is gone: on the original layout its address
  // (0x82631C58, a decomp-layout constant) is inside
  // ObjDirItr<RndMat>::Advance, and it never fired on either layout.
}

// ============================================================================
// IK telemetry reader (driven from Dc3NuiSequencerExtern, emulator.cc)
// ============================================================================
//
// Reads the always-fresh scratch slots populated by the byte-patch code-caves
// (see ApplyDc3IKInstrumentation) and walks the captured effector `this` for
// world-space context.  Also attempts a direct named-bone walk of player-0's
// character dir for ground-truth ankle/toe/pelvis bone object addresses.
//
// Every guest read is bounds-checked via the local IsGuestReadable() helper so
// a stale/garbage pointer cannot crash the emulator.  The function no-ops when
// telemetry is not allocated.
void ReadDc3IKTelemetry(Memory* memory, uint32_t frame) {
  if (!cvars::dc3_ik_telemetry) {
    return;
  }
  if (g_ik_telemetry_guest_addr == 0 || !memory) {
    return;
  }

  // Local readability guard mirroring emulator.cc's is_guest_readable: rejects
  // null/kernel/wrapping ranges and pages with no access.
  auto IsGuestReadable = [&](uint32_t guest_addr, uint32_t size) -> bool {
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
  auto LoadU32 = [&](uint32_t guest_addr) -> uint32_t {
    if (!IsGuestReadable(guest_addr, 4)) return 0;
    auto* p = memory->TranslateVirtual<uint8_t*>(guest_addr);
    return p ? xe::load_and_swap<uint32_t>(p) : 0;
  };
  auto LoadF32 = [&](uint32_t guest_addr) -> float {
    if (!IsGuestReadable(guest_addr, 4)) return 0.0f;
    auto* p = memory->TranslateVirtual<uint8_t*>(guest_addr);
    return p ? xe::load_and_swap<float>(p) : 0.0f;
  };
  auto LoadU8 = [&](uint32_t guest_addr) -> uint8_t {
    if (!IsGuestReadable(guest_addr, 1)) return 0;
    auto* p = memory->TranslateVirtual<uint8_t*>(guest_addr);
    return p ? *p : 0;
  };

  const uint32_t tel = g_ik_telemetry_guest_addr;
  if (!IsGuestReadable(tel, kIkTelemetrySize)) {
    return;
  }

  float totalWeight = LoadF32(tel + kIkSlotTotalWeight);
  float groundHeight = LoadF32(tel + kIkSlotGroundHeight);
  uint32_t effectorType = LoadU32(tel + kIkSlotEffectorType);
  float posWeight = LoadF32(tel + kIkSlotPosWeight);
  uint32_t thisPtr = LoadU32(tel + kIkSlotPollThisPtr);
  float ikElbowZ = LoadF32(tel + kIkSlotIkElbowZ);
  float fancyWeight = LoadF32(tel + kIkSlotDoFancyWeight);

  // Ankle-clamp ground truth — captured by the in-function clamp cave in Poll.
  // The slot holds the LAST ankle effector that ran its clamp branch (single
  // slot; both feet share it, but the values are near-identical and stable, so
  // this is sufficient to confirm/refute the native neutralZ==effZ collapse).
  // clampFactor = (neutralZ - groundHeight - 5) * 0.0909, clamped to [0,1].
  float clampNeutralZ = LoadF32(tel + kIkSlotClampNeutralZ);
  float clampEffZ = LoadF32(tel + kIkSlotClampEffZ);
  float clampFactor = LoadF32(tel + kIkSlotClampFactor);
  float clampNeutralX = LoadF32(tel + kIkSlotClampNeutralX);
  float clampNeutralY = LoadF32(tel + kIkSlotClampNeutralY);
  float clampEffX = LoadF32(tel + kIkSlotClampEffX);
  float clampEffY = LoadF32(tel + kIkSlotClampEffY);
  if (clampNeutralZ != 0.0f || clampEffZ != 0.0f || clampFactor != 0.0f) {
    // Reconstruct the expected clampFactor from neutralZ/groundHeight to sanity
    // check the cave (groundHeight is from the GetGroundHeight cave — may lag
    // the per-foot value but is the right order of magnitude).
    float expected = (clampNeutralZ - groundHeight - 5.0f) * 0.09090909f;
    if (expected < 0.0f) expected = 0.0f;
    if (expected > 1.0f) expected = 1.0f;
    XELOGI("DC3:IK CLAMP [frame {}] name=ankle neutralZ={:.4f} effZ={:.4f} "
           "clampFactor={:.4f} groundH={:.4f} (delta neutralZ-effZ={:.4f}, "
           "expectFactor={:.4f}) verdict={}",
           frame, clampNeutralZ, clampEffZ, clampFactor, groundHeight,
           clampNeutralZ - clampEffZ, expected,
           (std::abs(clampNeutralZ - clampEffZ) > 0.5f)
               ? "PLANTED(neutral!=eff)"
               : "COLLAPSED(neutral==eff)");
    // X/Y disambiguator: char-local (|X|≈0) vs venue-world (|X|≈±55, native
    // feet-in-floor bug).  This is the decisive ground-truth line.
    float maxAbsX = std::max(std::abs(clampNeutralX), std::abs(clampEffX));
    const char* spaceVerdict =
        (maxAbsX > 20.0f) ? "VENUE-WORLD(|X|>>0)" : "CHARACTER-LOCAL(|X|~0)";
    XELOGI("DC3:IK CLAMP2 [frame {}] name=ankle "
           "neutral=({:.4f},{:.4f},{:.4f}) eff=({:.4f},{:.4f},{:.4f}) "
           "clampF={:.4f} space={}",
           frame, clampNeutralX, clampNeutralY, clampNeutralZ,
           clampEffX, clampEffY, clampEffZ, clampFactor, spaceVerdict);
  } else {
    XELOGI("DC3:IK CLAMP [frame {}] no ankle-clamp data yet "
           "(ankle branch not hit since last alloc)", frame);
  }

  // Ankle IK FINAL OUTPUT — the value mEffector->SetWorldXfm(finalXfm) writes,
  // AFTER the Interp blend + the finger->effector back-transform.  This is the
  // apples-to-apples Xbox ground truth for whether the back-transform produces
  // a PLANTED ankle (z near floor ~0-1, sane X/Y) on the same venue-world
  // inputs that EXPLODE (~60-348) on the native port.  Single slot: holds the
  // LAST effector (ankle/hand) that reached the final SetWorldXfm.  The
  // captured `this` (= the dir-entry Hmx::Object vbase) is matched against the
  // .ikf entries in the char-dir walk below to print the effector name; this
  // standalone line is the lag-free fallback (effector this* + finalXfm.v).
  float finalXfmX = LoadF32(tel + kIkSlotFinalXfmX);
  float finalXfmY = LoadF32(tel + kIkSlotFinalXfmY);
  float finalXfmZ = LoadF32(tel + kIkSlotFinalXfmZ);
  uint32_t finalXfmThis = LoadU32(tel + kIkSlotFinalXfmThis);
  if (finalXfmThis != 0 || finalXfmX != 0.0f || finalXfmY != 0.0f ||
      finalXfmZ != 0.0f) {
    // PLANTED if z is near the floor with sane (non-exploded) X/Y; EXPLODED if
    // any component blows up (the native back-transform failure mode).
    float maxAbs = std::max({std::abs(finalXfmX), std::abs(finalXfmY),
                             std::abs(finalXfmZ)});
    const char* verdict =
        (maxAbs > 50.0f) ? "EXPLODED(|v|>>0)"
                         : (std::abs(finalXfmZ) < 2.0f ? "PLANTED(z~floor)"
                                                       : "OFF-FLOOR");
    XELOGI("DC3:IK OUT [frame {}] name=<this:{:08X}> "
           "finalXfm=({:.4f},{:.4f},{:.4f}) verdict={}",
           frame, finalXfmThis, finalXfmX, finalXfmY, finalXfmZ, verdict);
  } else {
    XELOGI("DC3:IK OUT [frame {}] no finalXfm data yet "
           "(no effector reached the final SetWorldXfm since last alloc)",
           frame);
  }

  // ANKLE-ONLY finalXfm OUT — same final-SetWorldXfm capture but gated on
  // r27==kEffectorTypeAnkle, so it is NOT overwritten by the later hand/pelvis
  // Polls.  THIS is the ankle's actual world-position output to compare against
  // the native port's exploding ankle (~60-348).  Captured `this` is matched
  // to the bone_L/R-ankle.ikf entries in the char-dir walk below for the name.
  float ankleOutX = LoadF32(tel + kIkSlotAnkleOutX);
  float ankleOutY = LoadF32(tel + kIkSlotAnkleOutY);
  float ankleOutZ = LoadF32(tel + kIkSlotAnkleOutZ);
  uint32_t ankleOutThis = LoadU32(tel + kIkSlotAnkleOutThis);
  if (ankleOutThis != 0 || ankleOutX != 0.0f || ankleOutY != 0.0f ||
      ankleOutZ != 0.0f) {
    float maxAbs = std::max({std::abs(ankleOutX), std::abs(ankleOutY),
                             std::abs(ankleOutZ)});
    const char* verdict =
        (maxAbs > 50.0f) ? "EXPLODED(|v|>>0)"
                         : (std::abs(ankleOutZ) < 2.0f ? "PLANTED(z~floor)"
                                                       : "OFF-FLOOR");
    XELOGI("DC3:IK OUT ANKLE [frame {}] name=<this:{:08X}> "
           "finalXfm=({:.4f},{:.4f},{:.4f}) verdict={}",
           frame, ankleOutThis, ankleOutX, ankleOutY, ankleOutZ, verdict);
  } else {
    XELOGI("DC3:IK OUT ANKLE [frame {}] no ankle finalXfm data yet "
           "(ankle branch not reached final SetWorldXfm since last alloc)",
           frame);
  }

  // Log even if slots are zero — helps diagnose whether the instrumented
  // functions are being called at all.
  if (totalWeight == 0.0f && thisPtr == 0) {
    XELOGI("DC3:IK [frame {}] NO DATA — all slots zero "
           "(IK effectors not active?)", frame);
    // Fall through to the direct bone walk below; it does not depend on the
    // effectors being active.
  } else {
    // Effector type names for readable output.
    static const char* kTypeNames[] = {
      "none", "pelvis", "ankle", "hand", "forearm", "head"
    };
    const char* typeName = (effectorType < 6) ? kTypeNames[effectorType] : "?";

    // Read member data through the captured this pointer for fixture context.
    // ObjPtr layout (Xbox 360 MSVC ABI):
    //   +0x00: vtable, +0x04: ObjRef::next, +0x08: ObjRef::prev,
    //   +0x0C: mObject (T*) — the pointed-to object, +0x10: mOwner.  Size 0x14.
    constexpr uint32_t kObjPtrObjectOffset = 0x0C;

    uint32_t mEffectorPtr = 0;
    uint32_t mGroundPtr = 0;
    uint32_t mMorePtr = 0;
    uint32_t constraintCount = 0;
    float effWorldX = 0, effWorldY = 0, effWorldZ = 0;
    uint8_t effDirty = 0;
    if (IsGuestReadable(thisPtr, 0xD0)) {
      mEffectorPtr = LoadU32(thisPtr + kHikOffMEffector + kObjPtrObjectOffset);
      mGroundPtr = LoadU32(thisPtr + kHikOffMGround + kObjPtrObjectOffset);
      mMorePtr = LoadU32(thisPtr + kHikOffMMore + kObjPtrObjectOffset);
      // ObjVector<Constraint>: mOwner(4) + std::vector { _M_start, _M_finish,
      // _M_end_of_storage }.  Count = (_M_finish - _M_start) / sizeof(Constraint).
      // Constraint = ObjPtr(0x14) + float(0x4) = 0x18 bytes.
      uint32_t vec_start = LoadU32(thisPtr + kHikOffMConstraints + 4);
      uint32_t vec_finish = LoadU32(thisPtr + kHikOffMConstraints + 8);
      if (vec_finish > vec_start && vec_start != 0) {
        constraintCount = (vec_finish - vec_start) / 0x18;
      }

      // Effector bone WorldXfm position + dirty flag (RndTransformable):
      //   +0x48 Transform mWorldXfm (Matrix3 0x24 + Vector3 v at +0x24)
      //     +0x6C v.x, +0x70 v.y, +0x74 v.z ;  +0xBD bool mDirty
      // mEffectorPtr is an ObjPtr<RndTransformable>::mObject, i.e. already
      // adjusted to the RndTransformable sub-object, so these offsets are valid.
      if (IsGuestReadable(mEffectorPtr, 0xBE)) {
        effWorldX = LoadF32(mEffectorPtr + 0x6C);
        effWorldY = LoadF32(mEffectorPtr + 0x70);
        effWorldZ = LoadF32(mEffectorPtr + 0x74);
        effDirty = LoadU8(mEffectorPtr + 0xBD);
      }
    }

    XELOGI("DC3:IK [frame {}] type={} totalWeight={:.4f} "
           "groundHeight={:.4f} posWeight={:.4f} "
           "ikElbowZ={:.4f} fancyWeight={:.4f} "
           "this={:08X} effector={:08X} ground={:08X} "
           "more={:08X} constraints={} "
           "effWorldPos=({:.4f},{:.4f},{:.4f}) effDirty={}",
           frame, typeName, totalWeight, groundHeight, posWeight,
           ikElbowZ, fancyWeight,
           thisPtr, mEffectorPtr, mGroundPtr, mMorePtr,
           constraintCount,
           effWorldX, effWorldY, effWorldZ, effDirty);
  }

  // --------------------------------------------------------------------------
  // PRIMARY GROUND-TRUTH PATH: live char-dir walk of player-0's HamCharacter.
  //
  // This is the master data source — it reads LIVE objects (no 30-frame slot
  // lag) and enumerates ALL bones and ALL ".ikf" effectors in one pass, so we
  // capture every effector type (ankle/pelvis/toe/hand) per read instead of
  // whichever effector happened to Poll last.
  //
  // Locate player-0's HamCharacter via the wardrobe, then scan its ObjectDir
  // hash table AND its subdirs' hash tables (ObjectDir::Find searches subDirs).
  // Each Entry is {const char* name; Hmx::Object* obj}; we read the name string
  // directly so we never need to replicate the hash function.
  //
  // Guest layout (verified 2026-06-03 via ghidra-struct, lookup_struct_offset,
  // and the RndMesh ctor @ 0x82640F70):
  //   *TheHamWardrobe slot @ 0x82F60110 -> HamWardrobe object.
  //   HamWardrobe::mMainCharacters @ +0x18  (ObjPtrVec<HamCharacter>;
  //       Object.h: ObjPtrVec { vptr@0, std::vector<Node> mNodes@0x4,
  //       mOwner@0x10 }; Node@0x14 each, Node.mObject@0xC).
  //   HamCharacter : Character : RndDir : ObjectDir, so the ObjectDir sub-object
  //       is at the HamCharacter base.
  //   ObjectDir::mHashTable @ +0x8, mSubDirs @ +0x50.
  //   KeylessHash { mEntries@0x0, mSize@0x4, mOwnEntries@0x8, mNumEntries@0xC }
  //       stores Entry inline; stride 8.  Empty/removed slots have name==0.
  //
  // Bone world position: bones are RndMesh.  An RndMesh* (base) reaches its
  // RndTransformable sub-object at +0x40 (RndMesh ctor calls the Trans ctor
  // with this=mesh+0x40; MakeWorldSphere reads mWorldXfm at mesh+0x88 and mDirty
  // at mesh+0xFD), so world pos = mesh + 0x40 + 0x6C = mesh+0xAC and dirty =
  // mesh + 0x40 + 0xBD = mesh+0xFD.  The entry "obj" pointer is the Hmx::Object
  // vbase (SetName stores `this`), which for the virtual-base RndMesh /
  // HamIKEffector is NOT the most-derived base.  We recover the real base via
  // the MSVC RTTICompleteObjectLocator (RecoverBase below; vbaseOff is 0x190 for
  // RndMesh and 0xE4 for HamIKEffector, confirmed statically), so both the bone
  // and ".ikf" effector world positions are anchored and lag-free.
  {
    constexpr uint32_t kTheHamWardrobeSlot = 0x82F60110;
    constexpr uint32_t kWardrobeMainChars = 0x18;   // ObjPtrVec<HamCharacter>
    constexpr uint32_t kObjPtrVecNodes = 0x04;      // std::vector<Node>
    constexpr uint32_t kObjPtrVecNodeObj = 0x0C;    // Node.mObject

    static const char* kBoneNames[] = {
      "bone_L-ankle.mesh", "bone_R-ankle.mesh",
      "bone_L-toe.mesh",   "bone_R-toe.mesh",
      "bone_L-knee.mesh",  "bone_R-knee.mesh",
      "bone_L-thigh.mesh", "bone_R-thigh.mesh",
      "bone_pelvis.mesh",  "spot_neck.mesh",
    };
    static const char* kEffTypeNames[] = {
      "none", "pelvis", "ankle", "hand", "forearm", "head"
    };

    // Throttle the verbose DIR-entry dump to a handful of reads total so the
    // log stays readable; emit per-bone / per-effector data every read.
    static int s_dir_dump_count = 0;
    const bool dump_entries = (s_dir_dump_count < 4);

    // Read a bounded guest C-string into out (NUL-terminated). Returns length.
    auto LoadCStr = [&](uint32_t ptr, char* out, int cap) -> int {
      out[0] = 0;
      if (!IsGuestReadable(ptr, 1)) return 0;
      int n = 0;
      for (; n < cap - 1; ++n) {
        uint8_t ch = LoadU8(ptr + static_cast<uint32_t>(n));
        out[n] = static_cast<char>(ch);
        if (ch == 0) break;
      }
      out[cap - 1] = 0;
      return n;
    };
    auto EndsWith = [](const char* s, const char* suf) -> bool {
      size_t ls = std::strlen(s), lf = std::strlen(suf);
      return ls >= lf && std::strcmp(s + (ls - lf), suf) == 0;
    };

    // Classify a HamIKEffector by its mEffector bone name (mirrors GetType()).
    auto ClassifyEff = [&](const char* effName) -> const char* {
      if (!effName || !effName[0]) return "none";
      if (std::strncmp(effName, "bone_pelvis", 11) == 0) return "pelvis";
      if (std::strncmp(effName, "bone_L-ankle", 12) == 0 ||
          std::strncmp(effName, "bone_R-ankle", 12) == 0) return "ankle";
      if (std::strncmp(effName, "bone_L-hand", 11) == 0 ||
          std::strncmp(effName, "bone_R-hand", 11) == 0) return "hand";
      if (std::strncmp(effName, "bone_L-foreArm", 11) == 0 ||
          std::strncmp(effName, "bone_R-foreArm", 11) == 0) return "forearm";
      if (std::strncmp(effName, "bone_head", 9) == 0) return "head";
      return "none";
    };

    // Recover the most-derived object base from a stored Hmx::Object* via the
    // MSVC RTTICompleteObjectLocator (see kColOffsetField comment). Returns 0 if
    // the chain can't be resolved safely.  Type-agnostic — works for any Milo
    // class with a virtual Hmx::Object base.
    auto RecoverBase = [&](uint32_t obj) -> uint32_t {
      if (!IsGuestReadable(obj, 4)) return 0;
      uint32_t vbptr = LoadU32(obj);                 // Object@Hmx vftable
      if (!IsGuestReadable(vbptr, 4)) return 0;
      uint32_t col = LoadU32(vbptr - 4);             // CompleteObjectLocator
      if (!IsGuestReadable(col + kColOffsetField, 4)) return 0;
      uint32_t vbaseOff = LoadU32(col + kColOffsetField);
      if (vbaseOff > kMaxVbaseOffset || vbaseOff > obj) return 0;
      return obj - vbaseOff;                          // most-derived base
    };

    // Scan one ObjectDir's hash table. dir = ObjectDir sub-object base.
    auto ScanDir = [&](uint32_t dir, const char* tag) {
      if (!IsGuestReadable(dir, 0x60)) return;
      uint32_t entries = LoadU32(dir + kObjDirHashTableOff + kKlhEntriesOff);
      int32_t size = static_cast<int32_t>(
          LoadU32(dir + kObjDirHashTableOff + kKlhSizeOff));
      if (size < 0 || size > 0x40000) size = 0;   // clamp garbage
      int dumped = 0;
      for (int32_t i = 0; i < size; ++i) {
        uint32_t entry_addr = entries + static_cast<uint32_t>(i) * kDirEntrySize;
        if (!IsGuestReadable(entry_addr, kDirEntrySize)) break;
        uint32_t name_ptr = LoadU32(entry_addr + 0);
        uint32_t obj_ptr = LoadU32(entry_addr + 4);
        if (!name_ptr || !obj_ptr) continue;       // empty/removed slot
        char name_buf[64];
        LoadCStr(name_ptr, name_buf, sizeof(name_buf));
        if (!name_buf[0]) continue;

        // One-shot visibility dump of the first ~60 non-empty entries.
        if (dump_entries && dumped < 60) {
          XELOGI("DC3:IK DIR[{}] entry[{}] name='{}' obj={:08X}",
                 tag, i, name_buf, obj_ptr);
          ++dumped;
        }

        // ".ikf" entries ARE HamIKEffectors. The entry obj is the Hmx::Object
        // vbase, so recover the HamIKEffector base before applying its member
        // offsets (mEffector @ 0x44, mConstraints @ 0xBC). Then read the
        // effector's bone (mEffector, already an adjusted RndTransformable*)
        // world position + dirty flag + the live constraint count.
        if (EndsWith(name_buf, ".ikf")) {
          uint32_t eff = RecoverBase(obj_ptr);
          uint32_t effBone = 0;
          uint32_t cons = 0;
          float wx = 0, wy = 0, wz = 0;
          uint8_t dirty = 0;
          if (eff && IsGuestReadable(eff, 0xD0)) {
            effBone = LoadU32(eff + kHikOffMEffector + kObjPtrObjOff);
            uint32_t vstart = LoadU32(eff + kHikOffMConstraints + 4);
            uint32_t vfinish = LoadU32(eff + kHikOffMConstraints + 8);
            if (vfinish > vstart && vstart != 0)
              cons = (vfinish - vstart) / 0x18;     // Constraint = 0x18 bytes
            // mEffector ObjPtr stores an already-adjusted RndTransformable*, so
            // its mWorldXfm position is at +0x6C and mDirty at +0xBD directly.
            if (IsGuestReadable(effBone, 0xBE)) {
              wx = LoadF32(effBone + kTransWorldPosX);
              wy = LoadF32(effBone + kTransWorldPosY);
              wz = LoadF32(effBone + kTransWorldPosZ);
              dirty = LoadU8(effBone + kTransDirty);
            }
          }
          // Classify from the .ikf entry name (e.g. "bone_L-ankle.ikf").
          const char* type = ClassifyEff(name_buf);
          XELOGI("DC3:IK EFF name={} type={} constraints={} effBase={:08X} "
                 "effBone={:08X} world=({:.4f},{:.4f},{:.4f}) dirty={}",
                 name_buf, type, cons, eff, effBone, wx, wy, wz, dirty);
          // Resolve the captured finalXfm OUT `this` to this effector's name.
          // The cave captured r31 = the dir-entry Hmx::Object vbase, i.e.
          // exactly obj_ptr for the matching .ikf entry — so a direct match
          // names the effector whose finalXfm world position we captured.
          auto NameOut = [&](uint32_t ot, float ox, float oy, float oz,
                             const char* tag) {
            if (ot == 0 || obj_ptr != ot) return;
            float m = std::max({std::abs(ox), std::abs(oy), std::abs(oz)});
            const char* v =
                (m > 50.0f) ? "EXPLODED(|v|>>0)"
                            : (std::abs(oz) < 2.0f ? "PLANTED(z~floor)"
                                                   : "OFF-FLOOR");
            XELOGI("DC3:IK OUT{} name={} type={} "
                   "finalXfm=({:.4f},{:.4f},{:.4f}) verdict={} (this={:08X})",
                   tag, name_buf, type, ox, oy, oz, v, ot);
          };
          NameOut(finalXfmThis, finalXfmX, finalXfmY, finalXfmZ, "");
          NameOut(ankleOutThis, ankleOutX, ankleOutY, ankleOutZ, " ANKLE");
        } else {
          // Bone mesh? Recover the RndMesh base from the entry's Hmx::Object
          // vbase, then read mWorldXfm.v at base + 0x40 + 0x6C and mDirty at
          // base + 0x40 + 0xBD (RndMesh's RndTransformable sub-object @ +0x40).
          for (const char* wanted : kBoneNames) {
            if (std::strcmp(name_buf, wanted) == 0) {
              uint32_t mesh = RecoverBase(obj_ptr);
              float wx = 0, wy = 0, wz = 0;
              uint8_t dirty = 0;
              bool have = false;
              float lvx = 0, mxx = 0, mxy = 0;
              (void)dirty;
              if (mesh && IsGuestReadable(mesh, 0x100)) {
                // Correct VMX128 RndTransformable layout (verified via GDB-RSP):
                //   mLocalXfm.m.x @ +0x08 (x@+0x08, y@+0x0C), mLocalXfm.v @ +0x38,
                //   mWorldXfm.v @ +0x78. rotZ = atan2(m.x.y, m.x.x) (the knee bend).
                wx = LoadF32(mesh + 0x78);
                wy = LoadF32(mesh + 0x7C);
                wz = LoadF32(mesh + 0x80);
                lvx = LoadF32(mesh + 0x38);
                mxx = LoadF32(mesh + 0x08);
                mxy = LoadF32(mesh + 0x0C);
                have = true;
              }
              // Per-frame leg trajectory: worldZ (beat indicator via pelvis),
              // localVx (bone length), and m.x.x/m.x.y (knee rotZ) for frame-matched
              // native-vs-Xbox comparison.
              XELOGI("DC3:IK BONE name={} meshBase={:08X} "
                     "worldZ={:.4f} localVx={:.4f} mxx={:.5f} mxy={:.5f} valid={}",
                     name_buf, mesh, wz, lvx, mxx, mxy, have ? 1 : 0);
              (void)wx; (void)wy;
              break;
            }
          }
        }
      }
    };

    uint32_t wardrobe = LoadU32(kTheHamWardrobeSlot);
    uint32_t mainChars =
        IsGuestReadable(wardrobe, 0x60) ? wardrobe + kWardrobeMainChars : 0;
    uint32_t vecStart = mainChars ? LoadU32(mainChars + kObjPtrVecNodes) : 0;
    uint32_t vecFinish = mainChars ? LoadU32(mainChars + kObjPtrVecNodes + 4) : 0;
    uint32_t player0 = 0;
    if (vecStart && vecFinish > vecStart) {
      player0 = LoadU32(vecStart + kObjPtrVecNodeObj);
    }

    if (!IsGuestReadable(player0, 0x340)) {
      XELOGI("DC3:IK BONE walk skipped frame {} "
             "(wardrobe={:08X} mainChars={:08X} player0={:08X} not readable)",
             frame, wardrobe, mainChars, player0);
    } else {
      // Scan the character's own dir first.
      ScanDir(player0, "self");

      // Then scan each subdir (bones/effectors are usually loaded as a subdir;
      // ObjectDir::Find walks subDirs, which is why the old own-dir-only walk
      // found 0).  mSubDirs is std::vector<ObjDirPtr<ObjectDir>> @ +0x50; each
      // element is 0x14 bytes with the subdir ObjectDir* at element+0xC.
      uint32_t sdStart = LoadU32(player0 + kObjDirSubDirsOff + 0);
      uint32_t sdFinish = LoadU32(player0 + kObjDirSubDirsOff + 4);
      int sdCount = 0;
      if (sdStart && sdFinish > sdStart) {
        sdCount = static_cast<int>((sdFinish - sdStart) / kObjDirPtrSize);
        if (sdCount < 0 || sdCount > 256) sdCount = 0;  // clamp garbage
      }
      if (dump_entries) {
        XELOGI("DC3:IK DIR player0={:08X} subdirs={} (sdStart={:08X} "
               "sdFinish={:08X})", player0, sdCount, sdStart, sdFinish);
      }
      for (int s = 0; s < sdCount; ++s) {
        uint32_t elem = sdStart + static_cast<uint32_t>(s) * kObjDirPtrSize;
        uint32_t subdir = LoadU32(elem + kObjDirPtrObjOff);
        if (!subdir) continue;
        char tagbuf[16];
        std::snprintf(tagbuf, sizeof(tagbuf), "sub%d", s);
        ScanDir(subdir, tagbuf);
      }

      if (dump_entries) ++s_dir_dump_count;
    }
  }
}

Dc3HackApplyResult ApplyDc3IKTelemetry(const Dc3HackContext& ctx) {
  Dc3HackApplyResult result;
  result.category = Dc3HackCategory::kDebug;
  ApplyDc3IKInstrumentation(ctx, result);
  return result;
}

}  // namespace xe
