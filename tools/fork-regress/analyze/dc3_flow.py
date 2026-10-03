"""S1: DC3 original layout, ymca flow (docs/fork/dc3/analyze_run.py, extended).

Host time of a milestone = the nearest PRECEDING `Thread Status Report (<ms>ms)`
line (emitted every ~3 s), so every time is an upper bound good to +0..3 s.

Harness contracts read here (cleanup must keep them or update this file in the
same commit): `Thread Status Report (<ms>ms)… SIGSEGV=<n>`
(emulator_headless.cc), `DC3 FAULTS (<ms>ms): SIGSEGV=<n> XMA=<m>
NON_XMA=<k>` (titles/dc3/dc3_fail_tripwire.cc), `DC3 Script: wait_screen '<x>'
SATISFIED`, `DC3 Script: screen -> '<x>'` and `gpState=`
(titles/dc3/dc3_scripted_input.cc), `TIMEOUT: <ms>ms
reached` (headless main).

Fault gate: every guest store to the XMA register aperture [0x7FEA0000,
0x7FEB0000) is a trapped-and-emulated device register write, so with real XMA
audio (titles/dc3 since the XMA stub went, 2026-10-02) SIGSEGV counts
~100,000 benign faults per run. When the binary logs `DC3 FAULTS` lines the
criterion is max NON_XMA == 0; older binaries (no such line) are still gated on
max SIGSEGV == 0.
"""
import os
import re
from pathlib import Path

def _screen(name):
    # A flow's wait_screen, or (for a screen the flow does not wait on, e.g.
    # game_screen in dc3-decomp's ymca.txt) the adapter's screen-change line.
    return re.compile(rf"wait_screen '{name}' SATISFIED|DC3 Script: screen -> '{name}'")


MILESTONES = [
    ("title_screen", _screen("title_screen")),
    ("main_screen", _screen("main_screen")),
    ("choose_mode_screen", _screen("choose_mode_screen")),
    ("song_select_screen", _screen("song_select_screen")),
    ("game_screen", _screen("game_screen")),
    ("first_gpstate2_playing", re.compile(r"gpState=2 .*paused=0")),
    ("first_gpstate3", re.compile(r"gpState=3")),
]
TS_RE = re.compile(r"Thread Status Report \((\d+)ms\).*SIGSEGV=(\d+)")
FAULTS_RE = re.compile(r"DC3 FAULTS \((\d+)ms\): SIGSEGV=(\d+) XMA=(\d+) NON_XMA=(\d+)")
# Another thread's log prefix can land between "TIMEOUT: " and the number
# ('TIMEOUT: i> 001445C9 230000ms reachedTimeout of 230000ms reached, ...',
# measured 2026-10-03 on an S1V run, ~1000 lines before the end of the log, so
# outside timeout_from_tail's window).
TIMEOUT_RE = re.compile(r"TIMEOUT: (?:[a-zA-Z!]> [0-9A-Fa-f]{8} )?(\d+)ms reached")
FAILMSG_RE = re.compile(r"mFailThreadMsg=([0-9A-Fa-f]+) '([^']*)'")
TAINT_RE = re.compile(r"TAINTED")
SONG_RE = re.compile(r"DC3 Script: song '([^']*)'")

# The song S1's flow selects. Enforced when the binary logs the song (an
# adapter with the `screen ->` line); xenia-ymca.txt played `thehustle` for
# weeks while every S1 passed (measured 2026-10-02, lane B2, via S2's DTA
# query), so the song is a criterion, not a measurement.
EXPECT_SONG = os.environ.get("FR_DC3_EXPECT_SONG", "ymca")

# Pass criteria (plan §4.2 S1).
TITLE_MAX_S = 30.0
GAME_MAX_S = 60.0
GP2_MIN = 60



def timeout_from_tail(log, tail_bytes=16384):
    """`TIMEOUT: <ms>ms reached` is printed by the headless main thread while
    other threads are still logging, so it can be split across lines
    ('i> F8000004 XE_SWAPTIMEOUT: \\n230000ms reached', measured). Search the
    log's tail across line breaks."""
    try:
        with open(log, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - tail_bytes))
            tail = f.read().decode("utf-8", "replace")
    except OSError:
        return None
    m = re.search(r"TIMEOUT:.{0,400}?(\d+)ms reached", tail, re.S)
    return int(m.group(1)) if m else None

def parse(log: Path) -> dict:
    now, seen, segv, gp2, reports = 0, {}, 0, 0, 0
    xma, non_xma, fault_lines = 0, 0, 0
    timeout_line, fail_msgs, tainted = None, [], 0
    song, screen_lines = None, 0
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
            fl = FAULTS_RE.search(line)
            if fl:
                fault_lines += 1
                xma = max(xma, int(fl.group(3)))
                non_xma = max(non_xma, int(fl.group(4)))
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
            if song is None:
                sm = SONG_RE.search(line)
                if sm:
                    song = sm.group(1)
            if "DC3 Script: screen -> '" in line:
                screen_lines += 1
    if timeout_line is None:
        timeout_line = timeout_from_tail(log)
    return {
        "milestones_s": {n: (None if n not in seen else round(seen[n] / 1000, 1))
                         for n, _ in MILESTONES},
        "gpstate2_paused0_samples": gp2,
        "max_sigsegv": segv,
        # None = the binary does not log DC3 FAULTS (gate falls back to SIGSEGV).
        "max_xma_faults": xma if fault_lines else None,
        "max_non_xma_faults": non_xma if fault_lines else None,
        "status_reports": reports,
        "last_report_s": round(now / 1000, 1),
        "timeout_reached_ms": timeout_line,
        "mfailthreadmsg_seen": fail_msgs,
        "tainted_lines": tainted,
        # The song the game is playing (`DC3 Script: song`); None on a binary
        # whose adapter predates the line (no `screen ->` lines either).
        "song": song,
        "song_probe": screen_lines > 0,
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
        "rc": 0, "timeout_line": "present",
    }
    if m.get("max_non_xma_faults") is not None:
        crit["max_non_xma_faults"] = 0
    else:
        crit["max_sigsegv"] = 0
    if ms["title_screen"] is None or ms["title_screen"] > TITLE_MAX_S:
        reasons.append(f"title_screen {ms['title_screen']} s (want <= {TITLE_MAX_S})")
    if ms["game_screen"] is None or ms["game_screen"] > GAME_MAX_S:
        reasons.append(f"game_screen {ms['game_screen']} s (want <= {GAME_MAX_S})")
    if m["gpstate2_paused0_samples"] < GP2_MIN:
        reasons.append(f"gpState=2&paused=0 samples {m['gpstate2_paused0_samples']} < {GP2_MIN}")
    if ms["first_gpstate3"] is None:
        reasons.append("first gpState=3 never seen")
    if m.get("song_probe") and EXPECT_SONG:
        crit["song"] = EXPECT_SONG
        if m.get("song") != EXPECT_SONG:
            reasons.append(f"song {m.get('song')!r} != {EXPECT_SONG!r}")
    if rc != 0:
        reasons.append(f"rc {rc} != 0")
    if m["timeout_reached_ms"] is None:
        reasons.append("no 'TIMEOUT: <ms>ms reached' line (did not run to its own timeout)")
    if m.get("max_non_xma_faults") is not None:
        if m["max_non_xma_faults"] != 0:
            reasons.append(f"max NON_XMA faults {m['max_non_xma_faults']} != 0 "
                           f"(SIGSEGV {m['max_sigsegv']}, XMA {m['max_xma_faults']})")
    elif m["max_sigsegv"] != 0:
        reasons.append(f"max SIGSEGV {m['max_sigsegv']} != 0")
    return reasons, crit


def analyze(run_dir: Path, meta: dict):
    m = parse(run_dir / "run.log")
    rc = meta.get("rc")
    reasons, crit = judge(m, rc)
    m["failure_class"] = failure_class(m, rc) if reasons else None
    return ("PASS" if not reasons else "FAIL"), reasons, m, crit
