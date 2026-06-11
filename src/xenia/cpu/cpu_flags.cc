/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/cpu_flags.h"

DEFINE_string(cpu, "any", "CPU backend [any, x64].", "CPU");

DEFINE_string(
    load_module_map, "",
    "Loads a .map for symbol names and to diff with the generated symbol "
    "database.",
    "CPU");

DEFINE_bool(disassemble_functions, false,
            "Disassemble functions during generation.", "CPU");

DEFINE_bool(trace_functions, false, "Generate tracing for function statistics.",
            "CPU");
DEFINE_bool(trace_function_coverage, false,
            "Generate tracing for function instruction coverage statistics.",
            "CPU");
DEFINE_bool(trace_function_references, false,
            "Generate tracing for function address references.", "CPU");
DEFINE_bool(trace_function_data, false,
            "Generate tracing for function result data.", "CPU");

// milo-trace (X-track) capture-session activation. When --milo_trace_enable is
// set AND --milo_trace_out names a path, a MiloTraceBegin() session is opened at
// title launch and the GuestFunction::Call override-wrap hook (X2) emits .mtr
// records for every traced address. --milo_trace_manifest is the JSON address
// manifest (empty => trace every guest call). Mirrors the trace_functions family.
DEFINE_bool(milo_trace_enable, false,
            "milo-trace: open a capture session at title launch (X-track). "
            "Requires --milo_trace_out. Emits per-call .mtr records via the "
            "GuestFunction::Call override-wrap hook.",
            "CPU");
DEFINE_string(milo_trace_manifest, "",
              "milo-trace: path to the JSON address manifest (traced addresses "
              "+ per-addr mem windows). Empty => trace every guest call.",
              "CPU");
DEFINE_string(milo_trace_out, "",
              "milo-trace: output .mtr path. Ignored unless "
              "--milo_trace_enable=true.",
              "CPU");
// X6-capture-fix (DEEPER CAPTURE): opt-in EXACT memory capture. When set, the
// capture hook (a) widens the Tier-B pointer chase (more nodes/bytes, plus the
// ctr/lr indirect-branch-target seeds), and (b) emits a Tier-C ACCESS_LOG of the
// region windows the traced call actually touched so the read graph closes for
// member-deref setters (SetSpeed/SetPan-class) that a heuristic chase misses.
// Off by default (per-instruction-grade capture is expensive); turn on only for
// a small, gated manifest.
DEFINE_bool(milo_trace_exact_mem, false,
            "milo-trace: opt into EXACT memory capture (wider Tier-B chase + "
            "Tier-C access log). Heavier; use with a gated manifest. Requires "
            "--milo_trace_enable=true.",
            "CPU");
// X8-CE (call effects). When set together with --milo_trace_enable, the
// entry/exit capture thunks (kDebugInfoTraceCallRecords) are emitted for ALL
// compiled functions, not just the manifest-traced set (ppc_translator.cc). That
// makes FORWARD calls into un-manifested callees of a traced (FULL) frame visible
// as LIGHT frames, so the parent's CALLS[] entry can be FINALIZED with the
// callee's actual return regs (r3/f1) + memory-write delta — the per-call oracle
// effect the a2i replay path needs (W4_A2_NONLEAF §2 found ret=0/wr=0 forward
// calls under X7). EXACT-mem per-access logging stays manifest-gated (the
// expensive part is unaffected). With this OFF, translation is byte-identical to
// X7 (the negative control). Doc: docs/W5_X8CE_CALL_EFFECTS.md.
DEFINE_bool(milo_trace_call_effects, false,
            "milo-trace (X8-CE): emit capture thunks for ALL functions so a "
            "traced frame's forward calls get their return regs + write delta "
            "captured into the parent's CALLS[] oracle. Requires "
            "--milo_trace_enable=true. OFF => byte-identical X7 translation.",
            "CPU");

DEFINE_bool(
    disable_global_lock, false,
    "Disables global lock usage in guest code. Does not affect host code.",
    "CPU");

DEFINE_bool(validate_hir, false,
            "Perform validation checks on the HIR during compilation.", "CPU");

DEFINE_uint64(
    pvr, 0x710700,
    "Processor version and revision number.\nBits 0 to 15 are the version "
    "number.\nBits 16 to 31 are the revision number.\nNote: Some XEXs (such as "
    "mfgbootlauncher.xex) may check for a value that's less than 0x710700.",
    "CPU");

// Breakpoints:
DEFINE_uint64(break_on_instruction, 0,
              "int3 before the given guest address is executed.", "CPU");
DEFINE_int32(break_condition_gpr, -1, "GPR compared to", "CPU");
DEFINE_uint64(break_condition_value, 0, "value compared against", "CPU");
DEFINE_string(break_condition_op, "eq", "comparison operator", "CPU");
DEFINE_bool(break_condition_truncate, true, "truncate value to 32-bits", "CPU");

DEFINE_bool(break_on_debugbreak, true, "int3 on JITed __debugbreak requests.",
            "CPU");

DEFINE_string(pe_override, "",
              "Path to a PE file to override the loaded XEX image. The PE "
              "sections will be copied over the original XEX's PE content.",
              "CPU");
