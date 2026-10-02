#!/usr/bin/env bash
set -euo pipefail

XENIA_DIR="${XENIA_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
BIN="${XENIA_BIN:-$XENIA_DIR/build/bin/Linux/Debug/xenia-headless}"
DECOMP_XEX="${DC3_DECOMP_XEX:-/home/free/code/milohax/dc3-decomp/build/373307D9/default.xex}"
ORIG_XEX="${DC3_ORIG_XEX:-/home/free/code/milohax/dc3-decomp/orig/373307D9/default.xex}"
TIMEOUT_SECS="${DC3_GATE_TIMEOUT_SECS:-20}"
HEADLESS_TIMEOUT_MS="${DC3_GATE_HEADLESS_TIMEOUT_MS:-15000}"
TMPDIR_GATE="${DC3_GATE_TMPDIR:-/tmp/xenia_cutover_gate}"
mkdir -p "$TMPDIR_GATE"

if [[ ! -x "$BIN" ]]; then
  echo "error: xenia-headless not found at $BIN" >&2
  exit 1
fi

run_case() {
  local case_name="$1" xex="$2" expected_total="$3" expected_layout="$4" expected_mode="$5" expected_sig_flag="$6" expected_guest_overrides="$7" expected_patched="$8" expected_overridden="$9"; shift 9
  local logfile="$TMPDIR_GATE/${case_name}.log"
  echo "== $case_name =="
  set +e
  timeout "${TIMEOUT_SECS}s" "$BIN" --gpu=null --target="$xex" --headless_timeout_ms="$HEADLESS_TIMEOUT_MS" "$@" >"$logfile" 2>&1
  local rc=$?
  set -e
  if [[ $rc -ne 0 && $rc -ne 124 ]]; then
    echo "FAIL: $case_name exited rc=$rc" >&2
    tail -80 "$logfile" >&2 || true
    return 1
  fi
  python3 - "$logfile" "$case_name" "$expected_total" "$expected_layout" "$expected_mode" "$expected_sig_flag" "$expected_guest_overrides" "$expected_patched" "$expected_overridden" <<'PY'
import re, sys
from pathlib import Path
log = Path(sys.argv[1]).read_text(errors='ignore')
case_name = sys.argv[2]
expected_total = int(sys.argv[3])
expected_layout = sys.argv[4]
expected_mode = sys.argv[5]
expected_sig_flag = int(sys.argv[6])
expected_guest_overrides = int(sys.argv[7])
expected_patched = int(sys.argv[8])
expected_overridden = int(sys.argv[9])

summary_re = re.compile(r"DC3: NUI resolver summary mode=(\w+) .* signature_hits=(\d+) .* strict_rejects=(\d+) total=(\d+)")
apply_re = re.compile(r"DC3: NUI/XBC apply path guest_overrides=(\d+) resolver_mode=(\w+) signature_resolver=(\d+)")
patch_re = re.compile(r"DC3: NUI patch/override summary: patched=(\d+) overridden=(\d+) skipped=(\d+) total=(\d+) layout=(\w+).*")
reg_re = re.compile(r"DC3: Registered (\d+) guest extern overrides from NUI patch table")

m_apply = apply_re.search(log)
m_summary = summary_re.search(log)
m_patch = patch_re.search(log)
if not (m_apply and m_summary and m_patch):
    print(f"FAIL {case_name}: missing expected DC3 summary lines", file=sys.stderr)
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
if guest_overrides != expected_guest_overrides: errs.append(f"guest_overrides={guest_overrides} expected {expected_guest_overrides}")
if apply_mode != expected_mode: errs.append(f"apply resolver_mode={apply_mode} expected {expected_mode}")
if resolver_mode != expected_mode: errs.append(f"resolver summary mode={resolver_mode} expected {expected_mode}")
if expected_sig_flag >= 0 and sig_flag != expected_sig_flag: errs.append(f"signature_resolver={sig_flag} expected {expected_sig_flag}")
if resolver_total != expected_total: errs.append(f"resolver total={resolver_total} expected {expected_total}")
if patch_total != expected_total: errs.append(f"patch total={patch_total} expected {expected_total}")
if layout != expected_layout: errs.append(f"layout={layout} expected {expected_layout}")
if patched != expected_patched: errs.append(f"patched={patched} expected {expected_patched}")
if overridden != expected_overridden: errs.append(f"overridden={overridden} expected {expected_overridden}")
if skipped != 0: errs.append(f"skipped={skipped} expected 0")
if expected_guest_overrides:
    if reg_count != expected_overridden: errs.append(f"registered={reg_count} expected {expected_overridden}")
    if expected_mode == 'strict':
        if sig_hits != expected_total or strict_rejects != 0:
            errs.append(f"strict signature coverage not full: hits={sig_hits} rejects={strict_rejects}")
else:
    if reg_count != 0: errs.append(f"registered={reg_count} expected 0")
    if expected_mode == 'legacy' and sig_hits != 0:
        errs.append(f"legacy mode unexpected signature hits={sig_hits}")

if "TIMEOUT:" not in log:
    errs.append("missing TIMEOUT marker (unexpected early exit)")

if errs:
    print(f"FAIL {case_name}: " + "; ".join(errs), file=sys.stderr)
    sys.exit(1)
print(f"PASS {case_name}: mode={expected_mode} layout={layout} total={patch_total} patched={patched} overridden={overridden} sig_hits={sig_hits} strict_rejects={strict_rejects}")
PY
}

# default/hybrid path (uses current defaults)
run_case default_orig "$ORIG_XEX" 59 original hybrid -1 1 0 59
run_case default_decomp "$DECOMP_XEX" 85 decomp hybrid -1 1 0 85
# strict signatures-only portability gate
run_case strict_orig "$ORIG_XEX" 59 original strict 1 1 0 59 \
  --dc3_nui_patch_resolver_mode=strict --dc3_nui_enable_signature_resolver=true \
  --dc3_nui_patch_manifest_path=/tmp/does_not_exist_manifest.json \
  --dc3_nui_symbol_map_path=/tmp/does_not_exist_symbols.txt
run_case strict_decomp "$DECOMP_XEX" 85 decomp strict 1 1 0 85 \
  --dc3_nui_patch_resolver_mode=strict --dc3_nui_enable_signature_resolver=true \
  --dc3_nui_patch_manifest_path=/tmp/does_not_exist_manifest.json \
  --dc3_nui_symbol_map_path=/tmp/does_not_exist_symbols.txt
echo "All DC3 NUI/XBC cutover gate cases (default+strict) passed. Logs: $TMPDIR_GATE"
