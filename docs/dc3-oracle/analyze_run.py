#!/usr/bin/env python3
"""Summarise a run_dc3_oracle.sh log: milestone -> approximate host time.

Time is taken from the nearest PRECEDING `Thread Status Report (<ms>ms)` line
(emitted every ~3 s), so each milestone is an upper-bounded-by-3s estimate.
"""
import re, sys
MILESTONES = [
    ("title_screen", re.compile(r"wait_screen 'title_screen' SATISFIED")),
    ("main_screen", re.compile(r"wait_screen 'main_screen' SATISFIED")),
    ("choose_mode_screen", re.compile(r"wait_screen 'choose_mode_screen' SATISFIED")),
    ("song_select_screen", re.compile(r"wait_screen 'song_select_screen' SATISFIED")),
    ("game_screen", re.compile(r"wait_screen 'game_screen' SATISFIED")),
    ("first gpState=2 (playing)", re.compile(r"gpState=2 .*paused=0")),
    ("first gpState=3", re.compile(r"gpState=3")),
]
ts_re = re.compile(r"Thread Status Report \((\d+)ms\).*SIGSEGV=(\d+)")
beat_re = re.compile(r"beat[=≈ ]+([0-9.]+)")
now, seen, segv = 0, {}, 0
gp2 = 0
for line in open(sys.argv[1], errors="replace"):
    m = ts_re.search(line)
    if m:
        now = int(m.group(1)); segv = max(segv, int(m.group(2))); continue
    if "gpState=2" in line and "paused=0" in line:
        gp2 += 1
    for name, rx in MILESTONES:
        if name not in seen and rx.search(line):
            seen[name] = now
for name, _ in MILESTONES:
    v = seen.get(name)
    print(f"{name:28s} {'-' if v is None else f'~{v/1000:6.1f}s (+0..3s)'}")
print(f"gpState=2&paused=0 samples: {gp2}; max SIGSEGV count: {segv}; last report: {now/1000:.1f}s")
