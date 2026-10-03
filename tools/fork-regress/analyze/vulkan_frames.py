"""S1V: DC3 original layout under Vulkan (GPU 1) with headless frame dump.

Measures: how many frames were captured (frame_<swap>.ppm, every
--headless_capture_interval swaps), at which swap indices, how far the ymca
flow got (dc3_flow milestones), and keeps one frame from game_screen (or the
furthest screen reached) as PNG for eyeballing.

PASS = rc 0 with the TIMEOUT line, title_screen reached, >= MIN_FRAMES frames,
and the kept frame is not a uniform fill. Reaching game_screen and the Milo
fail screen (Debug::Fail, "Program ended") are recorded, NOT required: on the BASELINE.md binary Vulkan gameplay failed 2/2 (the flow is
wall-clock driven and Vulkan is slower), so requiring it would make the
scenario red on the reference.

Contract: `VdSwap #<n>: … [CAPTURE]` (headless capture log line).
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

CAPTURE_RE = re.compile(r"VdSwap #(\d+):.*\[CAPTURE\]")
# A flow's wait_screen, or the adapter's screen-change line. The second is the
# only record of game_screen: the shared ymca flow never waits on it, so
# without it every gameplay capture stayed tagged multiuser_screen and the
# kept frame fell back to the last swap (post-song).
SAT_RE = re.compile(r"wait_screen '(\w+)' SATISFIED|DC3 Script: screen -> '(\w+)'")
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


def is_fail_screen(path: Path) -> bool:
    """The Milo Debug::Fail screen: white text on a flat dark red
    (mean ~(138, 5, 5)). It is what the MAIN thread draws forever after a
    MILO_FAIL -- 'Program ended'. Seen on every BASELINE.md Vulkan run from
    swap 1200 ('Could not find preview.tmov in dir song_info'), which is why
    those runs never reached game_screen."""
    try:
        from PIL import Image, ImageStat
        m = ImageStat.Stat(Image.open(path).convert("RGB").resize((160, 90))).mean
        return m[0] > 100 and m[1] < 30 and m[2] < 30
    except Exception:
        return False


def analyze(run_dir: Path, meta: dict):
    frames_dir = run_dir / "frames"
    ppms = sorted((p for p in frames_dir.glob("frame_*.ppm") if not p.name.endswith("_raw.ppm")),
                  key=lambda p: int(re.search(r"frame_(\d+)", p.name).group(1)))
    index = run_dir / "frames_index.json"
    fail_swaps = []
    if ppms:
        for p in ppms:
            if is_fail_screen(p):
                fail_swaps.append(int(re.search(r"frame_(\d+)", p.name).group(1)))
        index.write_text(json.dumps({"frames": [p.name for p in ppms],
                                     "fail_screen_swaps": fail_swaps}) + "\n")
    elif index.exists():
        # Re-analysis after the PPMs were pruned: count from the index; the
        # kept PNGs from the first analysis are still on disk.
        idx = json.loads(index.read_text())
        ppms = [frames_dir / n for n in idx["frames"]]
        fail_swaps = idx["fail_screen_swaps"]
    flow = dc3_flow.parse(run_dir / "run.log")
    # Order captures relative to screen milestones by LOG ORDER (no clocks).
    captures, screen_at_capture, current = [], {}, None
    with open(run_dir / "run.log", errors="replace") as f:
        for line in f:
            s = SAT_RE.search(line)
            if s:
                current = s.group(1) or s.group(2)
                continue
            c = CAPTURE_RE.search(line)
            if c:
                n = int(c.group(1))
                captures.append(n)
                screen_at_capture[n] = current
    on_disk = {int(re.search(r"frame_(\d+)", p.name).group(1)): p for p in ppms if p.exists()}
    listed = {int(re.search(r"frame_(\d+)", p.name).group(1)) for p in ppms}
    keep_screen, keep_idx = None, None
    for target in ("game_screen", "song_select_screen", "main_screen", "title_screen"):
        idxs = [n for n in captures if screen_at_capture.get(n) == target and n in listed]
        if idxs:
            keep_screen = target
            keep_idx = idxs[min(KEEP_AFTER_CAPTURES, len(idxs) - 1)]
            break
    if keep_idx is None and listed:
        keep_idx = max(listed)
    kept = {}
    reasons = []
    prior = sorted(run_dir.glob(f"kept_*_{keep_idx}.png")) if keep_idx is not None else []
    if keep_idx is not None:
        src = on_disk.get(keep_idx) or (prior[0] if prior else None)
        st, im = frame_stats(src) if src else ({"error": "frame pruned"}, None)
        kept = {"swap": keep_idx, "screen": keep_screen, **st}
        if im is not None:
            png = run_dir / f"kept_{keep_screen or 'last'}_{keep_idx}.png"
            if not png.exists():
                im.save(png)
            kept["png"] = str(png)
        if "stddev" in st and max(st["stddev"]) < 2.0:
            reasons.append(f"kept frame {keep_idx} is a uniform fill (stddev {st['stddev']})")
    if fail_swaps:
        first = min(fail_swaps)
        fpng = run_dir / f"fail_screen_{first}.png"
        src = frames_dir / f"frame_{first:04d}.ppm"
        if not fpng.exists() and src.exists():
            _, im = frame_stats(src)
            if im is not None:
                im.save(fpng)
    m = {"frames": len(ppms), "capture_swaps": captures[:400],
         "fail_screen_frames": len(fail_swaps),
         "fail_screen_first_swap": min(fail_swaps) if fail_swaps else None,
         "fail_screen_screen": (screen_at_capture.get(min(fail_swaps)) if fail_swaps else None),
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
            "informational": ["game_screen_reached", "capture_swaps",
                              "fail_screen_first_swap"]}
    return ("PASS" if not reasons else "FAIL"), reasons, m, crit
