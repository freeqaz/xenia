#!/usr/bin/env python3
"""fork-regress comparator.

  compare.py <baseline.json> <candidate summary.json | out-dir>   compare
  compare.py --make-baseline <summary.json | out-dir> [--note TEXT] condense a run
                                                                  into a baseline
  compare.py --paired <outA> <outB>                               ab.sh output: run i
                                                                  of A vs run i of B

--paired exists because the load gate cannot tell a load-induced failure from a
real one on a single run, but an interleaved pair can: A and B started minutes
apart see the same host. A loaded FAIL of A next to a PASS of B at a similar
load is evidence against A (PAIRED-FAIL), even though each run alone is
INCONCLUSIVE. Each pair prints both raw verdicts and both mean loads.

A baseline is a condensed summary.json (baselines/*.json, small text). The
comparison is per scenario:

  verdict      baseline PASS -> candidate FAIL         REGRESSION
               baseline PASS -> candidate INCONCLUSIVE INCONCLUSIVE (re-run on a quiet host)
               candidate SKIPPED where baseline ran    REGRESSION (capability lost)
               baseline FAIL -> candidate PASS          IMPROVED
  known races  every attempt that hit a known original-game race
               (`known_game_race:<name>`, README.md) is INCONCLUSIVE, never a
               PASS; the count is printed as KNOWN-RACE on every comparison
               and does not change the exit code (a scenario left
               INCONCLUSIVE by it still exits 3)
  S0           every changed/added/removed cvar default is LISTED (a cleanup
               lane must name each one in its commit message); a source
               ratchet that increases is a REGRESSION
  S3           count + LR histogram are already judged against the s66
               reference inside the verdict
  S6           RATCHET on the set of leaking patterns per variant: a pattern
               that did not leak in the baseline is a REGRESSION; counts are
               reported only (they scale with how long the run lived)
  S1/S2/S4/S5  milestone times (median over PASS runs that were not loaded)
               are reported as deltas, never judged: the flow is wall-clock
               driven

Exit: 0 no regression, 1 regression, 3 nothing regressed but something was
INCONCLUSIVE, 2 usage.
"""
import json
import os
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
        out["scenarios"][s] = strip({k: sc[k] for k in ("verdict", "counts", "need_pass",
                                                       "known_game_races", "runs",
                                                       "attempts") if k in sc})
    return out


WATCH = {
    "S1V": ["game_screen_reached", "fail_screen_first_swap", "fail_screen_screen"],
    "S2": ["answers.{+ 1 2}", "answers.{+ 5 5}", "first_poll_thread"],
    "S3": ["count", "lr_sequence_equal", "text_fnv1a64"],
    "S4": ["mode", "tv3_screens", "song_stream.chans", "song_stream.mState"],
    "S5": ["tv3_screens"],
}


def dig(m, dotted):
    for part in dotted.split(".") if not dotted.startswith("answers.") else ["answers", dotted[8:]]:
        m = m.get(part) if isinstance(m, dict) else None
    return m


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


KNOWN_RACE_PREFIX = "known_game_race:"  # lib/fr.py


def race_counts(sc):
    """Known-race attempts in a scenario, by race, from its attempts (a
    baseline condensed before the field existed is counted the same way)."""
    out = {}
    for a in sc.get("attempts", []):
        for r in a.get("reasons", []):
            if r.startswith(KNOWN_RACE_PREFIX):
                name = r[len(KNOWN_RACE_PREFIX):]
                out[name] = out.get(name, 0) + 1
    return out


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

        # Known original-game races (README.md): INCONCLUSIVE attempts, never
        # a PASS. Reported on every comparison so the rate stays visible; not
        # judged (the race is in the game, so a rate change is not evidence
        # about the binary on its own).
        for name, n in sorted(race_counts(c).items()):
            bn = race_counts(b).get(name, 0)
            findings.append(("KNOWN-RACE", s, f"known_game_race:{name} x{n} of "
                             f"{len(c.get('attempts', []))} attempts (baseline x{bn} of "
                             f"{len(b.get('attempts', []))})"))

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
            # Ratchet on the SET of leaking patterns per variant. Counts are
            # reported but not judged: the thread-6 dump repeats every status
            # report, so its count scales with how long the run lived (40 vs
            # 224 lines for the same leak on two binaries).
            def leaks(sc):
                out = {}
                for r in sc.get("runs", []):
                    m = r.get("measurements", {})
                    for k, n in (m.get("counts") or {}).items():
                        if n:
                            out.setdefault((m.get("variant"), k), []).append(n)
                return out
            bl, cl = leaks(b), leaks(c)
            for key in sorted(set(cl) - set(bl), key=str):
                findings.append(("REGRESSION", s, f"variant {key[0]} new leak '{key[1]}' "
                                 f"x{max(cl[key])}"))
            for key in sorted(set(bl) - set(cl), key=str):
                findings.append(("IMPROVED", s, f"variant {key[0]} leak gone '{key[1]}' "
                                 f"(was x{max(bl[key])})"))
            for key in sorted(set(bl) & set(cl), key=str):
                findings.append(("INFO", s, f"variant {key[0]} '{key[1]}' "
                                 f"x{max(bl[key])} -> x{max(cl[key])}"))

        # Watched measurements: not pass criteria, but a change is worth a
        # line (e.g. Vulkan suddenly reaching game_screen, or the song the
        # RB3 autopilot lands on).
        for key in WATCH.get(s, []):
            bvals = sorted({json.dumps(dig(r.get("measurements", {}), key)) for r in b.get("runs", [])})
            cvals = sorted({json.dumps(dig(r.get("measurements", {}), key)) for r in c.get("runs", [])})
            if bvals != cvals:
                findings.append(("CHANGED", s, f"{key}: {', '.join(bvals)} -> {', '.join(cvals)}"))

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


def raw_verdict(v):
    r = v["verdict"]
    if r == "INCONCLUSIVE" and any("host loaded" in x for x in v.get("reasons", [])):
        return "FAIL"
    return r


def paired(out_a, out_b):
    rows, bad = [], 0
    for va_path in sorted(Path(out_a).glob("*/run-*/verdict.json")):
        rel = va_path.relative_to(out_a)
        vb_path = Path(out_b) / rel
        if not vb_path.exists() or ".retry" in rel.parts[1]:
            continue
        # Use each side's FINAL attempt for this index.
        def final(p):
            ds = sorted((d for d in p.parent.parent.glob(p.parent.name + "*")
                         if (d / "verdict.json").exists()),
                        key=lambda d: (len(d.name), d.name))
            return json.loads((ds[-1] / "verdict.json").read_text())
        va, vb = final(va_path), final(vb_path)
        la = va["provenance"]["load"].get("load1_mean")
        lb = vb["provenance"]["load"].get("load1_mean")
        ra, rb = raw_verdict(va), raw_verdict(vb)
        tag = ""
        # A known original-game race stays INCONCLUSIVE on its side: not a
        # PAIRED-FAIL against that binary, and not a PASS.
        for side, v in (("A", va), ("B", vb)):
            for r in v.get("reasons", []):
                if r.startswith(KNOWN_RACE_PREFIX):
                    tag += f"{side}:{r} "
        # A failing side counts against its binary when it was not loaded
        # itself (below the gate), or no more loaded than the passing side.
        gate = float(os.environ.get("FR_LOAD_MAX", 80))
        def comparable(lf, lp):
            return lf is not None and lp is not None and (lf <= gate or lf <= 1.25 * lp + 5)
        if ra == "FAIL" and rb == "PASS":
            near = comparable(la, lb)
            tag = "PAIRED-FAIL(A)" if near else "A-FAIL(A more loaded)"
            bad += near
        elif ra == "PASS" and rb == "FAIL":
            near = comparable(lb, la)
            tag = "PAIRED-FAIL(B)" if near else "B-FAIL(B more loaded)"
        rows.append((str(rel.parent), ra, la, rb, lb, tag))
    print(f"{'run':16s} {'A':8s} {'ldA':>6s}  {'B':8s} {'ldB':>6s}  note")
    for r in rows:
        print(f"{r[0]:16s} {r[1]:8s} {r[2] or 0:6.1f}  {r[3]:8s} {r[4] or 0:6.1f}  {r[5]}")
    return 1 if bad else 0


def main(argv):
    if len(argv) == 4 and argv[1] == "--paired":
        return paired(argv[2], argv[3])
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
    order = ["REGRESSION", "INCONCLUSIVE", "KNOWN-RACE", "LIST", "CHANGED", "IMPROVED", "TIMING",
             "SAME", "INFO"]
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
