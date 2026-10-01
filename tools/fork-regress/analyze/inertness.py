"""S6: cross-title hook inertness. A title's hooks must leave no trace in another
title's run: no DC3 hook line in an RB3/DC1 log, no RB3 hook line in a DC3/DC1
log.

Variants (run index -> variant, see scenarios/S6.sh):
  1  DC1 TU0, 60 s, no title cvars        -> no DC3 and no RB3 lines
  2  DC3 ymca flow WITH every RB3 cvar on  -> no RB3 lines
  3  RB3DX flow WITH every DC3 cvar on     -> no DC3 lines

Known leaks on today's binaries (plan §4.2 S6): the headless thread-6
"present pipeline" dump for ANY title (emulator_headless.cc), the
nop_input_driver screen-aware paths with any script file, xam_nui. So S6 is
FAIL on the reference binaries; the comparator treats each pattern's count as
a RATCHET (candidate <= baseline), and Phase 2 must take them to zero.
"""
import re
from pathlib import Path

# Strip the xenia log prefix ("i> F8000028 ") so patterns anchor on the message.
PREFIX_RE = re.compile(r"^[a-zA-Z!]> [0-9A-Fa-f]{8} ")

DC3_PATTERNS = {
    "DC3 hook line": re.compile(r"^(DC3[: ]|\[dc3-debug\]|Dc3)"),
    "DC3 script/gpState probe": re.compile(r"(DC3 Script:|gpState=)"),
    "DC3 thread-6 present-pipeline dump": re.compile(
        r"(Function probes \(present pipeline\)|Indirection\[0x83A00964\]|Thunk\[0x83A00964\]|"
        r"\bmainCRTStartup\b)"),
    "DC3 DTA channel": re.compile(r"DTA channel"),
}
RB3_PATTERNS = {
    "RB3 hook line": re.compile(r"^(RB3[: ]|RB3DX|SI [A-Za-z])"),
    "RB3 stream census": re.compile(r"STREAM-CENSUS"),
}
OTHER_PATTERNS = {
    "TITLE-PROFILE": re.compile(r"TITLE-PROFILE"),
}

VARIANTS = {
    1: {"title": "dc1", "forbid": ["dc3", "rb3"]},
    2: {"title": "dc3", "forbid": ["rb3"]},
    3: {"title": "rb3dx", "forbid": ["dc3"]},
}
FAMILIES = {"dc3": DC3_PATTERNS, "rb3": RB3_PATTERNS}
EXAMPLES = 3


def scan(log: Path, forbid: list[str]) -> dict:
    pats = {}
    for fam in forbid:
        for name, rx in FAMILIES[fam].items():
            pats[f"{fam}: {name}"] = rx
    counts = {k: 0 for k in pats}
    examples = {k: [] for k in pats}
    with open(log, errors="replace") as f:
        for line in f:
            msg = PREFIX_RE.sub("", line.strip(), count=1)
            for k, rx in pats.items():
                if rx.search(msg):
                    counts[k] += 1
                    if len(examples[k]) < EXAMPLES:
                        examples[k].append(line.strip()[:200])
    return {"forbid": forbid, "counts": counts,
            "examples": {k: v for k, v in examples.items() if v},
            "total": sum(counts.values())}


def analyze(run_dir: Path, meta: dict):
    variant = int(meta.get("variant") or 0)
    if variant not in VARIANTS:
        return "FAIL", [f"unknown S6 variant {variant}"], {}, {}
    v = VARIANTS[variant]
    s = scan(run_dir / "run.log", v["forbid"])
    reasons = [f"{n} x {k}" for k, n in s["counts"].items() if n]
    m = {"variant": variant, "title": v["title"], **s}
    if meta.get("rc") not in (0, None):
        m["rc_note"] = f"rc {meta.get('rc')} (not a criterion: inertness reads the log only)"
    crit = {"other_title_lines": 0, "ratchet": "per-pattern count must not exceed baseline"}
    return ("PASS" if not reasons else "FAIL"), reasons, m, crit
