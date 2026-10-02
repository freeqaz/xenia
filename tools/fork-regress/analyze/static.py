"""S0: static. Records the binary's compiled-in cvar defaults (full map) and
source ratchets on a tree; PASS unless a measurement could not be taken. The
comparator turns both into regressions: a changed default must be listed, a
ratchet must not increase (plan §4.2 S0).

Ratchets (plan §4.2, counted with git grep so untracked files never count):
  title_id_literals_outside_titles  0x373307D9|0x45410914 in src/ outside src/xenia/titles/
  home_free_literals_in_src         '/home/free' in src/
  xelogi_in_gpu                     XELOGI( in src/xenia/gpu/

Allow-list (Phase 3): title IDs belong under src/xenia/titles/ only. Every
line outside it that matches TITLE_ID_RE (case-insensitive, with or without
0x, plus the kTitleDc3/kTitleRb3 constants) must be covered by an entry in
scenarios/S0.title-id-allowlist (path, max line count, reason). An uncovered
line FAILS S0 outright -- it is not a ratchet against a baseline.
"""
import subprocess
from pathlib import Path

import importlib.util

_spec = importlib.util.spec_from_file_location(
    "fr_core", Path(__file__).resolve().parent.parent / "lib" / "fr.py")
fr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fr)


def git_grep_count(tree, pattern, paths, exclude=None, fixed=False):
    args = ["git", "-C", tree, "grep", "-I", "-c"]
    args += ["-F"] if fixed else ["-E"]
    args += ["-e", pattern, "--"] + paths
    if exclude:
        args += [f":(exclude){exclude}"]
    p = subprocess.run(args, capture_output=True, text=True)
    if p.returncode not in (0, 1):
        return None
    return sum(int(l.rsplit(":", 1)[1]) for l in p.stdout.splitlines() if ":" in l)


TITLE_ID_RE = r"(0x)?(373307D9|45410914)|kTitle(Dc3|Rb3)"
ALLOWLIST = Path(__file__).resolve().parent.parent / "scenarios" / "S0.title-id-allowlist"


def read_allowlist(path):
    allowed = {}
    for raw in Path(path).read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = raw.split("\t")
        if len(parts) < 3 or not parts[1].strip().isdigit() or not parts[2].strip():
            raise ValueError(f"bad allow-list line (want path<TAB>count<TAB>reason): {raw!r}")
        allowed[parts[0].strip()] = int(parts[1])
    return allowed


def title_id_allowlist_check(tree):
    """Per-file counts of title-ID lines outside src/xenia/titles/, against the
    allow-list. Returns (measurements, reasons)."""
    p = subprocess.run(["git", "-C", tree, "grep", "-I", "-i", "-c", "-E", "-e", TITLE_ID_RE,
                        "--", "src", ":(exclude)src/xenia/titles/*"],
                       capture_output=True, text=True)
    if p.returncode not in (0, 1):
        return None, ["title-ID allow-list grep failed"]
    found = {}
    for l in p.stdout.splitlines():
        f, n = l.rsplit(":", 1)
        found[f] = int(n)
    try:
        allowed = read_allowlist(ALLOWLIST)
    except (OSError, ValueError) as e:
        return None, [f"title-ID allow-list unreadable: {e}"]
    unlisted = {f: n - allowed.get(f, 0) for f, n in found.items() if n > allowed.get(f, 0)}
    stale = {f: {"allowed": a, "found": found.get(f, 0)}
             for f, a in allowed.items() if found.get(f, 0) < a}
    reasons = [f"title-ID literal outside src/xenia/titles/ not in the allow-list: "
               f"{f} (+{n} line(s))" for f, n in sorted(unlisted.items())]
    m = {"found": found, "allowed": allowed, "unlisted": unlisted, "stale_entries": stale,
         "unlisted_lines": sum(unlisted.values())}
    return m, reasons


def analyze(run_dir: Path, meta: dict):
    reasons = []
    dfl = meta.get("defaults_toml")
    defaults = {}
    try:
        defaults = {k: fr.norm_value(v) for k, v in sorted(fr.flatten_toml(dfl).items())}
    except Exception as e:
        reasons.append(f"defaults dump unreadable: {e}")
    tree = meta.get("src_tree")
    ratchets = {}
    if tree:
        ratchets = {
            "title_id_literals_outside_titles": git_grep_count(
                tree, "0x373307D9|0x45410914", ["src"], exclude="src/xenia/titles/*"),
            "home_free_literals_in_src": git_grep_count(tree, "/home/free", ["src"], fixed=True),
            "xelogi_in_gpu": git_grep_count(tree, "XELOGI(", ["src/xenia/gpu"], fixed=True),
        }
        if any(v is None for v in ratchets.values()):
            reasons.append("a ratchet grep failed")
        title_ids, why = title_id_allowlist_check(tree)
        reasons += why
    else:
        title_ids = None
    m = {"cvar_count": len(defaults), "cvar_defaults": defaults,
         "src_tree": tree, "src_git": fr.git_rev(tree) if tree else {}, "ratchets": ratchets,
         "title_id_allowlist": title_ids}
    crit = {"defaults_dump": "readable", "ratchets": "comparator: must not increase",
            "cvar_defaults": "comparator: every changed default is reported",
            "title_id_allowlist": "0 title-ID lines outside src/xenia/titles/ beyond "
                                  "scenarios/S0.title-id-allowlist"}
    return ("PASS" if not reasons else "FAIL"), reasons, m, crit
