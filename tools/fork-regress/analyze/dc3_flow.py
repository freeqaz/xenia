"""S1: DC3 original layout, ymca flow (docs/fork/dc3/analyze_run.py, extended).

Host time of a milestone = the nearest PRECEDING `Thread Status Report (<ms>ms)`
line (emitted every ~3 s), so every time is an upper bound good to +0..3 s.

Harness contracts read here (cleanup must keep them or update this file in the
same commit): `Thread Status Report (<ms>ms)… SIGSEGV=<n>`
(emulator_headless.cc), `DC3 FAULTS (<ms>ms): SIGSEGV=<n> XMA=<m>
NON_XMA=<k>` (titles/dc3/dc3_fail_tripwire.cc), `DC3 Script: wait_screen '<x>'
SATISFIED`, `DC3 Script: screen -> '<x>'` and `gpState=`
(titles/dc3/dc3_scripted_input.cc), `TIMEOUT: <ms>ms
reached` (headless main), `MMIO soft-fault read from unmapped guest <addr>
(... guest lr <lr> ...)` (cpu/mmio_handler.cc) and `crash_guest=0x<pc>` on the
Thread Status Report line (emulator_headless.cc), both read only to recognise a
known original-game race (KNOWN_RACES below).

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
# The same branch of emulator_headless.cc first logs a whole XELOGI line,
# 'Timeout of <ms>ms reached, terminating...', which cannot be split; it is
# accepted too (S1V x2 run-02, 2026-10-03: the cout line was split by ten
# XMA lines).
TIMEOUT_RE = re.compile(r"TIMEOUT: (?:[a-zA-Z!]> [0-9A-Fa-f]{8} )?(\d+)ms reached"
                        r"|Timeout of (\d+)ms reached, terminating")
FAILMSG_RE = re.compile(r"mFailThreadMsg=([0-9A-Fa-f]+) '([^']*)'")
TAINT_RE = re.compile(r"TAINTED")
SONG_RE = re.compile(r"DC3 Script: song '([^']*)'")
# Kinect HLE (kernel/nui): resolution and the frame clock's counters.
NUI_RESOLVED_RE = re.compile(r"NUI HLE: NUI (\S+) resolved (\d+)/(\d+)")
NUI_CLOCK_RE = re.compile(r"NUI HLE: frame clock ([0-9.]+)/s produced=(\d+) "
                          r"events=(\d+) get_next=(\d+) served=(\d+)")
UNMAPPED_READ_RE = re.compile(r"MMIO soft-fault read from unmapped guest ([0-9A-Fa-f]{8}) \(([^)]*)\)")
GUEST_LR_RE = re.compile(r"guest lr ([0-9A-Fa-f]{8})")
CRASH_GUEST_RE = re.compile(r"crash_guest=0x([0-9A-Fa-f]+)")

# Known ORIGINAL-GAME races (README.md, "Known original-game races"). A run
# whose only failure is one of these is INCONCLUSIVE with reason
# `known_game_race:<name>` (so --retry re-runs it, and summary.json counts it),
# never PASS and never a plain FAIL.
#
# splash_postprocessor_uaf: the splash thread walks TheRnd.mPostProcessors in
# Rnd::DoWorldEnd while the main thread's NgSpotlightDrawer::Init frees the
# default SpotlightDrawer's list node; `++it` follows the freed node's first
# word (now the pool's free-list link) and the next iteration faults on the
# vtable read `lwz r11,0(r3)` -> `lwz r11,8(r11)` at 0x82662B18, a read of
# guest 0x00000008, with lr 0x82662B24 (the return from the previous
# EndWorld call). The match is all of: exactly one unmapped-read
# line in the log and it is that one (address 00000008, guest lr 82662B24),
# logged before title_screen; every crash_guest the status reports name is
# 0x82662B18; and exactly one non-XMA fault (one SIGSEGV on a binary without
# `DC3 FAULTS` lines). Any second fault, a different PC or lr, or the same
# read after title_screen keeps the plain FAIL.
KNOWN_RACE_PREFIX = "known_game_race:"
SPLASH_UAF = dict(name="splash_postprocessor_uaf", read_addr="00000008",
                  guest_lr="82662B24", crash_guest="82662B18")
KNOWN_RACES = [SPLASH_UAF]

# The song S1's flow selects. Enforced when the binary logs the song (an
# adapter with the `screen ->` line); xenia-ymca.txt played `thehustle` for
# weeks while every S1 passed (measured 2026-10-02, lane B2, via S2's DTA
# query), so the song is a criterion, not a measurement.
EXPECT_SONG = os.environ.get("FR_DC3_EXPECT_SONG", "ymca")

# Pass criteria (plan §4.2 S1).
TITLE_MAX_S = 30.0
GAME_MAX_S = 60.0
GP2_MIN = 60



# 1 MiB: on S1V with inline render the XMA threads keep logging after the
# timeout, and the TIMEOUT line was measured ~1000 lines (~70 KB) before the
# end of the log (2026-10-03), split by a whole XmaContext line.
def timeout_from_tail(log, tail_bytes=1 << 20):
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
    nui_resolved, nui_clock = None, None
    unmapped_reads, unmapped_count, crash_guests = [], 0, set()
    last_line = ""
    with open(log, errors="replace") as f:
        for line in f:
            last_line = line
            m = TS_RE.search(line)
            if m:
                now = int(m.group(1))
                segv = max(segv, int(m.group(2)))
                reports += 1
                cg = CRASH_GUEST_RE.search(line)
                if cg:
                    crash_guests.add(f"{int(cg.group(1), 16):08X}")
                continue
            if "soft-fault read from unmapped" in line:
                ur = UNMAPPED_READ_RE.search(line)
                if ur:
                    unmapped_count += 1
                    if len(unmapped_reads) < 8:
                        lr = GUEST_LR_RE.search(ur.group(2))
                        unmapped_reads.append({
                            "addr": ur.group(1).upper(),
                            "guest_lr": lr.group(1).upper() if lr else None,
                            "before_title_screen": "title_screen" not in seen,
                            "host_s_upper_bound": round(now / 1000, 1)})
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
                timeout_line = int(t.group(1) or t.group(2))
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
            if "NUI HLE:" in line:
                nr = NUI_RESOLVED_RE.search(line)
                if nr:
                    nui_resolved = f"{nr.group(1)} {nr.group(2)}/{nr.group(3)}"
                nc = NUI_CLOCK_RE.search(line)
                if nc:
                    nui_clock = {"rate": float(nc.group(1)),
                                 "produced": int(nc.group(2)),
                                 "events": int(nc.group(3)),
                                 "get_next": int(nc.group(4)),
                                 "served": int(nc.group(5))}
    if timeout_line is None:
        timeout_line = timeout_from_tail(log)
    m = {
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
        # Kinect HLE: "<NUI version> <resolved>/<entries>" and the last
        # frame-clock counters; None on a binary without kernel/nui.
        "nui_resolved": nui_resolved,
        "nui_frame_clock": nui_clock,
        # The song the game is playing (`DC3 Script: song`); None on a binary
        # whose adapter predates the line (no `screen ->` lines either).
        "song": song,
        "song_probe": screen_lines > 0,
        "last_line": last_line.strip()[:200],
        # Guest faults by site, for the known-race match (first 8 lines kept).
        "unmapped_reads": unmapped_reads,
        "unmapped_read_count": unmapped_count,
        "crash_guests": sorted(crash_guests),
    }
    m["known_game_race"] = known_game_race(m)
    return m


def known_game_race(m: dict) -> str | None:
    """Name of the known original-game race this run's faults match exactly,
    else None. See KNOWN_RACES."""
    faults = (m["max_non_xma_faults"] if m.get("max_non_xma_faults") is not None
              else m["max_sigsegv"])
    for race in KNOWN_RACES:
        if (m["unmapped_read_count"] == 1
                and m["unmapped_reads"][0]["addr"] == race["read_addr"]
                and m["unmapped_reads"][0]["guest_lr"] == race["guest_lr"]
                and m["unmapped_reads"][0]["before_title_screen"]
                and m["crash_guests"] == [race["crash_guest"]]
                and faults == 1):
            return race["name"]
    return None


def apply_known_race(verdict: str, reasons: list, flow: dict):
    """A FAIL whose faults are exactly a known original-game race becomes
    INCONCLUSIVE; the race reason goes first, the original reasons stay."""
    race = flow.get("known_game_race")
    if verdict == "FAIL" and race:
        return "INCONCLUSIVE", [KNOWN_RACE_PREFIX + race] + list(reasons)
    return verdict, reasons


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
    verdict, reasons = apply_known_race("PASS" if not reasons else "FAIL", reasons, m)
    if verdict == "INCONCLUSIVE":
        m["failure_class"] = KNOWN_RACE_PREFIX + m["known_game_race"]
    return verdict, reasons, m, crit
