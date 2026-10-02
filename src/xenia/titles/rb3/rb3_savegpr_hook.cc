/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914): the __savegprlr_23 override (NOT upstream).
 *
 * We need MemAlloc's entry arguments (r3 = size, r4 = align, r12 = caller LR)
 * while they are live, and a live guest-thread context to run guest code on.
 * Synthetic PPC trampolines/caves proved fragile (the JIT's scanner
 * mis-compiles cave functions; a .data cave corrupted live zero-init
 * statics). Instead we override a REAL, pre-declared guest function on
 * MemAlloc's path: __savegprlr_23 @ 0x82829244 (declared by
 * XexModule::FindSaveRest before any execution). MemAlloc's prologue is
 *   mflr r12 ; bl __savegprlr_23 ; ... ; stwu r1,-0xB0(r1)
 * so at the bl: r3 = size, r4 = align, r12 = caller LR, lr = 0x827BCD40 (the
 * return into MemAlloc, our filter key), r1 = the CALLER's SP. The handler
 * EXACTLY emulates __savegprlr_23 (std r23..r31 at r1-0x50..-0x10, stw r12 at
 * r1-8), so every other function calling it is unaffected.
 ******************************************************************************
 */

#include <atomic>

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"
#include "xenia/titles/rb3/rb3_flags.h"
#include "xenia/titles/rb3/rb3_internal.h"
#include "xenia/titles/title_hooks.h"

namespace xe {
namespace titles {
namespace rb3 {

namespace {

constexpr uint32_t kSaveGprLr23 = 0x82829244;
constexpr uint32_t kMemAllocRet = 0x827BCD40;  // bl at 0x827BCD3C in MemAlloc

void SaveGprLr23Extern(cpu::ppc::PPCContext* ppc_context,
                       kernel::KernelState* kernel_state) {
  static std::atomic<uint64_t> s_memalloc_calls{0};
  if (!ppc_context || !kernel_state) {
    return;
  }
  Memory* memory = kernel_state->memory();
  uint8_t* base = memory->virtual_membase();
  uint32_t sp = static_cast<uint32_t>(ppc_context->r[1]);
  // --- exact __savegprlr_23 emulation (must be semantically transparent) ---
  for (int i = 0; i < 9; ++i) {
    xe::store_and_swap<uint64_t>(base + sp - 0x50 + i * 8,
                                 ppc_context->r[23 + i]);
  }
  xe::store_and_swap<uint32_t>(base + sp - 8,
                               static_cast<uint32_t>(ppc_context->r[12]));
  if (static_cast<uint32_t>(ppc_context->lr) != kMemAllocRet) {
    return;
  }
  uint64_t seq = s_memalloc_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t size = static_cast<uint32_t>(ppc_context->r[3]);
  if (AllocTraceActive()) {
    AllocTraceOnMemAlloc(ppc_context, static_cast<uint32_t>(seq), size, sp);
  }
  if (cvars::si_load_dll) {
    SiHarnessOnMemAlloc(ppc_context, memory, seq);
  }
  if (cvars::rb3dx_alloc_probe) {
    AllocProbeOnMemAlloc(ppc_context, memory, seq, size, sp);
  }
}

}  // namespace

void InstallSaveGprLr23Hook(const TitleLaunchContext& ctx) {
  if (!cvars::rb3dx_alloc_probe && !cvars::si_load_dll &&
      cvars::rb3dx_alloc_trace_path.empty()) {
    return;
  }
  uint32_t insn0 =
      xe::load_and_swap<uint32_t>(ctx.memory->virtual_membase() + kSaveGprLr23);
  if (insn0 != 0xFAE1FFB0) {  // std r23,-0x50(r1)
    XELOGW(
        "RB3DX: __savegprlr_23 override NOT installed (unexpected word "
        "0x{:08X} at 0x{:08X})",
        insn0, kSaveGprLr23);
    return;
  }
  ctx.processor->RegisterGuestFunctionOverride(
      kSaveGprLr23, SaveGprLr23Extern, "RB3DX:__savegprlr_23(MemAlloc hook)");
  XELOGI(
      "RB3DX: MemAlloc hook installed via __savegprlr_23 override at "
      "0x{:08X} (filter lr=0x{:08X})",
      kSaveGprLr23, kMemAllocRet);
}

}  // namespace rb3
}  // namespace titles
}  // namespace xe
