"""S2: DC3 DTA channel round trip (plan §4.2 S2) + the S1 flow on the same run.

Contracts: `DC3 DTA channel: installed on`, `DC3 DTA channel: first poll on guest
thread <tid>` (dc3_dta_channel.cc); reply body `=> <value>` /
`=> !! refused: script error...` (the RB3Enhanced /dta/eval contract).
"""
import json
import os
import re
from pathlib import Path

import importlib.util

_spec = importlib.util.spec_from_file_location(
    "fr_dc3_flow", Path(__file__).with_name("dc3_flow.py"))
dc3_flow = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dc3_flow)

FIRST_POLL_RE = re.compile(r"DTA channel: first poll on guest thread ([0-9A-Fa-f]+)")
INSTALLED_RE = re.compile(r"DC3 DTA channel: installed on")

# dc3_eval.py prints each command's value with the wire's "=> " prefix
# stripped (split_results); raw xchan.py keeps it. Accept both.
EXPECT = {
    "{+ 1 2}": ("exact", "3"),
    "{no_such_func 1}": ("prefix", "!! refused: script error"),
    "{+ 5 5}": ("exact", "10"),
    "{size {object_list main Object FALSE}}": ("int", None),
}

# The song the flow must select (S2 criterion). Empty = record only.
EXPECT_SONG = os.environ.get("FR_DTA_EXPECT_SONG", "")


def strip_prefix(out: str) -> str:
    return out[3:] if out.startswith("=> ") else out


def analyze(run_dir: Path, meta: dict):
    reasons = []
    text = (run_dir / "run.log").read_text(errors="replace")
    installed = bool(INSTALLED_RE.search(text))
    fp = FIRST_POLL_RE.search(text)
    drv = {}
    try:
        drv = json.loads((run_dir / "driver.json").read_text())
    except (OSError, ValueError):
        reasons.append("driver.json missing (driver did not finish)")
    m = {"installed": installed, "first_poll_thread": fp.group(1) if fp else None,
         "driver_error": drv.get("error"), "first_poll_wait_s": drv.get("first_poll_wait_s"),
         "answers": {}, "latency_s": {}}
    if not installed:
        reasons.append("no 'DC3 DTA channel: installed on' line")
    if not fp:
        reasons.append("channel never polled")
    elif fp.group(1) != "00000006":
        reasons.append(f"first poll on guest thread {fp.group(1)}, want 00000006 (main)")
    if drv.get("error"):
        reasons.append(f"driver: {drv['error']}")
    got = {q["q"]: q for q in drv.get("queries", [])}
    for q, (kind, want) in EXPECT.items():
        r = got.get(q)
        if r is None:
            if not drv.get("error"):
                reasons.append(f"{q}: not sent")
            continue
        out = strip_prefix(r["stdout"].strip())
        m["answers"][q] = out
        m["latency_s"][q] = r["latency_s"]
        ok = (kind == "exact" and out == want) or \
             (kind == "prefix" and out.startswith(want)) or \
             (kind == "int" and re.fullmatch(r"\d+", out) is not None)
        if not ok:
            reasons.append(f"{q} -> {out!r} (rc {r['rc']}, {r['stderr'][:120]!r})")
    if "{size {object_list main Object FALSE}}" in m["answers"]:
        mm = re.fullmatch(r"(\d+)", m["answers"]["{size {object_list main Object FALSE}}"])
        m["object_list_main_count"] = int(mm.group(1)) if mm else None
    # Gameplay queries (recorded; the song is a criterion only when
    # FR_DTA_EXPECT_SONG names one).
    m["game_error"] = drv.get("game_error")
    m["game_answers"] = {q["q"]: strip_prefix(q["stdout"].strip())
                         for q in drv.get("game_queries", [])}
    m["song"] = m["game_answers"].get("{gamedata get song}")
    want_song = EXPECT_SONG
    if want_song and m["song"] != want_song:
        reasons.append(f"song {m['song']!r} != {want_song!r} "
                       f"(game_error {m['game_error']!r})")
    flow = dc3_flow.parse(run_dir / "run.log")
    flow_reasons, flow_crit = dc3_flow.judge(flow, meta.get("rc"))
    m["flow"] = flow
    m["flow_pass"] = not flow_reasons
    reasons += [f"flow: {r}" for r in flow_reasons]
    crit = {"installed": True, "first_poll_thread": "00000006",
            "answers": {q: (k, w) for q, (k, w) in EXPECT.items()},
            "flow": flow_crit, "song": EXPECT_SONG}
    return ("PASS" if not reasons else "FAIL"), reasons, m, crit
