#!/usr/bin/env bash
set -euo pipefail

XENIA_DIR="${XENIA_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
BIN="${XENIA_BIN:-$XENIA_DIR/build/bin/Linux/Debug/xenia-headless}"

TRACE_XEX="${DC3_TRACE_XEX:-/home/free/code/milohax/dc3-decomp/build/373307D9/default.xex}"
TRACE_BREAK_PC="${DC3_TRACE_BREAK_PC:-}"
TRACE_TIMEOUT_SECS="${DC3_TRACE_TIMEOUT_SECS:-30}"
HEADLESS_TIMEOUT_MS="${DC3_TRACE_HEADLESS_TIMEOUT_MS:-15000}"
TRACE_TMPDIR="${DC3_TRACE_TMPDIR:-/tmp/xenia_dc3_trace_on_break}"

TRACE_GPU="${DC3_TRACE_GPU:-null}"
BREAK_ON_UNIMPL="${DC3_TRACE_BREAK_ON_UNIMPL:-0}"
ENABLE_TELEMETRY="${DC3_TRACE_ENABLE_TELEMETRY:-1}"
ENABLE_DISASSEMBLE_FUNCTIONS="${DC3_TRACE_DISASSEMBLE_FUNCTIONS:-0}"
ENABLE_TRACE_FUNCTIONS="${DC3_TRACE_TRACE_FUNCTIONS:-0}"
ENABLE_TRACE_FUNCTION_COVERAGE="${DC3_TRACE_TRACE_FUNCTION_COVERAGE:-0}"
ENABLE_TRACE_FUNCTION_REFERENCES="${DC3_TRACE_TRACE_FUNCTION_REFERENCES:-0}"
ENABLE_TRACE_FUNCTION_DATA="${DC3_TRACE_TRACE_FUNCTION_DATA:-0}"

NUI_MANIFEST_PATH="${DC3_TRACE_MANIFEST_PATH:-${DC3_PARITY_MANIFEST_PATH:-}}"
NUI_SYMBOL_MAP_PATH="${DC3_TRACE_SYMBOL_MAP_PATH:-${DC3_PARITY_SYMBOL_MAP_PATH:-}}"

GUEST_DISASM_TOOL="${DC3_TRACE_GUEST_DISASM_TOOL:-$XENIA_DIR/tools/dc3_guest_disasm.py}"
TRACE_SYMBOLS_PATH="${DC3_TRACE_SYMBOLS_PATH:-$NUI_SYMBOL_MAP_PATH}"
TRACE_GUEST_DISASM_BEFORE="${DC3_TRACE_GUEST_DISASM_BEFORE:-10}"
TRACE_GUEST_DISASM_AFTER="${DC3_TRACE_GUEST_DISASM_AFTER:-20}"

normalize_bool() {
  case "${1,,}" in
    1|true|yes|on) echo "true" ;;
    0|false|no|off) echo "false" ;;
    *)
      echo "error: invalid boolean value '$1' (expected 0/1/true/false)" >&2
      return 1
      ;;
  esac
}

BREAK_ON_UNIMPL_BOOL="$(normalize_bool "$BREAK_ON_UNIMPL")" || exit 2
ENABLE_TELEMETRY_BOOL="$(normalize_bool "$ENABLE_TELEMETRY")" || exit 2
ENABLE_DISASSEMBLE_FUNCTIONS_BOOL="$(normalize_bool "$ENABLE_DISASSEMBLE_FUNCTIONS")" || exit 2
ENABLE_TRACE_FUNCTIONS_BOOL="$(normalize_bool "$ENABLE_TRACE_FUNCTIONS")" || exit 2
ENABLE_TRACE_FUNCTION_COVERAGE_BOOL="$(normalize_bool "$ENABLE_TRACE_FUNCTION_COVERAGE")" || exit 2
ENABLE_TRACE_FUNCTION_REFERENCES_BOOL="$(normalize_bool "$ENABLE_TRACE_FUNCTION_REFERENCES")" || exit 2
ENABLE_TRACE_FUNCTION_DATA_BOOL="$(normalize_bool "$ENABLE_TRACE_FUNCTION_DATA")" || exit 2

if [[ -z "$TRACE_BREAK_PC" ]]; then
  echo "error: set DC3_TRACE_BREAK_PC (guest address, e.g. 0x835B3D5C)" >&2
  exit 2
fi
if [[ ! -x "$BIN" ]]; then
  echo "error: xenia-headless not found at $BIN" >&2
  exit 1
fi
if [[ ! -f "$TRACE_XEX" ]]; then
  echo "error: target XEX not found: $TRACE_XEX" >&2
  exit 1
fi

mkdir -p "$TRACE_TMPDIR"
LOGFILE="$TRACE_TMPDIR/run.log"
TELEMETRY_JSONL="$TRACE_TMPDIR/runtime_telemetry.jsonl"
TRACE_DATA_BIN="$TRACE_TMPDIR/function_trace_data.bin"
BREAK_DISASM_TXT="$TRACE_TMPDIR/break_guest_disasm.txt"
CRASH_DISASM_TXT="$TRACE_TMPDIR/crash_guest_disasm.txt"
TRACER_LINES_TXT="$TRACE_TMPDIR/x64_trace_lines.log"
SUMMARY_TXT="$TRACE_TMPDIR/summary.txt"

rm -f "$LOGFILE" "$TELEMETRY_JSONL" "$TRACE_DATA_BIN" "$BREAK_DISASM_TXT" "$CRASH_DISASM_TXT" \
  "$TRACER_LINES_TXT" "$SUMMARY_TXT"

args=(
  --gpu="$TRACE_GPU"
  --target="$TRACE_XEX"
  --headless_timeout_ms="$HEADLESS_TIMEOUT_MS"
  --break_on_instruction="$TRACE_BREAK_PC"
  --break_on_unimplemented_instructions="$BREAK_ON_UNIMPL_BOOL"
  --disassemble_functions="$ENABLE_DISASSEMBLE_FUNCTIONS_BOOL"
  --trace_functions="$ENABLE_TRACE_FUNCTIONS_BOOL"
  --trace_function_coverage="$ENABLE_TRACE_FUNCTION_COVERAGE_BOOL"
  --trace_function_references="$ENABLE_TRACE_FUNCTION_REFERENCES_BOOL"
  --trace_function_data="$ENABLE_TRACE_FUNCTION_DATA_BOOL"
)

if [[ "$ENABLE_TRACE_FUNCTION_DATA_BOOL" == "true" ]]; then
  args+=(--trace_function_data_path="$TRACE_DATA_BIN")
fi
if [[ "$ENABLE_TELEMETRY_BOOL" == "true" ]]; then
  args+=(--dc3_runtime_telemetry_enable=true --dc3_runtime_telemetry_path="$TELEMETRY_JSONL")
fi
if [[ -n "$NUI_MANIFEST_PATH" && -f "$NUI_MANIFEST_PATH" ]]; then
  args+=(--dc3_nui_patch_manifest_path="$NUI_MANIFEST_PATH")
fi
if [[ -n "$NUI_SYMBOL_MAP_PATH" && -f "$NUI_SYMBOL_MAP_PATH" ]]; then
  args+=(--dc3_nui_symbol_map_path="$NUI_SYMBOL_MAP_PATH")
fi

echo "== DC3 trace-on-break headless run =="
echo "BIN:   $BIN"
echo "XEX:   $TRACE_XEX"
echo "BREAK: $TRACE_BREAK_PC"
echo "TMP:   $TRACE_TMPDIR"

set +e
timeout "${TRACE_TIMEOUT_SECS}s" "$BIN" "${args[@]}" >"$LOGFILE" 2>&1
RC=$?
set -e

{
  echo "rc=$RC"
  echo "log=$LOGFILE"
  echo "telemetry=$TELEMETRY_JSONL"
  echo "trace_data=$TRACE_DATA_BIN"
} >"$SUMMARY_TXT"

echo "Run complete: rc=$RC (124 means timeout wrapper fired; nonzero may also indicate breakpoint/assert/crash)"
echo "Log: $LOGFILE"
[[ -f "$TELEMETRY_JSONL" ]] && echo "Telemetry: $TELEMETRY_JSONL"
[[ -f "$TRACE_DATA_BIN" ]] && [[ -s "$TRACE_DATA_BIN" ]] && echo "Trace data: $TRACE_DATA_BIN"

if rg -n '^[td]!?>|^t>' "$LOGFILE" >/dev/null 2>&1; then
  # Existing x64 tracer plumbing logs with the 't' channel when compiled/active.
  rg -n 't>' "$LOGFILE" >"$TRACER_LINES_TXT" || true
  echo "Extracted x64 tracer lines: $TRACER_LINES_TXT"
fi

if [[ -f "$GUEST_DISASM_TOOL" ]]; then
  disasm_common=(
    --image "$TRACE_XEX"
    --before "$TRACE_GUEST_DISASM_BEFORE"
    --after "$TRACE_GUEST_DISASM_AFTER"
  )
  if [[ -n "$TRACE_SYMBOLS_PATH" && -f "$TRACE_SYMBOLS_PATH" ]]; then
    disasm_common+=(--symbols "$TRACE_SYMBOLS_PATH")
  fi

  python3 "$GUEST_DISASM_TOOL" "${disasm_common[@]}" --pc "$TRACE_BREAK_PC" \
    >"$BREAK_DISASM_TXT" 2>&1 || true
  [[ -f "$BREAK_DISASM_TXT" ]] && echo "Break disasm: $BREAK_DISASM_TXT"

  python3 "$GUEST_DISASM_TOOL" "${disasm_common[@]}" --xenia-log "$LOGFILE" \
    >"$CRASH_DISASM_TXT" 2>&1 || true
  [[ -s "$CRASH_DISASM_TXT" ]] && echo "Crash disasm: $CRASH_DISASM_TXT"
fi

if [[ -f "$TELEMETRY_JSONL" ]]; then
  python3 - "$TELEMETRY_JSONL" <<'PY' | tee -a "$SUMMARY_TXT"
import json, sys
from collections import Counter
from pathlib import Path

p = Path(sys.argv[1])
events = []
counts = Counter()
summary = None
for line in p.read_text(encoding="utf-8", errors="ignore").splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        e = json.loads(line)
    except json.JSONDecodeError:
        continue
    events.append(e)
    et = e.get("event", "<unknown>")
    counts[et] += 1
    if et == "dc3_summary":
        summary = e

print(f"telemetry_events={len(events)}")
for key in ("dc3_boot_milestone", "dc3_unresolved_call_stub_hit", "dc3_hot_loop_pc", "dc3_summary"):
    print(f"{key}={counts.get(key, 0)}")
if summary:
    print("summary_reason=" + str(summary.get("reason")))
    print("summary_total_unresolved=" + str(summary.get("total_unresolved_stub_hits")))
    print("summary_total_hot_loops=" + str(summary.get("total_hot_loop_samples")))
PY
fi

echo "Artifacts ready in: $TRACE_TMPDIR"
echo "Suggested follow-up:"
echo "  1) inspect $LOGFILE (break/crash path)"
echo "  2) inspect $BREAK_DISASM_TXT and $CRASH_DISASM_TXT"
echo "  3) if tracer lines exist, inspect $TRACER_LINES_TXT"

# Preserve xenia/timeout return code so callers can gate on it.
exit "$RC"
