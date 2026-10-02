"""S4 (clean retail TU5) and S5 (RB3DX): menu chain to gameplay entry.

Contracts read (emulator.cc RB3 UI probe): `RB3DX UI PROBE[n]: transState=<s>
curScreen=0x…'<name>' transScreen=0x…'<name>'`, `STREAM-CENSUS 0x… mState=<m>
recv=…(n=<r>) chans=…(n=<c>)`, `RB3: app-run-direct installed`,
`RB3: mogg-key-table installed`, `FAULT_LIVELOCK_ABORT`, and the headless
`Thread Status Report (<ms>ms)` clock (time = preceding report, +0..3 s).

S4 has two criteria sets. With the mogg key table installed (the content dir
derived it), gameplay: game_screen at transState=0 AND the song stream (the
widest StandardStream censused after game_screen) SEEN at mState=3 (kPlaying)
with receivers == channels at least once (a song that plays to the end then
moves on to mState=6, so the latest or highest state is not the test). Before
the §8x fix the song stream sat at mState=0 with
an EMPTY receiver vector (InitInfo never ran), so this is the §8x signal. The
channel count depends on which song the autopilot lands on (s66: 11 on
tv3_a; the post-s66 seed lands on a 14-channel song via tv3_c), so it is
recorded, not required. Without the key, menu-only: reach tv3_*_screen and see
the transition to game_screen begin.
"""
import re
from pathlib import Path

TS_RE = re.compile(r"Thread Status Report \((\d+)ms\).*SIGSEGV=(\d+)")
# Headless binaries since Lane C also print XMA=<m> NON_XMA=<k> on that line.
NON_XMA_RE = re.compile(r"Thread Status Report .* NON_XMA=(\d+)")
UI_RE = re.compile(r"UI PROBE\[(\d+)\]: transState=(\d+) curScreen=\S+'([^']*)' "
                   r"transScreen=\S+'([^']*)'")
STREAM_RE = re.compile(r"STREAM-CENSUS 0x([0-9A-Fa-f]+) mState=(\d+) .*?\(n=(\d+)\).*?\(n=(\d+)\)")
AUTOPILOT_RE = re.compile(r"UI PROBE\[\d+\]: autopilot (\S+)")
TIMEOUT_RE = re.compile(r"TIMEOUT: (\d+)ms reached")

TU5_MENU = ["main_hub_screen", "song_select_screen", "part_difficulty_screen", "tv3_*"]
DX_MENU = ["main_hub_screen", "song_select_screen"]



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
    now, seen, timeline, last = 0, {}, [], None
    stream_max = {}  # (stream addr) -> highest (mState, recv, chans) after game_screen
    # Streams EVER seen playing after game_screen: mState 3 (kPlaying) with
    # receivers == channels > 2. A song that plays to the end moves on to a
    # later state (6), so the highest tuple alone is not the criterion.
    stream_played = set()
    game_t0 = None
    installs = {"app_run_direct": False, "mogg_key_table": False,
                "mogg_key_table_rejected": False, "no_char_preview": False}
    livelock = 0
    autopilot = 0
    timeout_ms = None
    max_segv = 0
    max_non_xma = None  # None: the binary does not print NON_XMA
    trans_to_game = None
    with open(log, errors="replace") as f:
        for line in f:
            m = TS_RE.search(line)
            if m:
                now = int(m.group(1))
                max_segv = max(max_segv, int(m.group(2)))
                nx = NON_XMA_RE.search(line)
                if nx:
                    max_non_xma = max(max_non_xma or 0, int(nx.group(1)))
                continue
            m = UI_RE.search(line)
            if m:
                st, cur, trans = int(m.group(2)), m.group(3), m.group(4)
                key = (st, cur, trans)
                if key != last:
                    timeline.append({"t_s": round(now / 1000, 1), "probe": int(m.group(1)),
                                     "transState": st, "cur": cur, "trans": trans})
                    last = key
                for name in (cur, trans):
                    if name and name != "<null>":
                        seen.setdefault(name, round(now / 1000, 1))
                if trans == "game_screen" and trans_to_game is None:
                    trans_to_game = round(now / 1000, 1)
                if cur == "game_screen" and st == 0 and game_t0 is None:
                    game_t0 = round(now / 1000, 1)
                continue
            m = STREAM_RE.search(line)
            if m and game_t0 is not None:
                cand = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
                if cand[0] == 3 and cand[1] == cand[2] > 2:
                    stream_played.add(m.group(1))
                prev = stream_max.get(m.group(1))
                if prev is None or cand > prev:
                    stream_max[m.group(1)] = cand
                continue
            if "RB3: app-run-direct installed" in line:
                installs["app_run_direct"] = True
            elif "RB3: mogg-key-table installed" in line:
                installs["mogg_key_table"] = True
            elif "RB3: mogg-key-table NOT installed" in line:
                installs["mogg_key_table_rejected"] = True
            elif "UpdateCharCache no-op override installed" in line:
                installs["no_char_preview"] = True
            if "FAULT_LIVELOCK_ABORT" in line:
                livelock += 1
            if AUTOPILOT_RE.search(line):
                autopilot += 1
            t = TIMEOUT_RE.search(line)
            if t:
                timeout_ms = int(t.group(1))
    if timeout_ms is None:
        timeout_ms = timeout_from_tail(log)
    playing = sorted(({"stream": k, "mState": v[0], "recv": v[1], "chans": v[2],
                       "played": k in stream_played}
                      for k, v in stream_max.items()),
                     key=lambda d: (-d["played"], -d["mState"], -d["recv"]))
    tv3 = sorted(n for n in seen if re.fullmatch(r"tv3_\w+_screen", n))
    return {"screens_first_seen_s": seen, "timeline": timeline,
            "tv3_screens": tv3, "transition_to_game_screen_s": trans_to_game,
            "game_screen_entered_s": game_t0, "streams_after_game_screen": playing,
            "installs": installs, "livelock_aborts": livelock,
            "autopilot_actions": autopilot, "timeout_reached_ms": timeout_ms,
            "max_sigsegv": max_segv, "max_non_xma_faults": max_non_xma, "last_report_s": round(now / 1000, 1)}


def analyze(run_dir: Path, meta: dict):
    m = parse(run_dir / "run.log")
    argv = " ".join(meta.get("argv", []))
    tu5 = "--rb3_tu5_app_run_direct" in argv
    reasons = []
    crit = {"livelock_aborts": 0, "rc": 0, "timeout_line": "present"}
    if tu5:
        mode = "gameplay" if "--rb3_mogg_key_table=" in argv else "menu_only"
        m["mode"] = mode
        crit["mode"] = mode
        crit["installs"] = ["app_run_direct"] + (["mogg_key_table"] if mode == "gameplay" else [])
        if not m["installs"]["app_run_direct"]:
            reasons.append("no 'RB3: app-run-direct installed'")
        if mode == "gameplay" and not m["installs"]["mogg_key_table"]:
            reasons.append("no 'RB3: mogg-key-table installed'")
        crit["screens"] = TU5_MENU
        for s in TU5_MENU:
            if s == "tv3_*":
                if not m["tv3_screens"]:
                    reasons.append("never reached a tv3_*_screen")
            elif s not in m["screens_first_seen_s"]:
                reasons.append(f"never reached {s}")
        if mode == "menu_only":
            crit["transition_to_game_screen"] = "begins"
            if m["transition_to_game_screen_s"] is None:
                reasons.append("transition to game_screen never began")
        else:
            crit["game_screen"] = "transState=0"
            crit["stream"] = ("widest stream after game_screen: seen at mState=3 "
                              "with recv == chans > 2")
            streams = m["streams_after_game_screen"]
            song = max(streams, key=lambda d: (d["chans"], d["played"]), default=None)
            m["song_stream"] = song
            if m["game_screen_entered_s"] is None:
                reasons.append("game_screen never reached transState=0")
            elif not song or not song["played"]:
                reasons.append(f"song stream not playing after game_screen (saw {streams[:3]})")
    else:
        m["mode"] = "rb3dx"
        crit["screens"] = DX_MENU
        crit["game_screen"] = "transState=0"
        for s in DX_MENU:
            if s not in m["screens_first_seen_s"]:
                reasons.append(f"never reached {s}")
        if m["game_screen_entered_s"] is None:
            reasons.append("game_screen never reached transState=0")
    if m["livelock_aborts"]:
        reasons.append(f"{m['livelock_aborts']} FAULT_LIVELOCK_ABORT lines")
    if meta.get("rc") != 0:
        reasons.append(f"rc {meta.get('rc')} != 0")
    if m["timeout_reached_ms"] is None:
        reasons.append("no 'TIMEOUT: <ms>ms reached' line")
    # The timeline can be long; keep the first 60 transitions.
    m["timeline"] = m["timeline"][:60]
    return ("PASS" if not reasons else "FAIL"), reasons, m, crit
