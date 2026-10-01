#!/usr/bin/env python3
"""fork-regress comparator.

  compare.py <baseline.json> <candidate summary.json | out-dir>   compare
  compare.py --make-baseline <summary.json | out-dir> [--note TEXT] condense a run
                                                                  into a baseline

A baseline is a condensed summary.json (baselines/*.json, small text). The
comparison is per scenario:

  verdict      baseline PASS -> candidate FAIL         REGRESSION
               baseline PASS -> candidate INCONCLUSIVE INCONCLUSIVE (re-run on a quiet host)
               candidate SKIPPED where baseline ran    REGRESSION (capability lost)
               baseline FAIL -> candidate PASS          IMPROVED
  S0           every changed/added/removed cvar default is LISTED (a cleanup
               lane must name each one in its commit message); a source
               ratchet that increases is a REGRESSION
  S3           count + LR histogram are already judged against the s66
               reference inside the verdict
  S6           RATCHET: each other-title pattern count must not exceed the
               baseline's worst run for that variant
  S1/S2/S4/S5  milestone times (median over PASS runs that were not loaded)
               are reported as deltas, never judged: the flow is wall-clock
               driven

Exit: 0 no regression, 1 regression, 3 nothing regressed but something was
INCONCLUSIVE, 2 usage.
"""
import json
import statistics
import sys
from pathlib import Path

DROP_KEYS = {"timeline", "capture_swaps", "examples", "lr_sequence"}


def load_summary(p):
    p = Path(p)
    if p.is_dir():
        p = p / "summary.json"
    return json.loads(p.read_text())


def condense(summary, note=""):
    def strip(o):
        if isinstance(o, dict):
            return {k: strip(v) for k, v in o.items() if k not in DROP_KEYS}
        if isinstance(o, list):
            return [strip(v) for v in o]
        return o
    inv = summary.get("invocation", {})
    out = {"schema": "fork-regress/baseline-v1", "note": note,
           "binary_label": inv.get("binary_label"), "binary_xxh3": inv.get("binary_xxh3"),
           "binary_git": inv.get("binary_git"), "recorded": inv.get("started"),
           "load_max_gate": inv.get("load_max"),
           "verdicts": summary.get("verdicts"),
           "passive_inertness": summary.get("passive_inertness"),
           "scenarios": {}}
    for s, sc in summary.get("scenarios", {}).items():
        out["scenarios"][s] = strip({k: sc[k] for k in ("verdict", "counts", "need_pass", "runs",
                                                       "attempts") if k in sc})
    return out


def median_milestones(sc, key_path):
    vals = {}
    for r in sc.get("runs", []):
        if r.get("verdict") != "PASS" or r.get("loaded"):
            continue
        m = r.get("measurements", {})
        for k in key_path:
            m = m.get(k, {}) if isinstance(m, dict) else {}
        for name, t in (m or {}).items():
            if isinstance(t, (int, float)):
                vals.setdefault(name, []).append(t)
    return {k: statistics.median(v) for k, v in vals.items()}


def compare(base, cand):
    findings = []  # (severity, scenario, text)
    bs, cs = base.get("scenarios", {}), cand.get("scenarios", {})
    for s in sorted(set(bs) | set(cs)):
        b, c = bs.get(s), cs.get(s)
        if c is None:
            findings.append(("INFO", s, "not run by candidate"))
            continue
        if b is None:
            findings.append(("INFO", s, f"no baseline; candidate {c['verdict']}"))
            continue
        bv, cv = b["verdict"], c["verdict"]
        if bv == "PASS" and cv == "FAIL":
            findings.append(("REGRESSION", s, f"PASS -> FAIL {c.get('counts')}"))
        elif bv == "PASS" and cv == "INCONCLUSIVE":
            findings.append(("INCONCLUSIVE", s, f"PASS -> INCONCLUSIVE {c.get('counts')}"))
        elif cv == "SKIPPED" and bv != "SKIPPED":
            findings.append(("REGRESSION", s, f"{bv} -> SKIPPED (capability lost?)"))
        elif bv in ("FAIL", "SKIPPED", "INCONCLUSIVE") and cv == "PASS":
            findings.append(("IMPROVED", s, f"{bv} -> PASS"))
        elif bv == cv:
            findings.append(("SAME", s, f"{cv} {c.get('counts')}"))
        else:
            findings.append(("CHANGED", s, f"{bv} -> {cv}"))

        if s == "S0":
            bm = (b.get("runs") or [{}])[0].get("measurements", {})
            cm = (c.get("runs") or [{}])[0].get("measurements", {})
            bd, cd = bm.get("cvar_defaults", {}), cm.get("cvar_defaults", {})
            added = sorted(set(cd) - set(bd))
            removed = sorted(set(bd) - set(cd))
            changed = sorted(k for k in set(bd) & set(cd) if bd[k] != cd[k])
            for k in changed:
                findings.append(("LIST", s, f"cvar default changed: {k} {bd[k]!r} -> {cd[k]!r}"))
            if added:
                findings.append(("LIST", s, f"cvars added ({len(added)}): {', '.join(added)}"))
            if removed:
                findings.append(("LIST", s, f"cvars removed ({len(removed)}): {', '.join(removed)}"))
            br, cr = bm.get("ratchets", {}), cm.get("ratchets", {})
            for k, v in cr.items():
                if v is not None and br.get(k) is not None and v > br[k]:
                    findings.append(("REGRESSION", s, f"ratchet {k} {br[k]} -> {v}"))
                elif v is not None and br.get(k) is not None and v < br[k]:
                    findings.append(("IMPROVED", s, f"ratchet {k} {br[k]} -> {v}"))

        if s == "S6":
            worst = {}
            for r in b.get("runs", []):
                m = r.get("measurements", {})
                for k, n in (m.get("counts") or {}).items():
                    key = (m.get("variant"), k)
                    worst[key] = max(worst.get(key, 0), n)
            for r in c.get("runs", []):
                m = r.get("measurements", {})
                for k, n in (m.get("counts") or {}).items():
                    lim = worst.get((m.get("variant"), k))
                    if lim is not None and n > lim:
                        findings.append(("REGRESSION", s, f"variant {m.get('variant')} "
                                         f"'{k}' {lim} -> {n} (ratchet)"))
                    elif lim is not None and n < lim:
                        findings.append(("IMPROVED", s, f"variant {m.get('variant')} "
                                         f"'{k}' {lim} -> {n}"))

        paths = {"S1": ["milestones_s"], "S2": ["flow", "milestones_s"],
                 "S1V": ["flow", "milestones_s"], "S4": ["screens_first_seen_s"],
                 "S5": ["screens_first_seen_s"]}
        if s in paths:
            bmed, cmed = median_milestones(b, paths[s]), median_milestones(c, paths[s])
            for k in sorted(set(bmed) & set(cmed)):
                d = cmed[k] - bmed[k]
                if abs(d) >= 3.0:
                    findings.append(("TIMING", s, f"{k}: {bmed[k]:.1f}s -> {cmed[k]:.1f}s "
                                     f"({d:+.1f}s, median of unloaded PASS runs)"))
    return findings


def main(argv):
    if len(argv) >= 3 and argv[1] == "--make-baseline":
        note = ""
        if "--note" in argv:
            note = argv[argv.index("--note") + 1]
        print(json.dumps(condense(load_summary(argv[2]), note), indent=1))
        return 0
    if len(argv) != 3:
        print(__doc__)
        return 2
    base = json.loads(Path(argv[1]).read_text())
    cand = load_summary(argv[2])
    findings = compare(base, cand)
    order = ["REGRESSION", "INCONCLUSIVE", "LIST", "CHANGED", "IMPROVED", "TIMING", "SAME", "INFO"]
    for sev in order:
        for f in findings:
            if f[0] == sev:
                print(f"{f[0]:12s} {f[1]:4s} {f[2]}")
    if any(f[0] == "REGRESSION" for f in findings):
        print("RESULT: REGRESSION")
        return 1
    if any(f[0] == "INCONCLUSIVE" for f in findings):
        print("RESULT: INCONCLUSIVE")
        return 3
    print("RESULT: no regression")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
