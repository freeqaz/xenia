#!/usr/bin/env bash
set -euo pipefail

XENIA_DIR="${XENIA_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
BIN="${XENIA_BIN:-$XENIA_DIR/build/bin/Linux/Debug/xenia-headless}"
DECOMP_XEX="${DC3_DECOMP_XEX:-/home/free/code/milohax/dc3-decomp/build/373307D9/default.xex}"
ORIG_XEX="${DC3_ORIG_XEX:-/home/free/code/milohax/dc3-decomp/orig/373307D9/default.xex}"
TIMEOUT_SECS="${DC3_GATE_TIMEOUT_SECS:-25}"
HEADLESS_TIMEOUT_MS="${DC3_GATE_HEADLESS_TIMEOUT_MS:-15000}"
TMPDIR_GATE="${DC3_PARITY_TMPDIR:-/tmp/xenia_dc3_parity_gate}"
PARITY_MODE="${DC3_PARITY_MODE:-hybrid}"
REQUIRE_EQUAL_MILESTONES="${DC3_PARITY_REQUIRE_EQUAL_MILESTONES:-0}"
FAIL_ON_NEW_HOT_LOOP="${DC3_PARITY_FAIL_ON_NEW_HOT_LOOP:-0}"
COMMON_MANIFEST_PATH="${DC3_PARITY_MANIFEST_PATH:-}"
COMMON_SYMBOL_MAP_PATH="${DC3_PARITY_SYMBOL_MAP_PATH:-}"
ORIG_MANIFEST_PATH="${DC3_ORIG_MANIFEST_PATH:-$COMMON_MANIFEST_PATH}"
DECOMP_MANIFEST_PATH="${DC3_DECOMP_MANIFEST_PATH:-$COMMON_MANIFEST_PATH}"
ORIG_SYMBOL_MAP_PATH="${DC3_ORIG_SYMBOL_MAP_PATH:-$COMMON_SYMBOL_MAP_PATH}"
DECOMP_SYMBOL_MAP_PATH="${DC3_DECOMP_SYMBOL_MAP_PATH:-$COMMON_SYMBOL_MAP_PATH}"
TELEMETRY_DIFF_TOOL="${DC3_PARITY_TELEMETRY_DIFF_TOOL:-$XENIA_DIR/tools/dc3_runtime_telemetry_diff.py}"
TELEMETRY_DIFF_TOP="${DC3_PARITY_TELEMETRY_DIFF_TOP:-15}"
PARITY_SYMBOLIZE="${DC3_PARITY_SYMBOLIZE:-0}"
PARITY_TRIAGE="${DC3_PARITY_TRIAGE:-1}"
GUEST_DISASM_TOOL="${DC3_PARITY_GUEST_DISASM_TOOL:-$XENIA_DIR/tools/dc3_guest_disasm.py}"
CRASH_TRIAGE_TOOL="${DC3_PARITY_CRASH_TRIAGE_TOOL:-$XENIA_DIR/tools/dc3_crash_signature_triage.py}"
GUEST_DISASM_BEFORE="${DC3_PARITY_GUEST_DISASM_BEFORE:-8}"
GUEST_DISASM_AFTER="${DC3_PARITY_GUEST_DISASM_AFTER:-16}"
MILESTONE_POLICY="${DC3_PARITY_MILESTONE_POLICY:-warn}"
MILESTONE_CONTRACT="${DC3_PARITY_MILESTONE_CONTRACT:-session_begin,dc3_nui_patch_block_begin,dc3_nui_patch_apply_complete,dc3_hack_pack_apply_complete,headless_timeout_reached}"
FAIL_ON_STALE_MANIFEST="${DC3_PARITY_FAIL_ON_STALE_MANIFEST:-0}"
FAIL_ON_MANIFEST_FP_MISMATCH="${DC3_PARITY_FAIL_ON_MANIFEST_FP_MISMATCH:-0}"
BREAK_ON_UNIMPL="${DC3_PARITY_BREAK_ON_UNIMPL:-1}"

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
PARITY_SYMBOLIZE_BOOL="$(normalize_bool "$PARITY_SYMBOLIZE")" || exit 2
PARITY_TRIAGE_BOOL="$(normalize_bool "$PARITY_TRIAGE")" || exit 2

mkdir -p "$TMPDIR_GATE"

maybe_emit_crash_disasm() {
  local case_name="$1" image_path="$2" symbols_path="$3"
  local logfile="$TMPDIR_GATE/${case_name}.log"
  local artifact="$TMPDIR_GATE/${case_name}_crash_disasm.txt"
  if [[ "$PARITY_SYMBOLIZE_BOOL" != "true" ]]; then
    return 0
  fi
  if [[ ! -f "$logfile" ]]; then
    return 0
  fi
  if ! rg -q "==== CRASH DUMP ====" "$logfile"; then
    printf "No crash dump found in %s; crash disasm skipped.\n" "$logfile" >"$artifact"
    echo "Crash disasm artifact ($case_name): $artifact (no crash dump in log)"
    return 0
  fi
  if [[ ! -f "$image_path" ]]; then
    echo "WARN: $case_name crash disasm skipped (image missing: $image_path)"
    return 0
  fi
  if [[ ! -f "$GUEST_DISASM_TOOL" ]]; then
    echo "WARN: $case_name crash disasm skipped (tool missing: $GUEST_DISASM_TOOL)"
    return 0
  fi
  local -a disasm_args=(
    --image "$image_path"
    --xenia-log "$logfile"
    --before "$GUEST_DISASM_BEFORE"
    --after "$GUEST_DISASM_AFTER"
  )
  if [[ -n "$symbols_path" && -f "$symbols_path" ]]; then
    disasm_args+=(--symbols "$symbols_path")
  fi
  if python3 "$GUEST_DISASM_TOOL" "${disasm_args[@]}" >"$artifact" 2>&1; then
    echo "Crash disasm artifact ($case_name): $artifact"
  else
    echo "WARN: $case_name crash disasm helper failed (see $artifact)"
  fi
}

maybe_emit_symbolized_telemetry_diff_artifact() {
  local artifact="$TMPDIR_GATE/telemetry_diff_symbolized.txt"
  if [[ "$PARITY_SYMBOLIZE_BOOL" != "true" ]]; then
    return 0
  fi
  if [[ ! -f "$TMPDIR_GATE/orig.jsonl" || ! -f "$TMPDIR_GATE/decomp.jsonl" ]]; then
    return 0
  fi
  if [[ ! -f "$TELEMETRY_DIFF_TOOL" && "$TELEMETRY_DIFF_TOOL" == /* ]]; then
    echo "WARN: telemetry diff artifact skipped (tool missing: $TELEMETRY_DIFF_TOOL)"
    return 0
  fi
  local -a diff_args=(
    --orig "$TMPDIR_GATE/orig.jsonl"
    --decomp "$TMPDIR_GATE/decomp.jsonl"
    --top "$TELEMETRY_DIFF_TOP"
  )
  if [[ -n "$ORIG_SYMBOL_MAP_PATH" && -f "$ORIG_SYMBOL_MAP_PATH" ]]; then
    diff_args+=(--orig-symbols "$ORIG_SYMBOL_MAP_PATH")
  fi
  if [[ -n "$DECOMP_SYMBOL_MAP_PATH" && -f "$DECOMP_SYMBOL_MAP_PATH" ]]; then
    diff_args+=(--decomp-symbols "$DECOMP_SYMBOL_MAP_PATH")
  fi
  if python3 "$TELEMETRY_DIFF_TOOL" "${diff_args[@]}" >"$artifact" 2>&1; then
    echo "Telemetry diff artifact: $artifact"
  else
    echo "WARN: telemetry diff artifact generation failed (see $artifact)"
  fi
}

maybe_emit_crash_triage_artifact() {
  local case_name="$1" image_path="$2" symbols_path="$3"
  local logfile="$TMPDIR_GATE/${case_name}.log"
  local txt_artifact="$TMPDIR_GATE/${case_name}_crash_triage.txt"
  local json_artifact="$TMPDIR_GATE/${case_name}_crash_triage.json"
  if [[ "$PARITY_TRIAGE_BOOL" != "true" ]]; then
    return 0
  fi
  if [[ ! -f "$logfile" ]]; then
    return 0
  fi
  if [[ ! -f "$CRASH_TRIAGE_TOOL" ]]; then
    echo "WARN: $case_name crash triage skipped (tool missing: $CRASH_TRIAGE_TOOL)"
    return 0
  fi
  local -a triage_args=(--log "$logfile" --allow-no-crash --json-out "$json_artifact")
  if [[ -f "$image_path" ]]; then
    triage_args+=(--image "$image_path")
  fi
  if [[ -n "$symbols_path" && -f "$symbols_path" ]]; then
    triage_args+=(--symbols "$symbols_path")
  fi
  if python3 "$CRASH_TRIAGE_TOOL" "${triage_args[@]}" >"$txt_artifact" 2>&1; then
    echo "Crash triage artifact ($case_name): $txt_artifact"
  else
    echo "WARN: $case_name crash triage failed (see $txt_artifact)"
  fi
}

emit_symbolized_artifacts() {
  maybe_emit_crash_disasm orig "$ORIG_XEX" "$ORIG_SYMBOL_MAP_PATH"
  maybe_emit_crash_disasm decomp "$DECOMP_XEX" "$DECOMP_SYMBOL_MAP_PATH"
  maybe_emit_crash_triage_artifact orig "$ORIG_XEX" "$ORIG_SYMBOL_MAP_PATH"
  maybe_emit_crash_triage_artifact decomp "$DECOMP_XEX" "$DECOMP_SYMBOL_MAP_PATH"
  maybe_emit_symbolized_telemetry_diff_artifact
}

parity_gate_on_exit() {
  local rc=$?
  if [[ $rc -ne 0 && ( "$PARITY_SYMBOLIZE_BOOL" == "true" || "$PARITY_TRIAGE_BOOL" == "true" ) ]]; then
    set +e
    echo "Parity gate failed (rc=$rc); emitting failure artifacts into $TMPDIR_GATE"
    emit_symbolized_artifacts
  fi
  return "$rc"
}

trap parity_gate_on_exit EXIT

if [[ ! -x "$BIN" ]]; then
  echo "error: xenia-headless not found at $BIN" >&2
  exit 1
fi
if [[ "$PARITY_MODE" != "hybrid" && "$PARITY_MODE" != "strict" ]]; then
  echo "error: DC3_PARITY_MODE must be hybrid or strict (got '$PARITY_MODE')" >&2
  exit 1
fi

preflight_case_inputs() {
  local case_name="$1" xex="$2" expected_layout="$3" manifest_path="$4" symbol_map_path="$5"
  if [[ ! -f "$xex" ]]; then
    echo "FAIL: $case_name missing XEX: $xex" >&2
    return 1
  fi
  if [[ "$PARITY_MODE" == "strict" ]]; then
    return 0
  fi
  if [[ -n "$manifest_path" && ! -f "$manifest_path" ]]; then
    echo "FAIL: $case_name missing manifest: $manifest_path" >&2
    return 1
  fi
  if [[ -n "$symbol_map_path" && ! -f "$symbol_map_path" ]]; then
    echo "FAIL: $case_name missing symbol map: $symbol_map_path" >&2
    return 1
  fi
  if [[ -z "$manifest_path" ]]; then
    return 0
  fi

  python3 - "$case_name" "$xex" "$expected_layout" "$manifest_path" "$symbol_map_path" "$FAIL_ON_STALE_MANIFEST" <<'PY'
import json, os, sys
from pathlib import Path

case_name, xex, expected_layout, manifest, symbols, fail_stale = sys.argv[1:]
fail_stale = fail_stale == "1"
xex_p = Path(xex)
man_p = Path(manifest)
sym_p = Path(symbols) if symbols else None

errs = []
warns = []
try:
    obj = json.loads(man_p.read_text(encoding="utf-8"))
except Exception as e:
    errs.append(f"{case_name}: manifest parse failed: {e}")
    obj = None

if obj is not None:
    if obj.get("schema") != "xenia.dc3.nui_patch_manifest":
        errs.append(f"{case_name}: manifest schema={obj.get('schema')!r} unexpected")
    ver = obj.get("schema_version", obj.get("format_version"))
    if not isinstance(ver, int) or ver < 1:
        errs.append(f"{case_name}: manifest schema_version/format_version invalid: {ver!r}")
    targets = obj.get("targets")
    if not isinstance(targets, dict) or not targets:
        errs.append(f"{case_name}: manifest targets missing/empty")
    build_label = obj.get("build_label")
    if build_label and str(build_label) != expected_layout:
        warns.append(f"{case_name}: manifest build_label={build_label!r} expected {expected_layout!r}")
    text = (((obj.get("pe") or {}).get("text")) or {})
    if "xenia_runtime_fnv1a64" not in text:
        warns.append(f"{case_name}: manifest missing pe.text.xenia_runtime_fnv1a64 (runtime mismatch detection will rely on log)")

try:
    xex_m = xex_p.stat().st_mtime
    man_m = man_p.stat().st_mtime
    if xex_m - man_m > 1.0:
        msg = f"{case_name}: manifest older than XEX by {xex_m-man_m:.1f}s (possible stale manifest)"
        (errs if fail_stale else warns).append(msg)
    if sym_p and sym_p.exists():
        sym_m = sym_p.stat().st_mtime
        if sym_m - man_m > 1.0:
            warns.append(f"{case_name}: symbols newer than manifest by {sym_m-man_m:.1f}s (manifest may be stale)")
except OSError as e:
    warns.append(f"{case_name}: mtime preflight skipped ({e})")

for w in warns:
    print("WARN:", w)
if errs:
    for e in errs:
        print("FAIL:", e, file=sys.stderr)
    sys.exit(1)
PY
}

run_case() {
  local case_name="$1" xex="$2" expected_total="$3" expected_layout="$4" manifest_path="$5" symbol_map_path="$6"
  local logfile="$TMPDIR_GATE/${case_name}.log"
  local telefile="$TMPDIR_GATE/${case_name}.jsonl"
  rm -f "$logfile" "$telefile"

  local -a args=(
    --gpu=null
    --stub_nui_functions=true
    --break_on_unimplemented_instructions="$BREAK_ON_UNIMPL_BOOL"
    --target="$xex"
    --headless_timeout_ms="$HEADLESS_TIMEOUT_MS"
    --dc3_runtime_telemetry_enable=true
    --dc3_runtime_telemetry_path="$telefile"
  )
  if [[ "$PARITY_MODE" == "strict" ]]; then
    args+=(
      --dc3_nui_patch_resolver_mode=strict
      --dc3_nui_enable_signature_resolver=true
      --dc3_nui_patch_manifest_path=/tmp/does_not_exist_manifest.json
      --dc3_nui_symbol_map_path=/tmp/does_not_exist_symbols.txt
    )
  else
    if [[ -n "$manifest_path" ]]; then
      args+=(--dc3_nui_patch_manifest_path="$manifest_path")
    fi
    if [[ -n "$symbol_map_path" ]]; then
      args+=(--dc3_nui_symbol_map_path="$symbol_map_path")
    fi
  fi

  echo "== $case_name ($PARITY_MODE) =="
  set +e
  timeout "${TIMEOUT_SECS}s" "$BIN" "${args[@]}" >"$logfile" 2>&1
  local rc=$?
  set -e
  if [[ $rc -ne 0 && $rc -ne 124 ]]; then
    echo "FAIL: $case_name exited rc=$rc" >&2
    tail -80 "$logfile" >&2 || true
    return 1
  fi

  python3 - "$logfile" "$telefile" "$case_name" "$expected_total" "$expected_layout" "$PARITY_MODE" "$manifest_path" "$FAIL_ON_MANIFEST_FP_MISMATCH" <<'PY'
import json, re, sys
from pathlib import Path

log_path = Path(sys.argv[1])
tele_path = Path(sys.argv[2])
case_name = sys.argv[3]
expected_total = int(sys.argv[4])
expected_layout = sys.argv[5]
expected_mode = sys.argv[6]
manifest_path = sys.argv[7]
fail_on_fp_mismatch = sys.argv[8] == "1"
log = log_path.read_text(errors="ignore")

summary_re = re.compile(r"DC3: NUI resolver summary mode=(\w+) .* signature_hits=(\d+) .* strict_rejects=(\d+) total=(\d+)")
apply_re = re.compile(r"DC3: NUI/XBC apply path guest_overrides=(\d+) resolver_mode=(\w+) signature_resolver=(\d+)")
patch_re = re.compile(r"DC3: NUI patch/override summary: patched=(\d+) overridden=(\d+) skipped=(\d+) total=(\d+) layout=(\w+).*")
reg_re = re.compile(r"DC3: Registered (\d+) guest extern overrides from NUI patch table")
manifest_fp_disable_re = re.compile(r"Disabling patch manifest target resolution due fingerprint mismatch")

m_apply = apply_re.search(log)
m_summary = summary_re.search(log)
m_patch = patch_re.search(log)
if not (m_apply and m_summary and m_patch):
    print(f"FAIL {case_name}: missing expected DC3 summary lines", file=sys.stderr)
    sys.exit(1)
if "TIMEOUT:" not in log:
    print(f"FAIL {case_name}: missing TIMEOUT marker", file=sys.stderr)
    sys.exit(1)

guest_overrides = int(m_apply.group(1))
apply_mode = m_apply.group(2)
sig_flag = int(m_apply.group(3))
resolver_mode = m_summary.group(1)
sig_hits = int(m_summary.group(2))
strict_rejects = int(m_summary.group(3))
resolver_total = int(m_summary.group(4))
patched = int(m_patch.group(1))
overridden = int(m_patch.group(2))
skipped = int(m_patch.group(3))
patch_total = int(m_patch.group(4))
layout = m_patch.group(5)
reg_count = int(m_reg.group(1)) if (m_reg := reg_re.search(log)) else 0

errs = []
if guest_overrides != 1: errs.append(f"guest_overrides={guest_overrides} expected 1")
if apply_mode != expected_mode: errs.append(f"apply resolver_mode={apply_mode} expected {expected_mode}")
if resolver_mode != expected_mode: errs.append(f"resolver summary mode={resolver_mode} expected {expected_mode}")
if resolver_total != expected_total: errs.append(f"resolver total={resolver_total} expected {expected_total}")
if patch_total != expected_total: errs.append(f"patch total={patch_total} expected {expected_total}")
if layout != expected_layout: errs.append(f"layout={layout} expected {expected_layout}")
if patched != 0: errs.append(f"patched={patched} expected 0")
if overridden != expected_total: errs.append(f"overridden={overridden} expected {expected_total}")
if skipped != 0: errs.append(f"skipped={skipped} expected 0")
if reg_count != expected_total: errs.append(f"registered={reg_count} expected {expected_total}")
if expected_mode == "strict":
    if sig_flag != 1: errs.append(f"signature_resolver={sig_flag} expected 1")
    if sig_hits != expected_total or strict_rejects != 0:
        errs.append(f"strict signature coverage not full: hits={sig_hits} rejects={strict_rejects}")

if expected_mode != "strict" and manifest_path:
    if manifest_fp_disable_re.search(log):
        msg = "manifest target resolution disabled due runtime fingerprint mismatch"
        if fail_on_fp_mismatch:
            errs.append(msg)
        else:
            print(f"WARN {case_name}: {msg}")

events = []
if not tele_path.exists():
    errs.append("missing telemetry JSONL")
else:
    with tele_path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError as e:
                errs.append(f"bad telemetry JSON: {e}")
                break
    summary_events = [e for e in events if e.get("event") == "dc3_summary"]
    if not summary_events:
        errs.append("missing dc3_summary telemetry event")
    else:
        s = summary_events[-1]
        np = s.get("nui_patch", {})
        if np.get("overridden") != expected_total:
            errs.append(f"telemetry nui_patch.overridden={np.get('overridden')} expected {expected_total}")
        if np.get("layout") != expected_layout:
            errs.append(f"telemetry nui_patch.layout={np.get('layout')} expected {expected_layout}")
        nr = s.get("nui_resolver", {})
        if nr.get("total") != expected_total:
            errs.append(f"telemetry nui_resolver.total={nr.get('total')} expected {expected_total}")
        if expected_mode == "strict":
            if nr.get("signature_hits") != expected_total or nr.get("strict_rejects") != 0:
                errs.append(f"telemetry strict coverage mismatch: sig_hits={nr.get('signature_hits')} strict_rejects={nr.get('strict_rejects')}")
    if not any(e.get("event") == "dc3_nui_override_registered" for e in events):
        errs.append("missing dc3_nui_override_registered telemetry events")

if errs:
    print(f"FAIL {case_name}: " + "; ".join(errs), file=sys.stderr)
    sys.exit(1)

milestones = [e.get("name") for e in events if e.get("event") == "dc3_boot_milestone"]
hot_loops = [e for e in events if e.get("event") == "dc3_hot_loop_pc"]
unresolved = [e for e in events if e.get("event") == "dc3_unresolved_call_stub_hit"]
print(f"PASS {case_name}: mode={expected_mode} layout={layout} total={patch_total} overridden={overridden} sig_hits={sig_hits} strict_rejects={strict_rejects} telemetry={{milestones:{len(milestones)} hot_loops:{len(hot_loops)} unresolved:{len(unresolved)}}}")
PY
}

preflight_case_inputs orig "$ORIG_XEX" original "$ORIG_MANIFEST_PATH" "$ORIG_SYMBOL_MAP_PATH"
preflight_case_inputs decomp "$DECOMP_XEX" decomp "$DECOMP_MANIFEST_PATH" "$DECOMP_SYMBOL_MAP_PATH"

run_case orig "$ORIG_XEX" 59 original "$ORIG_MANIFEST_PATH" "$ORIG_SYMBOL_MAP_PATH"
run_case decomp "$DECOMP_XEX" 85 decomp "$DECOMP_MANIFEST_PATH" "$DECOMP_SYMBOL_MAP_PATH"

# Emit symbolized artifacts on successful runs too when enabled, so parity investigations have
# ready-to-open files without rerunning the gate.
emit_symbolized_artifacts

python3 - "$TMPDIR_GATE/orig.jsonl" "$TMPDIR_GATE/decomp.jsonl" "$REQUIRE_EQUAL_MILESTONES" "$FAIL_ON_NEW_HOT_LOOP" "$MILESTONE_POLICY" "$MILESTONE_CONTRACT" "$TMPDIR_GATE/orig.log" "$TMPDIR_GATE/decomp.log" <<'PY'
import json, sys
from collections import Counter
from pathlib import Path
import re

orig_path = Path(sys.argv[1]); decomp_path = Path(sys.argv[2])
require_equal_milestones = sys.argv[3] == "1"
fail_on_new_hot_loop = sys.argv[4] == "1"
milestone_policy = sys.argv[5].strip().lower()
milestone_contract = [s for s in sys.argv[6].split(",") if s]
orig_log_path = Path(sys.argv[7]); decomp_log_path = Path(sys.argv[8])

if milestone_policy not in ("off", "warn", "fail"):
    print(f"WARN: invalid milestone policy {milestone_policy!r}; using 'warn'")
    milestone_policy = "warn"

def load(path):
    events = []
    with path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if line:
                events.append(json.loads(line))
    return events

orig = load(orig_path); decomp = load(decomp_path)
orig_m = [e.get("name") for e in orig if e.get("event") == "dc3_boot_milestone"]
dec_m = [e.get("name") for e in decomp if e.get("event") == "dc3_boot_milestone"]
orig_hot = Counter((e.get("guest_pc"), int(e.get("count", 0))) for e in orig if e.get("event") == "dc3_hot_loop_pc")
dec_hot = Counter((e.get("guest_pc"), int(e.get("count", 0))) for e in decomp if e.get("event") == "dc3_hot_loop_pc")
orig_un = Counter((e.get("reason"), e.get("guest_addr")) for e in orig if e.get("event") == "dc3_unresolved_call_stub_hit")
dec_un = Counter((e.get("reason"), e.get("guest_addr")) for e in decomp if e.get("event") == "dc3_unresolved_call_stub_hit")

warns = []
errs = []
if orig_m != dec_m:
    msg = f"milestone sequences differ: orig={orig_m} decomp={dec_m}"
    (errs if require_equal_milestones else warns).append(msg)

dec_hot_pcs = {pc for (pc, _count) in dec_hot.keys()}
orig_hot_pcs = {pc for (pc, _count) in orig_hot.keys()}
new_hot = sorted(pc for pc in dec_hot_pcs if pc not in orig_hot_pcs and pc)
if new_hot:
    msg = "new decomp hot-loop PCs vs orig: " + ", ".join(new_hot[:10])
    (errs if fail_on_new_hot_loop else warns).append(msg)

contract_rank = {name: i for i, name in enumerate(milestone_contract)}
def highest_contract_rank(seq):
    rank = -1
    for item in seq:
        if item in contract_rank:
            rank = max(rank, contract_rank[item])
    return rank

def highest_contract_name(seq):
    r = highest_contract_rank(seq)
    return milestone_contract[r] if 0 <= r < len(milestone_contract) else "<none>"

common_prefix = 0
for a, b in zip(orig_m, dec_m):
    if a != b:
        break
    common_prefix += 1

orig_rank = highest_contract_rank(orig_m)
dec_rank = highest_contract_rank(dec_m)
milestone_verdict = "PASS"
milestone_msgs = []
if milestone_policy != "off":
    if dec_rank < orig_rank:
        milestone_verdict = "FAIL" if milestone_policy == "fail" else "WARN"
        milestone_msgs.append(
            f"decomp reached earlier contract milestone (decomp={highest_contract_name(dec_m)} "
            f"orig={highest_contract_name(orig_m)})"
        )
    elif orig_m != dec_m:
        milestone_verdict = "WARN"
        milestone_msgs.append(
            f"sequence divergence after common_prefix={common_prefix} "
            f"(orig_len={len(orig_m)} decomp_len={len(dec_m)})"
        )
print(
    "Milestone contract verdict: "
    f"{milestone_verdict} policy={milestone_policy} common_prefix={common_prefix} "
    f"orig_reached={highest_contract_name(orig_m)} decomp_reached={highest_contract_name(dec_m)}"
)
for msg in milestone_msgs:
    print("  milestone:", msg)
if milestone_verdict == "FAIL":
    errs.append("milestone contract regression")

CRT_RE = re.compile(
    r"DC3: CRT table (__x[ci]_a\.\.__x[ci]_z).*?:\s*(\d+) total entries, .*?(\d+) valid, .*?(\d+) nullified-oob, .*?(\d+) nullified-bisect, .*?(\d+) nullified-skip"
)
SUM_RE = re.compile(r"DC3: NUI resolver summary")

def parse_crt(log_path: Path):
    out = {}
    if not log_path.exists():
        return out
    text = log_path.read_text(encoding="utf-8", errors="ignore")
    for m in CRT_RE.finditer(text):
        out[m.group(1)] = {
            "total": int(m.group(2)),
            "valid": int(m.group(3)),
            "nullified_oob": int(m.group(4)),
            "nullified_bisect": int(m.group(5)),
            "nullified_skip": int(m.group(6)),
        }
    return out

orig_crt = parse_crt(orig_log_path)
dec_crt = parse_crt(decomp_log_path)
def cxx_row(rows):
    for k, v in rows.items():
        if "__xc_a" in k:
            return v
    return None

o_cxx = cxx_row(orig_crt)
d_cxx = cxx_row(dec_crt)
print("CRT impact triage:")
if o_cxx and d_cxx:
    print(
        "  cxx_ctor_table: "
        f"orig valid={o_cxx['valid']} skip={o_cxx['nullified_skip']} "
        f"decomp valid={d_cxx['valid']} skip={d_cxx['nullified_skip']}"
    )
    skip_delta = d_cxx["nullified_skip"] - o_cxx["nullified_skip"]
    valid_delta = d_cxx["valid"] - o_cxx["valid"]
    print(f"  deltas: valid={valid_delta:+} nullified_skip={skip_delta:+}")
    if skip_delta > 0 and dec_rank < orig_rank:
        print(
            "  REPRIORITIZE: decomp/jeff CRT constructor export fixes ahead of low-impact cleanup "
            "(milestone regression + higher decomp CRT nullified-skip)."
        )
    elif skip_delta > 0:
        print("  HINT: decomp CRT skip count is higher than original; watch for init-stage regressions.")
    else:
        print("  HINT: no CRT skip regression vs original in this parity run.")
else:
    print("  <CRT table summary missing in one or both logs>")

if warns:
    for w in warns:
        print("WARN:", w)
if errs:
    for e in errs:
        print("FAIL:", e, file=sys.stderr)
    sys.exit(1)

print("Parity diff summary: warn-only comparisons complete")
print("  orig milestones:", orig_m)
print("  decomp milestones:", dec_m)
print("  orig unresolved unique:", len(orig_un))
print("  decomp unresolved unique:", len(dec_un))
PY

if [[ -f "$TMPDIR_GATE/orig.jsonl" && -f "$TMPDIR_GATE/decomp.jsonl" ]]; then
  if [[ -x "$TELEMETRY_DIFF_TOOL" || "$TELEMETRY_DIFF_TOOL" == *.py ]]; then
    echo "Telemetry diff (top=$TELEMETRY_DIFF_TOP):"
    diff_args=(
      --orig "$TMPDIR_GATE/orig.jsonl"
      --decomp "$TMPDIR_GATE/decomp.jsonl"
      --top "$TELEMETRY_DIFF_TOP"
    )
    if [[ -n "$ORIG_SYMBOL_MAP_PATH" ]]; then
      diff_args+=(--orig-symbols "$ORIG_SYMBOL_MAP_PATH")
    fi
    if [[ -n "$DECOMP_SYMBOL_MAP_PATH" ]]; then
      diff_args+=(--decomp-symbols "$DECOMP_SYMBOL_MAP_PATH")
    fi
    python3 "$TELEMETRY_DIFF_TOOL" "${diff_args[@]}" || true
  fi
fi

echo "DC3 runtime parity gate passed for mode=$PARITY_MODE. Artifacts: $TMPDIR_GATE"
