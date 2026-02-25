#include "xenia/dc3_hack_pack.h"

#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/memory.h"

DECLARE_bool(fake_kinect_data);

namespace xe {
namespace {

}  // namespace

Dc3HackApplyResult ApplyDc3SkeletonHackPack(const Dc3HackContext& ctx) {
  Dc3HackApplyResult result;
  result.category = Dc3HackCategory::kSkeleton;

  if (!ctx.memory) {
    result.failed++;
    return result;
  }
  if (!cvars::fake_kinect_data || ctx.is_decomp_layout) {
    result.skipped++;
    return result;
  }

  Memory* memory = ctx.memory;
  const uint32_t kGetNextFrameAddr = 0x829C2790;
  const uint32_t kSkeletonFrameSize = 0xAB0;  // 2736 bytes
  const uint32_t kDataSize = kSkeletonFrameSize + 4;  // +4 for counter

  uint32_t data_guest_addr = memory->SystemHeapAlloc(kDataSize, 0x10);
  if (!data_guest_addr) {
    XELOGW("DC3: Failed to allocate guest memory for fake skeleton data");
    result.failed++;
    return result;
  }
  const uint32_t kSkeletonDataAddr = data_guest_addr;
  const uint32_t kCounterAddr = data_guest_addr + kSkeletonFrameSize;

  auto* heap = memory->LookupHeap(kGetNextFrameAddr);
  if (!heap) {
    result.failed++;
    return result;
  }

  heap->Protect(kGetNextFrameAddr, 0x4C, kMemoryProtectRead | kMemoryProtectWrite);
  auto* stub_mem = memory->TranslateVirtual<uint8_t*>(kGetNextFrameAddr);
  auto* counter_mem = memory->TranslateVirtual<uint8_t*>(kCounterAddr);
  auto* data_mem = memory->TranslateVirtual<uint8_t*>(kSkeletonDataAddr);
  if (!stub_mem || !counter_mem || !data_mem) {
    XELOGW("DC3: Failed to translate memory for fake Kinect skeleton injection");
    result.failed++;
    return result;
  }

  uint32_t ppc_stub[] = {
      0x7C882378,                                // mr r8, r4
      0x3CA00000 | (kSkeletonDataAddr >> 16),    // lis r5, hi16(data)
      0x60A50000 | (kSkeletonDataAddr & 0xFFFF), // ori r5, r5, lo16(data)
      0x38C00000 | (kSkeletonFrameSize / 4),     // li r6, word_count
      0x7CC903A6,                                // mtctr r6
      0x80E50000,                                // lwz r7, 0(r5)
      0x90E40000,                                // stw r7, 0(r4)
      0x38A50004,                                // addi r5, r5, 4
      0x38840004,                                // addi r4, r4, 4
      0x4200FFF0,                                // bdnz -16 (to lwz)
      0x3CA00000 | (kCounterAddr >> 16),         // lis r5, hi16(counter)
      0x60A50000 | (kCounterAddr & 0xFFFF),      // ori r5, r5, lo16(counter)
      0x80C50000,                                // lwz r6, 0(r5)
      0x38C60001,                                // addi r6, r6, 1
      0x90C50000,                                // stw r6, 0(r5)
      0x90C80004,                                // stw r6, 4(r8) (timestamp)
      0x90C80008,                                // stw r6, 8(r8) (frame num)
      0x38600000,                                // li r3, 0 (S_OK)
      0x4E800020,                                // blr
  };
  for (size_t i = 0; i < sizeof(ppc_stub) / sizeof(ppc_stub[0]); i++) {
    xe::store_and_swap<uint32_t>(stub_mem + i * 4, ppc_stub[i]);
  }

  xe::store_and_swap<uint32_t>(counter_mem, 0);
  std::memset(data_mem, 0, kSkeletonFrameSize);

  auto write_float = [data_mem](uint32_t offset, float value) {
    xe::store_and_swap<float>(data_mem + offset, value);
  };
  auto write_u32 = [data_mem](uint32_t offset, uint32_t value) {
    xe::store_and_swap<uint32_t>(data_mem + offset, value);
  };

  write_u32(0x0008, 1);
  write_float(0x0014, 1.0f);
  write_float(0x0024, 1.0f);

  const uint32_t skel0 = 0x30;
  write_u32(skel0 + 0x00, 2);
  write_u32(skel0 + 0x04, 1);
  write_u32(skel0 + 0x0C, 0);
  write_float(skel0 + 0x10, 0.0f);
  write_float(skel0 + 0x14, 0.9f);
  write_float(skel0 + 0x18, 2.0f);
  write_float(skel0 + 0x1C, 1.0f);

  struct JointPos {
    float x, y, z;
  };
  JointPos joints[20] = {
      {0.00f, 0.90f, 2.0f},   {0.00f, 1.10f, 2.0f},   {0.00f, 1.35f, 2.0f},
      {0.00f, 1.60f, 2.0f},   {-0.20f, 1.35f, 2.0f},  {-0.50f, 1.35f, 2.0f},
      {-0.75f, 1.35f, 2.0f},  {-0.85f, 1.35f, 2.0f},  {0.20f, 1.35f, 2.0f},
      {0.50f, 1.35f, 2.0f},   {0.75f, 1.35f, 2.0f},   {0.85f, 1.35f, 2.0f},
      {-0.15f, 0.90f, 2.0f},  {-0.15f, 0.50f, 2.0f},  {-0.15f, 0.05f, 2.0f},
      {0.15f, 0.90f, 2.0f},   {0.15f, 0.50f, 2.0f},   {0.15f, 0.05f, 2.0f},
      {-0.15f, 0.00f, 2.0f},  {0.15f, 0.00f, 2.0f},
  };
  const uint32_t joints_offset = skel0 + 0x20;
  for (int j = 0; j < 20; j++) {
    uint32_t off = joints_offset + j * 16;
    write_float(off + 0, joints[j].x);
    write_float(off + 4, joints[j].y);
    write_float(off + 8, joints[j].z);
    write_float(off + 12, 1.0f);
  }
  const uint32_t tracking_offset = skel0 + 0x160;
  for (int j = 0; j < 20; j++) {
    write_u32(tracking_offset + j * 4, 2);
  }

  struct BinaryPatch {
    uint32_t address;
    uint32_t value;
    const char* name;
  };
  BinaryPatch skel_patches[] = {
      {0x8242E74C, 0x3B800021,
       "SkeletonUpdateThread: timeout INFINITE -> 33ms"},
      {0x8242E1B0, 0x60000000, "SkeletonUpdate::Update: NOP IsOverride branch"},
  };
  for (const auto& p : skel_patches) {
    auto* h = memory->LookupHeap(p.address);
    if (!h) {
      result.failed++;
      continue;
    }
    h->Protect(p.address, 4, kMemoryProtectRead | kMemoryProtectWrite);
    auto* m = memory->TranslateVirtual<uint8_t*>(p.address);
    if (!m) {
      result.failed++;
      continue;
    }
    xe::store_and_swap<uint32_t>(m, p.value);
    XELOGI("  Patched {:08X}: {}", p.address, p.name);
    result.applied++;
  }

  XELOGI("DC3: Fake Kinect skeleton data written at {:08X} ({} bytes), "
         "PPC stub at {:08X}, counter at {:08X}",
         kSkeletonDataAddr, kSkeletonFrameSize, kGetNextFrameAddr, kCounterAddr);
  result.applied++;
  return result;
}

}  // namespace xe
