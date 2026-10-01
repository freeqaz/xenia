"""S1: DC3 original layout, ymca flow (docs/dc3-oracle/analyze_run.py, extended).

Host time of a milestone = the nearest PRECEDING `Thread Status Report (<ms>ms)`
line (emitted every ~3 s), so every time is an upper bound good to +0..3 s.

Harness contracts read here (cleanup must keep them or update this file in the
same commit): `Thread Status Report (<ms>ms)… SIGSEGV=<n>`
(emulator_headless.cc), `DC3 Script: wait_screen '<x>' SATISFIED` and
`gpState=` (nop_input_driver.cc), `TIMEOUT: <ms>ms reached` (headless main).
"""
import re
from pathlib import Path

MILESTONES = [
    ("title_screen", re.compile(r"wait_screen 'title_screen' SATISFIED")),
    ("main_screen", re.compile(r"wait_screen 'main_screen' SATISFIED")),
    ("choose_mode_screen", re.compile(r"wait_screen 'choose_mode_screen' SATISFIED")),
    ("song_select_screen", re.compile(r"wait_screen 'song_select_screen' SATISFIED")),
    ("game_screen", re.compile(r"wait_screen 'game_screen' SATISFIED")),
    ("first_gpstate2_playing", re.compile(r"gpState=2 .*paused=0")),
    ("first_gpstate3", re.compile(r"gpState=3")),
]
TS_RE = re.compile(r"Thread Status Report \((\d+)ms\).*SIGSEGV=(\d+)")
TIMEOUT_RE = re.compile(r"TIMEOUT: (\d+)ms reached")
FAILMSG_RE = re.compile(r"mFailThreadMsg=([0-9A-Fa-f]+) '([^']*)'")
TAINT_RE = re.compile(r"TAINTED")

# Pass criteria (plan §4.2 S1).
TITLE_MAX_S = 30.0
GAME_MAX_S = 60.0
GP2_MIN = 60


def parse(log: Path) -> dict:
    now, seen, segv, gp2, reports = 0, {}, 0, 0, 0
    timeout_line, fail_msgs, tainted = None, [], 0
    last_line = ""
    with open(log, errors="replace") as f:
        for line in f:
            last_line = line
            m = TS_RE.search(line)
            if m:
                now = int(m.group(1))
                segv = max(segv, int(m.group(2)))
                reports += 1
                continue
            if "gpState=2" in line and "paused=0" in line:
                gp2 += 1
            for name, rx in MILESTONES:
                if name not in seen and rx.search(line):
                    seen[name] = now
            t = TIMEOUT_RE.search(line)
            if t:
                timeout_line = int(t.group(1))
            fm = FAILMSG_RE.search(line)
            if fm and fm.group(2) not in fail_msgs:
                fail_msgs.append(fm.group(2))
            if TAINT_RE.search(line):
                tainted += 1
    return {
        "milestones_s": {n: (None if n not in seen else round(seen[n] / 1000, 1))
                         for n, _ in MILESTONES},
        "gpstate2_paused0_samples": gp2,
        "max_sigsegv": segv,
        "status_reports": reports,
        "last_report_s": round(now / 1000, 1),
        "timeout_reached_ms": timeout_line,
        "mfailthreadmsg_seen": fail_msgs,
        "tainted_lines": tainted,
        "last_line": last_line.strip()[:200],
    }


def failure_class(m: dict, rc) -> str | None:
    ms = m["milestones_s"]
    if rc not in (0, None) and rc != 124:
        return f"process_exit_rc{rc}"
    if ms["title_screen"] is None:
        return "boot_hang_or_no_title"
    if ms["game_screen"] is None:
        return "flow_stall_before_game_screen"
    if m["gpstate2_paused0_samples"] < GP2_MIN:
        return "gameplay_short"
    return None


def judge(m: dict, rc, timeout_ms_expected=None):
    reasons = []
    ms = m["milestones_s"]
    crit = {
        "title_screen_le_s": TITLE_MAX_S, "game_screen_le_s": GAME_MAX_S,
        "gpstate2_paused0_samples_ge": GP2_MIN, "first_gpstate3": "seen",
        "rc": 0, "timeout_line": "present", "max_sigsegv": 0,
    }
    if ms["title_screen"] is None or ms["title_screen"] > TITLE_MAX_S:
        reasons.append(f"title_screen {ms['title_screen']} s (want <= {TITLE_MAX_S})")
    if ms["game_screen"] is None or ms["game_screen"] > GAME_MAX_S:
        reasons.append(f"game_screen {ms['game_screen']} s (want <= {GAME_MAX_S})")
    if m["gpstate2_paused0_samples"] < GP2_MIN:
        reasons.append(f"gpState=2&paused=0 samples {m['gpstate2_paused0_samples']} < {GP2_MIN}")
    if ms["first_gpstate3"] is None:
        reasons.append("first gpState=3 never seen")
    if rc != 0:
        reasons.append(f"rc {rc} != 0")
    if m["timeout_reached_ms"] is None:
        reasons.append("no 'TIMEOUT: <ms>ms reached' line (did not run to its own timeout)")
    if m["max_sigsegv"] != 0:
        reasons.append(f"max SIGSEGV {m['max_sigsegv']} != 0")
    return reasons, crit


def analyze(run_dir: Path, meta: dict):
    m = parse(run_dir / "run.log")
    rc = meta.get("rc")
    reasons, crit = judge(m, rc)
    m["failure_class"] = failure_class(m, rc) if reasons else None
    return ("PASS" if not reasons else "FAIL"), reasons, m, crit
