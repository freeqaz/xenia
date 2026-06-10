/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * milo-trace X-track capture emitter (NOT upstream xenia).
 *
 * Emits per-function-call milo-trace records (.mtr) from the PPC->x64 JIT for
 * DC3 (validation ground truth) and rb3-xenon (discovery). Records are the
 * FROZEN milo-trace T1 format (mtr_format.h, vendored verbatim from the
 * milo-trace repo). The Python codec (milo-trace/milo_trace/trace_format/
 * codec.py) reads back the exact bytes this TU writes.
 *
 * DAG task: X1 (MiloTrace TU: thunks + binary sink + manifest parse).
 * Companion seams (separate downstream tasks, see patches/xenia/INSERTION_POINTS):
 *   X2  function.cc Call hook (override-wrap prototype) calls OnEntry/OnExit.
 *   X5  cpu_flags + kDebugInfoTraceCallRecords + translator plumbing.
 *   X6  x64_emitter prologue/epilog CallNative(OnEntry/OnExit), addr-gated.
 *
 * This TU is self-contained: it defines the thunks, the per-thread binary
 * record buffers, the file sink (std::ofstream, NOT ChunkedMappedMemoryWriter —
 * that path is null on Linux), the lifecycle (Begin/End, modelled on
 * dc3_runtime_telemetry.cc), and the JSON manifest parser. Wiring it into the
 * JIT/override path is X2/X6.
 ******************************************************************************
 */

#ifndef XENIA_CPU_MILO_TRACE_H_
#define XENIA_CPU_MILO_TRACE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

namespace xe {
namespace cpu {

// Per-address capture configuration parsed from the JSON manifest.
//
// `mem_window_back`/`mem_window_fwd` size the Tier-A stack window relative to r1
// ([r1 - back, r1 + fwd)). Defaults mirror the frozen schema (§2.3 Tier A:
// [r1-256, r1+512)). `exact_mem` opts a function into the Method-B exact write
// log (X7); off => Method-A snapshot-diff windows.
struct MiloTraceAddrConfig {
  uint32_t mem_window_back = 256;
  uint32_t mem_window_fwd = 512;
  uint32_t arg_chase_len = 256;  // bytes captured per pointer-arg node (Tier B)
  bool exact_mem = false;
};

// Session-wide config (mirrors Dc3RuntimeTelemetryConfig in spirit).
struct MiloTraceConfig {
  std::string title_id;       // e.g. "545107D5" — provenance only
  std::string out_path;       // .mtr output path
  std::string manifest_path;  // JSON manifest of traced addresses (may be empty)
  std::string target_sha1;    // hex sha1 of the xex (provenance; may be empty)
  int arch = 2;               // mtr_arch_t: 1=gekko, 2=xenon (Xenia => xenon)
  int capture_method = 3;     // mtr_capture_method_t: 3=xenia_override, 2=jit
  // X6-capture-fix (DEEPER CAPTURE): session-wide opt-in to EXACT memory capture
  // (wider Tier-B chase + Tier-C ACCESS_LOG). Off by default; set from
  // --milo_trace_exact_mem. A per-addr manifest "exact_mem" still overrides per
  // function, but this makes EVERY traced address exact when set.
  bool exact_mem = false;
};

// ---------------------------------------------------------------------------
// Lifecycle (model: dc3_runtime_telemetry.cc Begin/End/IsActive).
// ---------------------------------------------------------------------------

// Opens the .mtr sink, writes the file header, loads the manifest. No-op (and
// leaves IsActive() false) if out_path is empty.
void MiloTraceBegin(const MiloTraceConfig& config);

// Flushes all per-thread buffers and closes the sink. `reason` is recorded as a
// NOTE in a trailing diagnostic record.
void MiloTraceEnd(const char* reason);

bool MiloTraceIsActive();

// True iff `start_addr` is in the traced-address set (or the set is empty, in
// which case ALL addresses are traced — useful for the override-wrap proto where
// the override list is the gate). Cheap; safe to call from the JIT prologue gate.
bool MiloTraceShouldTrace(uint32_t start_addr);

// Returns the per-address config (or the session default) for `start_addr`.
MiloTraceAddrConfig MiloTraceConfigFor(uint32_t start_addr);

// ---------------------------------------------------------------------------
// The capture thunks. CallNative ABI: first arg is `void* raw_context` which is
// `ThreadState**` (x64_tracers.cc:55 pattern); second arg is the traced
// function's guest entry VA (mov rdx,<addr> at the emit site). The override-wrap
// path (X2) passes a PPCContext* wrapped to look like raw_context — see the .cc.
// ---------------------------------------------------------------------------

// Snapshot entry state: full reg file + Tier-A stack window + Tier-B pointer
// chase from arg regs. Pushes a pending record onto the thread's shadow stack.
void MiloTraceOnEntry(void* raw_context, uint64_t start_addr);

// Snapshot exit state: full reg file + re-read captured windows -> mem_out
// write delta. Pops the pending record, finishes it, appends to the buffer.
// This is the TRUE-epilog exit (a real guest return — regs_out is the
// post-function state). ENTRY-PIN: pairs with the prologue entry hook so the
// record's entry/exit are pinned to the real prologue/epilog.
void MiloTraceOnExit(void* raw_context, uint64_t start_addr);

// X6-capture-fix (ENTRY-PIN): the TAIL-call exit. The traced frame jumps away
// mid-function (a `b`/`bctr` tail), so regs_out is the args-to-tail-callee state,
// NOT a clean return. Finalizes the record like OnExit but tags it as a tail
// handoff (a `TAIL:` NOTE) so the replay consumer does not match regs_out as a
// return. Same shadow-stack pop semantics as OnExit.
void MiloTraceOnExitTail(void* raw_context, uint64_t start_addr);

// X6-capture-fix (DEEPER CAPTURE / Tier C): per-load/store memory-access logger.
// Called from the JIT load/store sequences (gated by the per-function exact-mem
// flag, so off-path code is byte-identical) with the resolved guest address,
// access size in bytes, and is_write (0=read, 1=write). Appends to the top
// pending record's ACCESS_LOG when that record is in exact_mem mode. raw_context
// is the JIT context register (a PPCContext* whose first qword is ThreadState*),
// but this thunk does not need it (kept for ABI uniformity with TraceMemory*).
void MiloTraceMemAccess(void* raw_context, uint32_t address, uint32_t size,
                        uint32_t is_write);

// Override-wrap convenience (X2): the GuestFunction::Call hook holds a
// PPCContext* directly, not a raw_context (ThreadState**). These take the
// context as an opaque pointer (the .cc casts it to ppc::PPCContext*) so callers
// can wire the hook without pulling ppc_context.h into the header. Pass
// thread_state->context().
void MiloTraceOnEntryCtx(void* ppc_context, uint32_t start_addr);
void MiloTraceOnExitCtx(void* ppc_context, uint32_t start_addr);

// ---------------------------------------------------------------------------
// Manifest parser (exposed for testing + reuse from Processor::PreLaunch / X6).
// Parses the milo-trace manifest JSON (schema in docs/research/02-xenia-capture
// §4.1) into an address set + per-addr config map. Returns the number of
// addresses parsed; -1 on parse failure.
// ---------------------------------------------------------------------------
int MiloTraceLoadManifest(std::string_view path);

// Direct access to the traced-address set (e.g. to plumb into the X64Emitter).
const std::unordered_set<uint32_t>& MiloTraceTracedAddresses();

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_MILO_TRACE_H_
