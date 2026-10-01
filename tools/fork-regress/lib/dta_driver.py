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


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


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
            t = time.time()
            try:
                p = subprocess.run([PY, DC3_EVAL, "-T", "xenia", "--socket", str(sock),
                                    "--timeout", "60", q],
                                   capture_output=True, text=True, timeout=90)
                out["queries"].append({"q": q, "rc": p.returncode,
                                       "stdout": p.stdout.strip()[:2000],
                                       "stderr": p.stderr.strip()[:2000],
                                       "latency_s": round(time.time() - t, 3)})
            except subprocess.TimeoutExpired:
                out["queries"].append({"q": q, "rc": None, "stdout": "",
                                       "stderr": "client timeout 90 s",
                                       "latency_s": round(time.time() - t, 3)})
    (rd / "driver.json").write_text(json.dumps(out, indent=2) + "\n")


if __name__ == "__main__":
    main()
