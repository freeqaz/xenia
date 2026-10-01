"""S3: DC3 decomp layout, 627 forced traps (plan §4.2 S3).

What it measures: the 2026-08-24 decomp-built default.xex, booted for 120 s
through the decomp-layout hack pack, executes `tw`/`td` forced traps that the
JIT logs as `tw/td forced trap hit! PC=… LR=<caller>` (x64_emitter.cc,
TrapDebugBreak). The COUNT (627) and the per-LR HISTOGRAM are a fingerprint of
how far the decomp image gets and through which code: every trap is a
MILO_ASSERT/debug-break site in decomp code, and LR names its caller. A core
change that perturbs JIT, kernel or memory semantics moves either the count or
the histogram. Measured identical across s63..s66 (2026-08-28/29), including
the exact LR SEQUENCE, so the sequence hash is recorded too (informational:
thread interleaving may legitimately reorder it).

Reference: reference/trap627_s66.json, extracted from the pinned
dc3_nonreg_s66.log (python3 analyze/trap627.py --extract <log>).
"""
import hashlib
import json
import re
import sys
from collections import Counter
from pathlib import Path

TRAP_RE = re.compile(r"tw/td forced trap hit!.*?LR=([0-9A-Fa-f]{8})")
LAYOUT_RE = re.compile(r"DC3: NUI patch layout=(\w+)")
MANIFEST_MISMATCH_RE = re.compile(
    r"Disabling patch manifest target resolution due fingerprint mismatch")
SYMBOLS_RE = re.compile(r"Loaded NUI symbol manifest '[^']*' \((\d+) \.text symbols\)")
FNV_RE = re.compile(r"DC3: \.text fingerprint .*fnv1a64=([0-9A-Fa-f]+)")
TIMEOUT_RE = re.compile(r"TIMEOUT: (\d+)ms reached")
REF = Path(__file__).resolve().parent.parent / "reference" / "trap627_s66.json"


def extract(log: Path) -> dict:
    lrs, layout, mismatch, syms, fnv, tmo = [], None, False, None, None, None
    with open(log, errors="replace") as f:
        for line in f:
            m = TRAP_RE.search(line)
            if m:
                lrs.append(m.group(1).upper())
                continue
            if layout is None and (m := LAYOUT_RE.search(line)):
                layout = m.group(1)
            if MANIFEST_MISMATCH_RE.search(line):
                mismatch = True
            if syms is None and (m := SYMBOLS_RE.search(line)):
                syms = int(m.group(1))
            if fnv is None and (m := FNV_RE.search(line)):
                fnv = m.group(1).upper()
            if (m := TIMEOUT_RE.search(line)):
                tmo = int(m.group(1))
    hist = dict(sorted(Counter(lrs).items(), key=lambda kv: (-kv[1], kv[0])))
    return {"count": len(lrs), "lr_histogram": hist, "distinct_lrs": len(hist),
            "lr_sequence_sha256": hashlib.sha256(",".join(lrs).encode()).hexdigest(),
            "layout": layout, "manifest_fingerprint_mismatch_line": mismatch,
            "symbols_loaded": syms, "text_fnv1a64": fnv, "timeout_reached_ms": tmo}


def analyze(run_dir: Path, meta: dict):
    ref = json.loads(REF.read_text())
    m = extract(run_dir / "run.log")
    reasons = []
    if m["count"] != ref["count"]:
        reasons.append(f"forced trap count {m['count']} != {ref['count']}")
    if m["lr_histogram"] != ref["lr_histogram"]:
        diff = {}
        for lr in sorted(set(ref["lr_histogram"]) | set(m["lr_histogram"])):
            a, b = ref["lr_histogram"].get(lr, 0), m["lr_histogram"].get(lr, 0)
            if a != b:
                diff[lr] = {"ref": a, "got": b}
        m["lr_histogram_diff"] = diff
        reasons.append(f"LR histogram differs at {len(diff)} LRs")
    m["lr_sequence_equal"] = m["lr_sequence_sha256"] == ref["lr_sequence_sha256"]
    if m["layout"] != "decomp":
        reasons.append(f"layout={m['layout']} (want decomp)")
    if not m["manifest_fingerprint_mismatch_line"]:
        reasons.append("manifest-fingerprint-mismatch line absent")
    if m["timeout_reached_ms"] is None:
        reasons.append("no 'TIMEOUT: <ms>ms reached' line")
    if meta.get("rc") != 0:
        reasons.append(f"rc {meta.get('rc')} != 0")
    crit = {"count": ref["count"], "lr_histogram": "equal to reference/trap627_s66.json",
            "layout": "decomp", "manifest_fingerprint_mismatch_line": True,
            "timeout_line": "present", "rc": 0,
            "informational": ["lr_sequence_equal", "symbols_loaded", "text_fnv1a64"]}
    return ("PASS" if not reasons else "FAIL"), reasons, m, crit


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--extract":
        r = extract(Path(sys.argv[2]))
        r["source"] = Path(sys.argv[2]).name
        print(json.dumps(r, indent=2))
    else:
        sys.exit("usage: trap627.py --extract <log>")
