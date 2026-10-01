#!/usr/bin/env python3
"""Tabulate flow-scenario outcomes against host load, across any number of
fork-regress out-dirs. This is the evidence behind FR_LOAD_MAX (lib/fr.py).

  load_evidence.py <out-dir>... [--scenarios S1,S2,S4,S5] [--json]

For each run: scenario, binary label, verdict BEFORE the load gate (the
analyzer's own PASS/FAIL), failure class, 1-min load at start / mean / max,
PSI cpu some avg10 mean, and title/game_screen times. Then PASS rate per load
bucket. Runs SKIPPED or interrupted are left out.
"""
import json
import sys
from pathlib import Path

BUCKETS = [(0, 30), (30, 45), (45, 60), (60, 80), (80, 100), (100, 1e9)]


def raw_verdict(v):
    # finalize() may have turned FAIL into INCONCLUSIVE because of load or an
    # external signal; the evidence needs the analyzer's own answer.
    r = v["verdict"]
    if r == "INCONCLUSIVE" and any("host loaded" in x for x in v.get("reasons", [])):
        return "FAIL"
    return r


def rows(dirs, scen):
    for d in dirs:
        for p in sorted(Path(d).glob("*/run-*/verdict.json")):
            v = json.loads(p.read_text())
            if v["scenario"] not in scen or v["verdict"] == "SKIPPED":
                continue
            if any("interrupted" in r for r in v.get("reasons", [])):
                continue
            m, ld = v.get("measurements", {}), v["provenance"].get("load", {})
            ms = m.get("milestones_s") or (m.get("flow") or {}).get("milestones_s") or {}
            scr = m.get("screens_first_seen_s") or {}
            yield {
                "run": f"{p.parent.parent.parent.name}/{v['scenario']}/{p.parent.name}",
                "scenario": v["scenario"],
                "binary": v["provenance"].get("binary_label"),
                "verdict": raw_verdict(v),
                "failure_class": m.get("failure_class") or (m.get("flow") or {}).get("failure_class"),
                "load_start": ld.get("load1_start"), "load_mean": ld.get("load1_mean"),
                "load_max": ld.get("load1_max"), "psi_mean": ld.get("psi_cpu_some_avg10_mean"),
                "title_s": ms.get("title_screen"), "game_s": ms.get("game_screen"),
                "main_hub_s": scr.get("main_hub_screen"),
                "rb3_game_s": m.get("game_screen_entered_s"),
            }


def main(argv):
    scen = {"S1", "S2", "S4", "S5", "S1V"}
    as_json = "--json" in argv
    args = [a for a in argv[1:] if a != "--json"]
    if "--scenarios" in args:
        i = args.index("--scenarios")
        scen = set(args[i + 1].split(","))
        args = args[:i] + args[i + 2:]
    rs = list(rows(args, scen))
    if as_json:
        print(json.dumps(rs, indent=1))
        return 0
    print(f"{'run':44s} {'verdict':7s} {'class':30s} {'ld0':>6s} {'ldμ':>6s} {'ldmax':>6s} "
          f"{'psiμ':>5s} {'title':>6s} {'game':>6s}")
    for r in sorted(rs, key=lambda r: (r["load_mean"] or 0)):
        g = r["game_s"] if r["game_s"] is not None else r["rb3_game_s"]
        print(f"{r['run'][:44]:44s} {r['verdict']:7s} {str(r['failure_class'] or '')[:30]:30s} "
              f"{r['load_start'] or 0:6.1f} {r['load_mean'] or 0:6.1f} {r['load_max'] or 0:6.1f} "
              f"{r['psi_mean'] or 0:5.1f} {str(r['title_s'] or '-'):>6s} {str(g or '-'):>6s}")
    print("\nPASS rate by mean 1-min load:")
    for lo, hi in BUCKETS:
        b = [r for r in rs if r["load_mean"] is not None and lo <= r["load_mean"] < hi]
        if b:
            p = sum(1 for r in b if r["verdict"] == "PASS")
            print(f"  [{lo:>3.0f},{hi if hi < 1e9 else 'inf':>4}) {p}/{len(b)} PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
