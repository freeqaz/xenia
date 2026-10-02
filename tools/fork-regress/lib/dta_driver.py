#!/usr/bin/env python3
"""S2 driver: wait for the DC3 DTA channel's first main-thread poll, then send
the round-trip queries through the real client
(dc3-decomp/tools/console/dc3_eval.py -T xenia --socket <sock>).

usage: dta_driver.py <run-dir> <xenia-pid>
Writes <run-dir>/driver.json. Bounded: gives up 150 s after start if the
channel never polls, and each query has its own 60 s client timeout.
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

DC3_EVAL = os.environ.get(
    "DC3_EVAL", "/home/free/code/milohax/dc3-decomp/tools/console/dc3_eval.py")
PY = os.environ.get("PY", "/usr/bin/python3")
QUERIES = [
    "{+ 1 2}",
    "{no_such_func 1}",
    "{+ 5 5}",
    "{size {object_list main Object FALSE}}",
]
WAIT_FIRST_POLL_S = float(os.environ.get("FR_DTA_WAIT_S", 150))
# Let the guest get past boot before the object census (the spike's 702 came
# from a run already on the menus).
CENSUS_DELAY_S = float(os.environ.get("FR_DTA_CENSUS_DELAY_S", 0))
# Gameplay queries: sent once the song is playing (the first `gpState=2 ...
# paused=0` probe line), plus GAME_DELAY_S. Read-only. They pin which song the
# flow selected and which animation HamDirector::SongAnim hands the dancers.
GAME_QUERIES = [
    "{gamedata get song}",
    "{gamedata getp 0 difficulty}",
    "{hamprovider get merge_moves}",
    "{{$hamdirector player_song_anim 0} name}",
    "{{$hamdirector player_song_anim 0} num_keys $hamdirector (clip)}",
    "{{{$hamdirector get_world} find player_1_routine_builder.anim} "
    "num_keys $hamdirector (clip)}",
    "{{$hamdirector difficulty_song_anim 2} num_keys $hamdirector (clip)}",
]
GAME_DELAY_S = float(os.environ.get("FR_DTA_GAME_DELAY_S", 10))
# Exploration only: FR_DTA_SCREEN_PROBE="<screen>|<query>|<seconds>" sends
# <query> back to back for <seconds> once the adapter logs `screen -> '<screen>'`
# (answers land in driver.json "screen_probe" with host timestamps).
SCREEN_PROBE = os.environ.get("FR_DTA_SCREEN_PROBE", "")
WAIT_GAME_S = float(os.environ.get("FR_DTA_WAIT_GAME_S", 150))


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def send(sock, q):
    t = time.time()
    try:
        p = subprocess.run([PY, DC3_EVAL, "-T", "xenia", "--socket", str(sock),
                            "--timeout", "60", q],
                           capture_output=True, text=True, timeout=90)
        return {"q": q, "rc": p.returncode, "stdout": p.stdout.strip()[:2000],
                "stderr": p.stderr.strip()[:2000],
                "latency_s": round(time.time() - t, 3)}
    except subprocess.TimeoutExpired:
        return {"q": q, "rc": None, "stdout": "", "stderr": "client timeout 90 s",
                "latency_s": round(time.time() - t, 3)}


def main():
    rd, pid = Path(sys.argv[1]), int(sys.argv[2])
    sock = rd / "dta.sock"
    out = {"socket": str(sock), "client": DC3_EVAL, "queries": [],
           "first_poll_wait_s": None, "error": None}
    t0 = time.time()
    log = rd / "run.log"
    while True:
        if not alive(pid):
            out["error"] = "xenia exited before the channel polled"
            break
        try:
            if sock.exists() and "DTA channel: first poll" in log.read_text(errors="replace"):
                out["first_poll_wait_s"] = round(time.time() - t0, 1)
                break
        except OSError:
            pass
        if time.time() - t0 > WAIT_FIRST_POLL_S:
            out["error"] = f"no first poll within {WAIT_FIRST_POLL_S:.0f} s"
            break
        time.sleep(1)
    if out["error"] is None:
        for q in QUERIES:
            if q.startswith("{size") and CENSUS_DELAY_S:
                time.sleep(CENSUS_DELAY_S)
            out["queries"].append(send(sock, q))
    if out["error"] is None and SCREEN_PROBE:
        scr, pq, secs = SCREEN_PROBE.split("|")
        out["screen_probe"] = []
        t1 = time.time()
        while time.time() - t1 < 150 and alive(pid):
            try:
                if f"screen -> '{scr}'" in log.read_text(errors="replace"):
                    break
            except OSError:
                pass
            time.sleep(0.05)
        t2 = time.time()
        while time.time() - t2 < float(secs) and alive(pid):
            r = send(sock, pq)
            r["t"] = round(time.time() - t2, 3)
            out["screen_probe"].append(r)
    out["game_queries"] = []
    out["game_error"] = None
    if out["error"] is None:
        t1 = time.time()
        while True:
            if not alive(pid):
                out["game_error"] = "xenia exited before gameplay"
                break
            try:
                txt = log.read_text(errors="replace")
            except OSError:
                txt = ""
            if any("gpState=2" in ln and "paused=0" in ln
                   for ln in txt.splitlines()):
                out["game_wait_s"] = round(time.time() - t1, 1)
                break
            if time.time() - t1 > WAIT_GAME_S:
                out["game_error"] = f"no gpState=2 playing within {WAIT_GAME_S:.0f} s"
                break
            time.sleep(1)
        if out["game_error"] is None:
            time.sleep(GAME_DELAY_S)
            for q in GAME_QUERIES:
                out["game_queries"].append(send(sock, q))
    (rd / "driver.json").write_text(json.dumps(out, indent=2) + "\n")


if __name__ == "__main__":
    main()
