"""S0: static. Records the binary's compiled-in cvar defaults (full map) and
source ratchets on a tree; PASS unless a measurement could not be taken. The
comparator turns both into regressions: a changed default must be listed, a
ratchet must not increase (plan §4.2 S0).

Ratchets (plan §4.2, counted with git grep so untracked files never count):
  title_id_literals_outside_titles  0x373307D9|0x45410914 in src/ outside src/xenia/titles/
  home_free_literals_in_src         '/home/free' in src/
  xelogi_in_gpu                     XELOGI( in src/xenia/gpu/
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
    m = {"cvar_count": len(defaults), "cvar_defaults": defaults,
         "src_tree": tree, "src_git": fr.git_rev(tree) if tree else {}, "ratchets": ratchets}
    crit = {"defaults_dump": "readable", "ratchets": "comparator: must not increase",
            "cvar_defaults": "comparator: every changed default is reported"}
    return ("PASS" if not reasons else "FAIL"), reasons, m, crit
