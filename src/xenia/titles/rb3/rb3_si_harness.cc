/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914): same-instrument (RB3Enhanced.dll) harness
 * (NOT upstream). --si_load_dll, --si_init_va, --si_force_allow_va,
 * --si_hook_verify. WORKSTREAM-rb3-on-xenia-bringup.md §8f.
 ******************************************************************************
 */

#include <algorithm>
#include <atomic>

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
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

constexpr uint32_t kH1ProcessConfig = 0x8276FA08;
constexpr uint32_t kH2RecalcGemList = 0x82794740;
constexpr uint32_t kDllBase = 0x84000000;
// The from-source RB3Enhanced.dll image is ~0x850000 (its config sits at
// 0x84829xxx-0x8485Dxxx depending on the build).
constexpr uint32_t kDllEnd = 0x84860000;

// Makes every committed guest page in [lo, hi) read/write through the guest
// heap (so the heap's bookkeeping and the host mapping agree). Returns the
// number of bytes made writable.
uint32_t MakeGuestWritable(Memory* memory, uint32_t lo, uint32_t hi) {
  uint32_t a = lo, bytes = 0;
  while (a < hi) {
    auto* heap = memory->LookupHeap(a);
    HeapAllocationInfo info = {};
    if (!heap || !heap->QueryRegionInfo(a, &info) || !info.region_size) {
      a += 0x10000;
      continue;
    }
    uint32_t region_end = static_cast<uint32_t>(std::min<uint64_t>(
        hi, static_cast<uint64_t>(info.base_address) + info.region_size));
    if (region_end <= a) {
      a += 0x10000;
      continue;
    }
    if ((info.state & kMemoryAllocationCommit) &&
        heap->Protect(a, region_end - a,
                      kMemoryProtectRead | kMemoryProtectWrite)) {
      bytes += region_end - a;
    }
    a = region_end;
  }
  return bytes;
}

// RB3Enhanced-DLL same-instrument GAMEPLAY-hook install verifier
// (--si_hook_verify). Read-only. RB3E's HookFunction rewrites the FIRST
// instruction of the target to `b <stub-in-DLL>`, so the pass condition is
// observable in guest RAM: word[site] decodes to a `b` whose target is inside
// the DLL image and within +/-32MB (PPC I-form reach). Stock (no DLL / hooks
// not installed) = 0x7D8802A6 (mflr r12).
void SiHookVerifyThread(Memory* memory) {
  GuestReader reader(memory, /*trust_image_windows=*/cvars::si_load_dll);
  const uint32_t kStockPrologue = 0x7D8802A6;  // mflr r12
  auto classify = [&](const char* tag, uint32_t site) {
    if (!reader.Readable(site)) {
      XELOGW("SI HOOKVERIFY {} @0x{:08X}: UNREADABLE", tag, site);
      return;
    }
    uint32_t w = reader.R32(site);
    if (w == kStockPrologue) {
      XELOGW("SI HOOKVERIFY {} @0x{:08X}: word=0x{:08X} [STOCK mflr r12 -- NOT "
             "hooked]",
             tag, site, w);
      return;
    }
    if ((w >> 26) != 18u) {
      XELOGW("SI HOOKVERIFY {} @0x{:08X}: word=0x{:08X} [NON-BRANCH first "
             "word]",
             tag, site, w);
      return;
    }
    int32_t li = static_cast<int32_t>(w & 0x03FFFFFCu);
    if (li & 0x02000000) li |= static_cast<int32_t>(0xFC000000u);
    uint32_t tgt = (w & 2) ? static_cast<uint32_t>(li)
                           : static_cast<uint32_t>(int64_t(site) + li);
    int64_t rel = int64_t(tgt) - int64_t(site);
    bool in_dll = tgt >= kDllBase && tgt < kDllEnd;
    bool in_reach = rel > -33554432 && rel < 33554432;
    XELOGW("SI HOOKVERIFY {} @0x{:08X}: word=0x{:08X} b->0x{:08X} (rel={}) [{}]",
           tag, site, w, tgt, static_cast<long long>(rel),
           in_dll ? (in_reach ? "PASS -- b into DLL space" : "OUT OF REACH")
                  : "b to NON-DLL target");
  };
  for (uint32_t n = 0;; ++n) {
    if (n < 3 || (n % 20) == 0) {
      bool mapped = reader.Readable(kDllBase);
      XELOGI("SI HOOKVERIFY[{}]: DLL-base word@0x{:08X}=0x{:08X} ({})", n,
             kDllBase, reader.R32(kDllBase),
             mapped ? "mapped" : "unmapped -- DLL NOT loaded");
      classify("H1", kH1ProcessConfig);
      classify("H2", kH2RecalcGemList);
    }
    if (!ProbeSleep(500)) {
      return;
    }
  }
}

}  // namespace

void InstallSiHarness(const TitleLaunchContext& ctx) {
  if (cvars::si_hook_verify) {
    Memory* memory = ctx.memory;
    SpawnProbeThread([memory]() { SiHookVerifyThread(memory); });
    XELOGI("RB3DX: SI DLL-hook verify thread started (--si_hook_verify)");
  }
  if (!cvars::si_load_dll) {
    return;
  }
  // Map the DLL here, at the stable pre-LaunchModule point: loading a module
  // from a mid-boot guest-thread callback, with the guest heaps live, races
  // the loader. Only the DLL's own code may install the hooks (current
  // InitSameInstrument installs ~20 SI_HOOKs + ~22 game-call stub pokes), and
  // that has to run on a live guest thread -- SiHarnessOnMemAlloc.
  auto dll = ctx.kernel_state->LoadUserModule("game:\\RB3Enhanced.dll",
                                              /*call_entry=*/false);
  if (!dll) {
    XELOGE("SI LOADDLL: LoadUserModule(game:\\RB3Enhanced.dll) FAILED");
    return;
  }
  XELOGW("SI LOADDLL: RB3Enhanced.dll loaded entry=0x{:08X} is_dll={}",
         dll->entry_point(), dll->is_dll_module());
  if (cvars::si_init_va == 0) {
    XELOGE(
        "SI LOADDLL: --si_init_va is 0 -- nothing will install the hooks "
        "(the host-emulated detour table, approach (b), was removed: it "
        "cannot wire the current DLL's hook set)");
  }
  // call_entry=false skips DllMain/ini load, so config.AllowSameInstrument
  // stays 0 and the installed hooks run pass-through unless armed here.
  if (cvars::si_force_allow_va != 0) {
    uint32_t allow_va = static_cast<uint32_t>(cvars::si_force_allow_va);
    auto* heap = ctx.memory->LookupHeap(allow_va);
    if (heap && heap->Protect(allow_va, 1,
                              kMemoryProtectRead | kMemoryProtectWrite)) {
      uint8_t* host = ctx.memory->virtual_membase() + allow_va;
      uint8_t before = *host;
      *host = 1;
      XELOGW(
          "SI LOADDLL: config.AllowSameInstrument @0x{:08X} {} -> 1 "
          "(--si_force_allow_va; hook bodies armed)",
          allow_va, before);
    } else {
      XELOGE(
          "SI LOADDLL: config.AllowSameInstrument @0x{:08X} NOT armed: the "
          "guest heap does not track that page as committed (see "
          "GuestReader's trust_image_windows note)",
          allow_va);
    }
  }
}

void SiHarnessOnMemAlloc(cpu::ppc::PPCContext* ppc_context, Memory* memory,
                         uint64_t seq) {
  static std::atomic<bool> s_fired{false};
  bool expected = false;
  // Fire once, well into boot (heaps up, hook sites resolvable but not yet
  // JIT-compiled, so lazy compilation picks up the patched bytes).
  if (cvars::si_init_va == 0 || seq < 800 ||
      !s_fired.compare_exchange_strong(expected, true)) {
    return;
  }
  uint32_t init_va = static_cast<uint32_t>(cvars::si_init_va);
  // RB3E_PokeBranch / HookFunction are plain guest stores into title .text
  // and DLL .text with no dcbst/icbi. On hardware those pages are writable;
  // under Xenia they are read-only (the first SI_POKE_B guest-faulted,
  // /tmp/rb3-si2). Make them writable through the guest heap, and leave them
  // so: the DLL's game-call stubs keep poking these ranges for the whole run.
  uint32_t title_bytes = MakeGuestWritable(memory, 0x82000000u, 0x83000000u);
  uint32_t dll_bytes = MakeGuestWritable(memory, kDllBase, kDllEnd);
  XELOGW(
      "SI LOADDLL: made 0x{:X} title-image bytes and 0x{:X} DLL-image bytes "
      "guest-writable",
      title_bytes, dll_bytes);
  if (!title_bytes || !dll_bytes) {
    XELOGE(
        "SI LOADDLL: InitSameInstrument NOT run: the guest heap does not "
        "track the {} image as committed, so its pages cannot be made "
        "writable (XexModule::Load resets the image heap's page table when "
        "the DLL loads -- an emulator gap)",
        title_bytes ? "DLL" : "title");
    return;
  }
  uint64_t saved_r[32];
  for (int i = 0; i < 32; ++i) saved_r[i] = ppc_context->r[i];
  uint64_t saved_lr = ppc_context->lr;
  uint64_t saved_ctr = ppc_context->ctr;
  XELOGW(
      "SI LOADDLL: invoking InitSameInstrument @0x{:08X} on guest thread "
      "(MemAlloc call #{})",
      init_va, seq);
  bool ok = ppc_context->processor->Execute(ppc_context->thread_state, init_va);
  for (int i = 0; i < 32; ++i) ppc_context->r[i] = saved_r[i];
  ppc_context->lr = saved_lr;
  ppc_context->ctr = saved_ctr;
  uint8_t* base = memory->virtual_membase();
  XELOGW(
      "SI LOADDLL: InitSameInstrument returned ok={}. H1@0x{:08X}=0x{:08X} "
      "H2@0x{:08X}=0x{:08X}",
      ok, kH1ProcessConfig,
      xe::load_and_swap<uint32_t>(base + kH1ProcessConfig), kH2RecalcGemList,
      xe::load_and_swap<uint32_t>(base + kH2RecalcGemList));
}

}  // namespace rb3
}  // namespace titles
}  // namespace xe
