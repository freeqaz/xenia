#!/usr/bin/env python3
"""fork-regress core: provenance, cvars, load gating, verdicts, aggregation.

Run with /usr/bin/python3 (needs tomllib, Python >= 3.11).

Subcommands (all used by run.sh / the scenario scripts; nothing here launches
xenia itself):

  defaults   <bin> <cache-dir>               dump the binary's compiled-in cvar
                                             defaults (cached by xxh3), print path
  has-cvars  <defaults.toml> <name>...       exit 0 iff every cvar exists
  finalize   <scenario> <run-dir>            analyse one run, write verdict.json
  aggregate  <scenario> <scenario-dir>       combine runs -> scenario.json
  summary    <out-dir>                       combine scenarios -> summary.json

verdict.json schema "fork-regress/v1":
  scenario, run_dir, verdict (PASS|FAIL|INCONCLUSIVE|SKIPPED), reasons[],
  loaded (bool), measurements{}, criteria{}, provenance{...}
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

try:
    import tomllib
except ImportError:  # pragma: no cover
    sys.exit("fr.py needs Python >= 3.11 (tomllib); run it with /usr/bin/python3")

HERE = Path(__file__).resolve().parent
HARNESS = HERE.parent
SCHEMA = "fork-regress/v1"

# --------------------------------------------------------------------------
# Scenario table. N = default run count; need = PASSes required for the
# scenario to PASS; flow = load-sensitive (wall-clock-driven input/nav).
# --------------------------------------------------------------------------
SCENARIOS = {
    "S0":  dict(name="static: cvar defaults + source ratchets", N=1, need=1, flow=False,
                analyzer="static"),
    "S1":  dict(name="DC3 original, null GPU, ymca flow", N=3, need=2, flow=True,
                analyzer="dc3_flow"),
    "S1V": dict(name="DC3 original, Vulkan GPU 1, frame dump", N=1, need=1, flow=True,
                analyzer="vulkan_frames"),
    "S2":  dict(name="DC3 DTA channel round trip", N=2, need=1, flow=True,
                analyzer="dta"),
    "S3":  dict(name="DC3 decomp layout, 627 forced traps", N=2, need=2, flow=False,
                analyzer="trap627"),
    "S4":  dict(name="RB3 clean TU5 -> gameplay (+ song stream)", N=2, need=1, flow=True,
                analyzer="rb3_flow"),
    "S5":  dict(name="RB3DX -> gameplay", N=2, need=1, flow=True,
                analyzer="rb3_flow"),
    "S6":  dict(name="cross-title hook inertness", N=3, need=3, flow=False,
                analyzer="inertness"),
}

# Load gate. Max sampled 1-min loadavg during the run. Chosen from measured
# outcomes, see README.md "Load threshold". Overridable with FR_LOAD_MAX.
DEFAULT_LOAD_MAX = 60.0


def load_max() -> float:
    try:
        return float(os.environ.get("FR_LOAD_MAX", DEFAULT_LOAD_MAX))
    except ValueError:
        return DEFAULT_LOAD_MAX


# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------
def sha256(path) -> str | None:
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def xxh3(path) -> str | None:
    try:
        out = subprocess.run(["xxhsum", "-H3", str(path)], capture_output=True,
                             text=True, check=True).stdout.split()[0]
        return out.replace("XXH3_", "")
    except (OSError, subprocess.CalledProcessError, IndexError):
        return None


def git_rev(path) -> dict:
    """git HEAD + dirty count of the tree containing `path` (or {})."""
    p = Path(path).resolve()
    d = p if p.is_dir() else p.parent
    try:
        top = subprocess.run(["git", "-C", str(d), "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, check=True).stdout.strip()
        head = subprocess.run(["git", "-C", top, "rev-parse", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
        branch = subprocess.run(["git", "-C", top, "rev-parse", "--abbrev-ref", "HEAD"],
                                capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", top, "status", "--porcelain",
                                "--ignore-submodules=all"],
                               capture_output=True, text=True).stdout
        return {"tree": top, "head": head, "branch": branch,
                "dirty_files": len([l for l in dirty.splitlines() if l.strip()])}
    except (OSError, subprocess.CalledProcessError):
        return {}


def read_json(path, default=None):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return default


def write_json(path, obj):
    tmp = Path(str(path) + ".tmp")
    tmp.write_text(json.dumps(obj, indent=2, sort_keys=False) + "\n")
    tmp.replace(path)


# --------------------------------------------------------------------------
# cvars
# --------------------------------------------------------------------------
def flatten_toml(path) -> dict:
    """xenia toml -> {cvar: value}. Categories are flattened (names are unique)."""
    with open(path, "rb") as f:
        data = tomllib.load(f)
    flat = {}
    for k, v in data.items():
        if isinstance(v, dict):
            for k2, v2 in v.items():
                flat[k2] = v2
        else:
            flat[k] = v
    return flat


def defaults_dump(binary: str, cache_dir: str) -> str:
    """Dump the binary's compiled-in defaults: run it with an EMPTY storage
    root and no target, which makes SetupConfig write a full toml and exit.
    Cached per binary xxh3 so a whole run.sh invocation pays it once."""
    h = xxh3(binary) or "unknown"
    cache = Path(cache_dir) / f"defaults.{h}.toml"
    if cache.exists():
        return str(cache)
    cache.parent.mkdir(parents=True, exist_ok=True)
    scratch = cache.parent / f"defaults.{h}.storage"
    scratch.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    try:
        subprocess.run(["bash", "-c", 'ulimit -c 0; exec timeout -k 5 60 "$@"', "_",
                        binary, f"--storage_root={scratch}"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    except OSError:
        pass
    produced = scratch / "xenia.config.toml"
    if not produced.exists():
        raise SystemExit(f"defaults dump failed for {binary}")
    produced.replace(cache)
    return str(cache)


ARG_RE = re.compile(r"^--([A-Za-z0-9_]+)(?:=(.*))?$", re.S)


def parse_argv_cvars(argv: list[str]) -> dict:
    out = {}
    for a in argv:
        m = ARG_RE.match(a)
        if m:
            out[m.group(1)] = m.group(2) if m.group(2) is not None else "true"
    return out


def norm_value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


def effective_cvars(defaults_toml: str, config_toml: str | None, argv: list[str]):
    """defaults <- --config file <- argv. Values as strings."""
    eff = {k: norm_value(v) for k, v in flatten_toml(defaults_toml).items()}
    defaults = dict(eff)
    src = {k: "default" for k in eff}
    if config_toml and Path(config_toml).exists():
        # A config key the binary does not know is still recorded: it is
        # inert, but it is part of what was passed.
        for k, v in flatten_toml(config_toml).items():
            eff[k] = norm_value(v)
            src[k] = "config"
    unknown = []
    for k, v in parse_argv_cvars(argv).items():
        if k not in defaults and k not in ("target", "config"):
            unknown.append(k)
        eff[k] = v
        src[k] = "argv"
    nondefault = {k: {"value": eff[k], "default": defaults.get(k), "source": src[k]}
                  for k in sorted(eff) if defaults.get(k) != eff[k]}
    return dict(sorted(eff.items())), nondefault, unknown


# --------------------------------------------------------------------------
# load
# --------------------------------------------------------------------------
def load_stats(run_dir: Path) -> dict:
    rows = []
    try:
        for line in (run_dir / "load.tsv").read_text().splitlines():
            parts = line.split()
            if len(parts) >= 4 and parts[0] != "epoch":
                rows.append([float(x) for x in parts[:4]])
    except (OSError, ValueError):
        pass
    if not rows:
        return {"samples": 0}
    l1 = [r[1] for r in rows]
    psi = [r[3] for r in rows]
    return {
        "samples": len(rows),
        "load1_start": l1[0], "load1_end": l1[-1],
        "load1_max": max(l1), "load1_mean": round(sum(l1) / len(l1), 2),
        "psi_cpu_some_avg10_max": max(psi),
        "psi_cpu_some_avg10_mean": round(sum(psi) / len(psi), 2),
        "nproc": os.cpu_count(),
        "threshold_load1_max": load_max(),
    }


# --------------------------------------------------------------------------
# provenance
# --------------------------------------------------------------------------
def provenance(run_dir: Path, meta: dict) -> dict:
    binary = meta.get("binary")
    defaults_toml = meta.get("defaults_toml")
    config = meta.get("config")
    argv = meta.get("argv", [])
    prov = {
        "binary": binary,
        "binary_xxh3": meta.get("binary_xxh3") or (xxh3(binary) if binary else None),
        "binary_label": meta.get("binary_label"),
        "binary_git": meta.get("binary_git") or {},
        "harness_git": git_rev(HARNESS),
        "inputs": {k: {"path": v, "sha256": sha256(v)}
                   for k, v in (meta.get("inputs") or {}).items()},
        "config": {"path": meta.get("config_src"), "sha256": sha256(config) if config else None},
        "argv": argv,
        "rc": meta.get("rc"),
        "wall_s": meta.get("wall_s"),
        "start_epoch": meta.get("start_epoch"),
        "host": {"hostname": socket.gethostname(), "nproc": os.cpu_count(),
                 "kernel": os.uname().release},
        "load": load_stats(run_dir),
        "dropped_cvars": meta.get("dropped_cvars", []),
    }
    if defaults_toml and Path(defaults_toml).exists():
        eff, nondefault, unknown = effective_cvars(defaults_toml, config, argv)
        prov["cvars_effective"] = eff
        prov["cvars_nondefault"] = nondefault
        prov["cvars_unknown_on_argv"] = unknown
        prov["cvars_count"] = len(eff)
    return prov


# --------------------------------------------------------------------------
# analyzers
# --------------------------------------------------------------------------
def load_analyzer(name: str):
    path = HARNESS / "analyze" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"fr_an_{name}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def classify_signal(rc):
    """timeout(1) exit codes: 124 = our timeout fired (TERM), 137 = KILL after -k.
    128+N = child died of signal N. Distinguish our own kill from an external one."""
    if rc is None:
        return None
    if rc == 124:
        return "harness_timeout_term"
    if rc == 137:
        return "harness_timeout_kill"
    if rc > 128:
        return f"signal_{rc - 128}"
    return None


def finalize(scenario: str, run_dir: Path) -> dict:
    meta = read_json(run_dir / "run_meta.json", {}) or {}
    spec = SCENARIOS[scenario]
    prov = provenance(run_dir, meta)
    if meta.get("skipped") or meta.get("inconclusive"):
        kind = "SKIPPED" if meta.get("skipped") else "INCONCLUSIVE"
        v = {"schema": SCHEMA, "scenario": scenario, "run_dir": str(run_dir),
             "verdict": kind, "reasons": [meta.get("skipped") or meta.get("inconclusive")],
             "loaded": False,
             "measurements": {}, "criteria": {}, "provenance": prov}
        write_json(run_dir / "verdict.json", v)
        return v
    an = load_analyzer(spec["analyzer"])
    verdict, reasons, measurements, criteria = an.analyze(run_dir, meta)
    lmax = prov["load"].get("load1_max")
    loaded = bool(spec["flow"] and lmax is not None and lmax > load_max())
    if meta.get("interrupted"):
        verdict, reasons = "INCONCLUSIVE", reasons + ["harness interrupted"]
    sig = classify_signal(meta.get("rc"))
    # An external signal (not our timeout, not a guest-fault abort the
    # analyzer already judged) makes the run INCONCLUSIVE.
    if sig in ("signal_15", "signal_9", "signal_1", "signal_2") and verdict == "FAIL":
        verdict = "INCONCLUSIVE"
        reasons.append(f"process died from external {sig}")
    if loaded and verdict == "FAIL":
        verdict = "INCONCLUSIVE"
        reasons.append(f"host loaded: 1-min load max {lmax} > {load_max()} "
                       f"(a FAIL under load is not evidence against the binary)")
    v = {"schema": SCHEMA, "scenario": scenario, "scenario_name": spec["name"],
         "run_dir": str(run_dir), "verdict": verdict, "reasons": reasons,
         "loaded": loaded, "measurements": measurements, "criteria": criteria,
         "provenance": prov}
    write_json(run_dir / "verdict.json", v)
    return v


# --------------------------------------------------------------------------
# aggregation
# --------------------------------------------------------------------------
def run_dirs(scenario_dir: Path):
    """Final attempt of each run index: run-01, run-01.retry1, ... -> last."""
    by_idx = {}
    for d in sorted(scenario_dir.glob("run-*")):
        if not (d / "verdict.json").exists():
            continue
        idx = d.name.split(".")[0]
        by_idx.setdefault(idx, []).append(d)
    return {k: sorted(v, key=lambda p: (len(p.name), p.name)) for k, v in by_idx.items()}


def aggregate(scenario: str, scenario_dir: Path) -> dict:
    spec = SCENARIOS[scenario]
    runs, attempts = [], []
    for idx, ds in sorted(run_dirs(scenario_dir).items()):
        vs = [read_json(d / "verdict.json") for d in ds]
        attempts.extend({"run": d.name, "verdict": v["verdict"],
                         "loaded": v.get("loaded"),
                         "load1_max": v["provenance"]["load"].get("load1_max"),
                         "reasons": v["reasons"]} for d, v in zip(ds, vs))
        # A later attempt only exists because the earlier was INCONCLUSIVE.
        final = vs[-1]
        runs.append({"run": ds[-1].name, "verdict": final["verdict"],
                     "loaded": final.get("loaded"),
                     "measurements": final["measurements"]})
    counts = {k: sum(1 for r in runs if r["verdict"] == k)
              for k in ("PASS", "FAIL", "INCONCLUSIVE", "SKIPPED")}
    n = len(runs) - counts["SKIPPED"]
    need = min(spec["need"], max(n, 1))
    if n == 0:
        verdict = "SKIPPED" if runs else "INCONCLUSIVE"
    elif counts["PASS"] >= need:
        verdict = "PASS"
    elif counts["PASS"] + counts["INCONCLUSIVE"] < need:
        verdict = "FAIL"
    else:
        verdict = "INCONCLUSIVE"
    out = {"schema": SCHEMA, "scenario": scenario, "name": spec["name"],
           "verdict": verdict, "need_pass": need, "counts": counts,
           "runs": runs, "attempts": attempts}
    write_json(scenario_dir / "scenario.json", out)
    return out


def summary(out_dir: Path) -> dict:
    scen = {}
    for s in SCENARIOS:
        p = out_dir / s / "scenario.json"
        if p.exists():
            scen[s] = read_json(p)
    meta = read_json(out_dir / "invocation.json", {})
    # Passive inertness: every DC3 run's log must carry no RB3 hook line and
    # every RB3 run's log no DC3 hook line, whatever the scenario measured.
    inert = load_analyzer("inertness")
    passive = {}
    for s, forbid in (("S1", ["rb3"]), ("S1V", ["rb3"]), ("S2", ["rb3"]), ("S3", ["rb3"]),
                      ("S4", ["dc3"]), ("S5", ["dc3"])):
        for d in sorted((out_dir / s).glob("run-*")):
            log = d / "run.log"
            if log.exists():
                r = inert.scan(log, forbid)
                passive[f"{s}/{d.name}"] = {"forbid": forbid, "total": r["total"],
                                            "counts": {k: n for k, n in r["counts"].items() if n}}
    out = {"schema": SCHEMA, "invocation": meta,
           "verdicts": {s: v["verdict"] for s, v in scen.items()},
           "passive_inertness": passive,
           "scenarios": scen}
    write_json(out_dir / "summary.json", out)
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "defaults":
        print(defaults_dump(argv[2], argv[3]))
        return 0
    if cmd == "has-cvars":
        flat = flatten_toml(argv[2])
        missing = [c for c in argv[3:] if c not in flat]
        if missing:
            print(" ".join(missing))
            return 1
        return 0
    if cmd == "finalize":
        v = finalize(argv[2], Path(argv[3]))
        print(f"{argv[2]} {Path(argv[3]).name}: {v['verdict']}"
              + (" (loaded)" if v.get("loaded") else "")
              + ("" if not v["reasons"] else " -- " + "; ".join(v["reasons"])[:300]))
        return 0
    if cmd == "aggregate":
        v = aggregate(argv[2], Path(argv[3]))
        print(f"== {argv[2]} {v['verdict']}  {v['counts']}")
        return 0
    if cmd == "summary":
        v = summary(Path(argv[2]))
        for s, verdict in v["verdicts"].items():
            print(f"  {s:4s} {verdict}")
        return 0
    if cmd == "verdict-of":
        v = read_json(Path(argv[2]) / "verdict.json", {})
        print(v.get("verdict", "MISSING"))
        return 0
    print(f"unknown subcommand {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
