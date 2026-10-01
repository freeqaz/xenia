"""S1V: DC3 original layout under Vulkan (GPU 1) with headless frame dump.

Measures: how many frames were captured (frame_<swap>.ppm, every
--headless_capture_interval swaps), at which swap indices, how far the ymca
flow got (dc3_flow milestones), and keeps one frame from game_screen (or the
furthest screen reached) as PNG for eyeballing.

PASS = rc 0 with the TIMEOUT line, title_screen reached, >= MIN_FRAMES frames,
and the kept frame is not a uniform fill. Reaching game_screen is recorded but
NOT required: on the BASELINE.md binary Vulkan gameplay failed 2/2 (the flow is
wall-clock driven and Vulkan is slower), so requiring it would make the
scenario red on the reference.

Contract: `VdSwap #<n>: … [CAPTURE]` (headless capture log line).
"""
import os
import re
from pathlib import Path

import importlib.util

_spec = importlib.util.spec_from_file_location(
    "fr_dc3_flow", Path(__file__).with_name("dc3_flow.py"))
dc3_flow = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dc3_flow)

CAPTURE_RE = re.compile(r"VdSwap #(\d+):.*\[CAPTURE\]")
SAT_RE = re.compile(r"wait_screen '(\w+)' SATISFIED")
MIN_FRAMES = 10
# Swaps after game_screen SATISFIED before the kept frame: past the transition.
KEEP_AFTER_CAPTURES = 2


def frame_stats(path: Path):
    try:
        from PIL import Image, ImageStat
        im = Image.open(path).convert("RGB")
        st = ImageStat.Stat(im)
        return {"size": list(im.size), "mean": [round(x, 1) for x in st.mean],
                "stddev": [round(x, 1) for x in st.stddev]}, im
    except Exception as e:  # PIL missing or unreadable frame
        return {"error": str(e)}, None


def analyze(run_dir: Path, meta: dict):
    frames_dir = run_dir / "frames"
    ppms = sorted((p for p in frames_dir.glob("frame_*.ppm") if not p.name.endswith("_raw.ppm")),
                  key=lambda p: int(re.search(r"frame_(\d+)", p.name).group(1)))
    flow = dc3_flow.parse(run_dir / "run.log")
    # Order captures relative to screen milestones by LOG ORDER (no clocks).
    captures, screen_at_capture, current = [], {}, None
    with open(run_dir / "run.log", errors="replace") as f:
        for line in f:
            s = SAT_RE.search(line)
            if s:
                current = s.group(1)
                continue
            c = CAPTURE_RE.search(line)
            if c:
                n = int(c.group(1))
                captures.append(n)
                screen_at_capture[n] = current
    on_disk = {int(re.search(r"frame_(\d+)", p.name).group(1)): p for p in ppms}
    keep_screen, keep_idx = None, None
    for target in ("game_screen", "song_select_screen", "main_screen", "title_screen"):
        idxs = [n for n in captures if screen_at_capture.get(n) == target and n in on_disk]
        if idxs:
            keep_screen = target
            keep_idx = idxs[min(KEEP_AFTER_CAPTURES, len(idxs) - 1)]
            break
    if keep_idx is None and on_disk:
        keep_idx = max(on_disk)
    kept = {}
    reasons = []
    if keep_idx is not None:
        st, im = frame_stats(on_disk[keep_idx])
        kept = {"swap": keep_idx, "screen": keep_screen, **st}
        if im is not None:
            png = run_dir / f"kept_{keep_screen or 'last'}_{keep_idx}.png"
            im.save(png)
            kept["png"] = str(png)
        if "stddev" in st and max(st["stddev"]) < 2.0:
            reasons.append(f"kept frame {keep_idx} is a uniform fill (stddev {st['stddev']})")
    m = {"frames": len(ppms), "capture_swaps": captures[:400],
         "captures_logged": len(captures), "kept_frame": kept, "flow": flow,
         "game_screen_reached": flow["milestones_s"]["game_screen"] is not None}
    if meta.get("rc") != 0:
        reasons.append(f"rc {meta.get('rc')} != 0")
    if flow["timeout_reached_ms"] is None:
        reasons.append("no 'TIMEOUT: <ms>ms reached' line")
    if flow["milestones_s"]["title_screen"] is None:
        reasons.append("title_screen never reached")
    if len(ppms) < MIN_FRAMES:
        reasons.append(f"{len(ppms)} frames < {MIN_FRAMES}")
    if os.environ.get("FR_KEEP_FRAMES", "0") != "1":
        for p in frames_dir.glob("*.ppm"):
            p.unlink()
    crit = {"rc": 0, "timeout_line": "present", "title_screen": "reached",
            "frames_ge": MIN_FRAMES, "kept_frame": "not uniform",
            "informational": ["game_screen_reached", "capture_swaps"]}
    return ("PASS" if not reasons else "FAIL"), reasons, m, crit
