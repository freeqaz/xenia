/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914) title module: entry points shared between its
 * translation units (NOT upstream). Everything here assumes the caller has
 * already checked the title ID.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_RB3_RB3_INTERNAL_H_
#define XENIA_TITLES_RB3_RB3_INTERNAL_H_

#include <cstdint>

#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
class Memory;
namespace titles {
struct TitleLaunchContext;
namespace rb3 {

// rb3_savegpr_hook.cc: the __savegprlr_23 override (MemAlloc's prologue
// helper). Installed when any of its consumers below is enabled.
void InstallSaveGprLr23Hook(const TitleLaunchContext& ctx);

// rb3_alloc_trace.cc
// Opens the binary trace sink and installs the MemFree / MemAlloc-return
// overrides (--rb3dx_alloc_trace_path).
void InstallAllocTrace(const TitleLaunchContext& ctx);
bool AllocTraceActive();
// Called from the __savegprlr_23 override at MemAlloc's entry. `seq` is the
// MemAlloc ordinal; `size` is r3, `sp` the caller's SP.
void AllocTraceOnMemAlloc(cpu::ppc::PPCContext* ppc_context, uint32_t seq,
                          uint32_t size, uint32_t sp);
// --rb3dx_alloc_probe: log suspicious (top byte set) and >= 4 MiB requests.
void AllocProbeOnMemAlloc(cpu::ppc::PPCContext* ppc_context, Memory* memory,
                          uint64_t seq, uint32_t size, uint32_t sp);

// rb3_si_harness.cc
// --si_load_dll (map the DLL, arm the allow flag) and --si_hook_verify.
void InstallSiHarness(const TitleLaunchContext& ctx);
// Called from the __savegprlr_23 override at MemAlloc's entry, on a live
// guest thread: runs InitSameInstrument once, well into boot.
void SiHarnessOnMemAlloc(cpu::ppc::PPCContext* ppc_context, Memory* memory,
                         uint64_t seq);

// rb3_ui_probe.cc: --rb3dx_ui_probe (read-only sampler thread).
void StartUiProbe(const TitleLaunchContext& ctx);

// rb3_autopilot.cc: --rb3dx_autoconfirm_parts (pad-injection thread).
void StartAutopilot(const TitleLaunchContext& ctx);

// rb3_scripted_input.cc: the RB3 screen reader for --scripted_input_file
// `wait_screen` directives.
void InstallScriptedInputAdapter();
void RemoveScriptedInputAdapter();

}  // namespace rb3
}  // namespace titles
}  // namespace xe

#endif  // XENIA_TITLES_RB3_RB3_INTERNAL_H_
