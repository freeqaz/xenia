# DC3 Headless Render Stabilization — Alternating 3D Resolve

**Status:** OPEN. The DC3 song-playing scene (dancers + venue) renders, but **intermittently**:
roughly every *other* captured frame resolves the 3D scene to the visible/captured buffer; the
interleaved captured frames show only the viewport CLEAR colors + the Kinect HUD boxes. The
camera/lighting animate between good frames, so the scene is LIVE — this is a present/resolve
stability bug, not a content bug. Goal: every captured frame resolves the 3D scene, so per-frame
IK/skeleton telemetry is deterministic.

Branch: `headless-vulkan-linux` (xenia repo). Related: `xenia-vulkan-rendering` memory,
`project-xenia-async-stall` memory (cont. 12), `dc3-decomp/docs/runtime/XENIA_ASYNC_COMPLETION_STALL.md`.

---

## Architecture (as currently understood — VERIFY before relying)

Headless capture is a **2-frame handshake** driven from `VulkanCommandProcessor::IssueSwap`
(`src/xenia/gpu/vulkan/vulkan_command_processor.cc:1329`), `presenter == nullptr` branch:

1. **Top of every swap:** if `deferred_draws_` non-empty → `FlushDeferredDraws()` (replays the
   recorded draws). Then `EndSubmission(true)`.
2. Frame bookkeeping; decide `should_capture` (every `headless_capture_interval_` frames) and
   `next_is_capture` (the frame *before* a capture).
3. On a `next_is_capture` swap → set `deferred_draws_enabled_ = true` + `SetWarmupWait(true)` so
   the NEXT frame's `IssueDraw`/`IssueCopy` calls are *recorded* into `deferred_draws_` instead of
   executed.
4. On a `should_capture` swap → the deferred draws recorded during the render frame were flushed at
   step 1; then PHASE 1 submits readback (texture load + image copy of `frontbuffer_ptr`), PHASE 2
   (next swap) reads the pixels → PPM.

`IssueDraw` (line 2639): when `deferred_draws_enabled_`, pushes a `DeferredDrawState` instead of
drawing. `IssueCopy` (line 3303) is the EDRAM→shared-memory resolve.

**`FlushDeferredDraws` (line 5069), current WIP behavior — runs on EVERY flush:**
`EndSubmission(true)` → `AwaitAllQueueOperationsCompletion()` → `render_target_cache_->ClearCache()`
(destroys VkFramebuffers/render passes — added to stop `device_lost`) → `ResetState()`
(`DestroyAllRenderTargets(false)`) → `BeginFrame()` → `vkCmdFillBuffer` zero the whole EDRAM buffer
(fence-waited) → replay each deferred draw via `IssueDraw(...)` → `deferred_draws_.clear()`.

So **every capture frame destroys + recreates all host render targets and zeroes EDRAM before
replaying the draws.** The original `xenia-vulkan-rendering` note called this "first-flush-only" to
avoid a 2nd-`ResetState` `device_lost`; the current tree does it on every flush (device_lost solved
via `ClearCache()` first). The alternating resolve likely lives in this teardown ↔ resolve/present
interaction.

## GROUND TRUTH (log-mined from nopause2 run — NO new GPU run needed)

Mined `/tmp/xenia-boot-logs/nopause2/run.log` (the known-good "dancers" run, capture_interval=200)
+ visually classified `nopause2/png/*`. Hard facts:

1. **The 3D scene is DRAWN + RESOLVED on EVERY flush.** `FlushDeferredDraws` summary lines are
   consistent across all captures: **~1023–1382 draws, 23–24 copies (resolves) every flush** — they
   do NOT alternate with the good/bad visual pattern. So the scene geometry + EDRAM resolves happen
   every frame. This RULES OUT "draws/resolves suppressed on bad frames" and the EDRAM-zero-races-
   draws hypothesis (the draws all succeed regardless).
2. **Captures always read the SAME frontbuffer address.** VdSwap double-buffers by parity: even
   VdSwap → `ptr=0x1E830000`, odd VdSwap → `ptr=0x1EBC8000`. Captures fire at multiples of 200
   (all even) → **always read `0x1E830000`**. Both good (9600) and bad (9400) captures read the same
   address. RULES OUT "capture reads the wrong/alternating buffer" (hypothesis D).
3. **HUD always renders; 3D world only periodically.** Bad captured frames (e.g. `all_9400.png`,
   ~120 KB) show the HUD move-cards over TWO split viewport CLEARS (left=black, right=blue) but NO
   dancers/venue. Good captured frames (`all_9600.png`, ~450 KB) show the full 3D world. The two
   split clear-color halves + HUD are present EVERY captured frame.
4. **Periodicity ≈ 1 good in 8–10 captures**, not strictly "every other" (cont.12 was approximate).
   PNG sizes: full-scene spikes (245–453 KB) interleaved with HUD-only (~115–170 KB).

**Leading hypothesis (H1 — front-buffer persistence wiped by per-flush teardown):** DC3 redraws the
HUD into the front buffer every present but re-renders the 3D world into `0x1E830000` only every
~8–10 frames, relying on the front buffer PERSISTING between scene re-renders (normal HW behavior).
Our `FlushDeferredDraws` **zeroes the whole EDRAM buffer + DestroyAllRenderTargets on every flush**,
destroying the persisted scene. So a capture only shows the 3D world on the exact frame the game
happened to re-render it; otherwise it catches HUD-over-cleared-buffer. The `xenia-vulkan-rendering`
memory's original "first-flush-only" guard existed precisely to avoid this; the current tree lost it
(now teardown runs every flush — `deferred_flush_count_` is incremented but NOT used to gate).

**Cheapest decisive experiment (A/B):** gate the EDRAM-zero + ClearCache + ResetState block in
`FlushDeferredDraws` to **first-flush-only** (`if (deferred_flush_count_ == 1)`), rebuild, re-run the
nopause capture. Prediction if H1 is right: the 3D world persists → appears on EVERY captured frame
(possibly with the old menu-ghosting tradeoff returning, which is acceptable for telemetry). If it
instead goes all-ghost/black → teardown is load-bearing and H1 is wrong.

## Ranked hypotheses (UNDERSTAND workflow synthesis — 3 parallel Opus analyses + synthesis, 2026-06-03)

All three analysts independently top-ranked a **double-buffer A/B ping-pong** theory (resolve writes
render frame's `RB_COPY_DEST_BASE`, readback reads capture frame's fetch-0 base, one swap apart).
Synthesis **refuted it (H4, 0.08)** using the GROUND TRUTH above (captures always read `0x1E830000`;
pattern is ~1-in-8-10 not every-other; draws/resolves present every flush) and promoted:

- **H1 (0.78) — per-flush teardown destroys front-buffer persistence.** DC3 redraws the HUD into the
  swap buffer (`0x1E830000`) every present but re-renders+re-resolves the 3D world into it only every
  ~8–10 guest frames, relying on the host VkImage PERSISTING between scene re-renders (normal Xbox360
  HW). `FlushDeferredDraws` unconditionally zeroes the whole EDRAM buffer (vkCmdFillBuffer ~5154) +
  `ClearCache()`+`ResetState()` (5113-5117) every flush. On the majority of captures the deferred
  batch did NOT re-resolve the world, so no resolve fires the swap texture's watch → `base_outdated_`
  stays false → `LoadTextureData` short-circuits → the host VkImage is whatever it last held, but the
  persisted scene that would still be on HW was wiped → HUD + clears only. `deferred_flush_count_` is
  incremented (5094) but **never used to gate** — the original "first-flush-only" guard was lost.
- **H2 (0.70) — stale-swap-texture short-circuit is the proximate present gate.** `RequestSwapTexture`
  → `LoadTextureData` re-uploads ONLY when `base_outdated_`/`mips_outdated_` set (texture_cache.cc:650).
  Composes with H1: if teardown were gated, a false `base_outdated_` would just present the still-valid
  persisted image (good).
- **H3 (0.32) — EDRAM-zero corrupts resolves from PRE-EXISTING tiles.** `ResetState` empties
  `ownership_ranges_`; `DumpRenderTargets` skips empty ranges → a resolve whose source tiles were
  owned by a prior frame's RT dumps nothing into the just-zeroed EDRAM → copies zeros. Explains the
  exact 50% (461359/921600) plateau (one RT/half resolves live, the other from zeroed tiles). Secondary.
- **H4 (0.08, REFUTED) — double-buffer ping-pong.** Kept only to close it explicitly with the run.

## Instrumentation (APPLIED — 3 RSTAB lines, grep token `RSTAB:`)

1. `vulkan_command_processor.cc` FlushDeferredDraws, after `ok = IssueCopy(); copy_count++;` (~5269):
   `RSTAB: REPLAY_COPY flush#{} copy#{} dest_base=0x{RB_COPY_DEST_BASE} ok={}` — every replayed resolve
   TARGET this flush. Q: does ANY copy target the swap base `0x1E830000` on good flushes vs none on bad?
2. `texture_cache.cc` LoadTextureData, before the early-return (~650): `RSTAB: LOAD base_page=...
   base_outdated=... mips_outdated=... decision={SKIP_STALE|UPLOAD}` — proximate present gate (H2).
3. `vulkan_command_processor.cc` IssueSwap, after the nonzero-pixel summary (~1681): `RSTAB: CAPTURE
   frame={} flush#={} frontbuffer=0x{} nonzero_pct={} verdict={SCENE|HUD_ONLY}` (SCENE if nonzero>15%) —
   the ground-truth good/bad label + join key (flush#, frame).

**Decision rule (join on flush#):**
- Every SCENE flush has a `REPLAY_COPY dest_base=0x1E830000` AND a `LOAD base_page=0x1E830 ... UPLOAD`,
  while every HUD_ONLY flush has NO copy to `0x1E830000` AND `LOAD ... SKIP_STALE` → **H1+H2 confirmed**.
- A HUD_ONLY flush WITH a copy to `0x1E830000` yet `UPLOAD` + low nonzero → resolve copied zeroed EDRAM
  → **H3**.
- `LOAD base_page` differs between good/bad → **H4 revives** (not expected).

## Candidate fixes (choose after the run confirms)

- **Cheap (H1 revert):** gate the teardown block (ClearCache+ResetState+BeginFrame 5113-5121 AND the
  EDRAM fill 5126-5186) behind `if (deferred_flush_count_ == 1)` — restores "first-flush-only".
  Reintroduces the menu/transition ghosting the teardown was added to fix (acceptable for telemetry per
  the old `xenia-vulkan-rendering` note, but not ideal).
- **Durable (H2):** keep teardown for ghosting; force the swap texture to re-upload every capture
  (force `base_outdated_`=true / `RangeWrittenByGpu` over the frontbuffer extent before `LoadTextureData`),
  so the host VkImage always reloads the persisted scene from shared memory (NOT zeroed — only EDRAM is).
  Lower-risk variant: read back the buffer the world-resolve actually wrote (`RB_COPY_DEST_BASE`) when
  it differs from fetch-0.
- **If H3 dominant for the 50% plateau:** gate ONLY the EDRAM `vkCmdFillBuffer` to first-flush-only
  (keep ClearCache/ResetState which fix device_lost), OR route the manual fill through
  `UseEdramBuffer(kComputeWrite)` so `edram_buffer_usage_` stays in sync.

## Experiment log

### ⚠️ METRIC CORRECTION (read first)
The in-engine `nonzero_pct` verdict (count of pixels with r|g|b != 0) is **MISLEADING**: a solid blue
CLEAR screen is 100% "nonzero". Confirmed visually — rstab2 frame_1600 logged `nonzero_pct=100
verdict=SCENE` but is **entirely blue clear + the 2 Kinect calibration HUD boxes, zero 3D content**.
**Use PNG file size as the scene-content proxy instead** (high-frequency geometry detail compresses
large): `<10 KB` = total clear (calibration gap); `~120–170 KB` = HUD move-cards over the split
black|blue clears (BAD — no scene); `>250 KB` = real dancers+venue scene (GOOD). The RSTAB `verdict`
field is unreliable; the `RSTAB: REPLAY_COPY` and `RSTAB: LOAD` correlations below are still valid.

### rstab1 — baseline (per-flush teardown, the buggy WIP), gpu=1, interval=200
RSTAB decision rule fired clean and **refuted H1/H2, confirmed H3**:
- **All 45 flushes** have a `RSTAB: REPLAY_COPY dest_base=0x1E830000` (the resolve to the swap buffer
  runs every flush) AND **all 45** `RSTAB: LOAD base_page=0x1E830 ... UPLOAD` (swap texture re-uploads
  every flush, never SKIP_STALE). So the scene is NOT missing due to persistence/staleness — **the
  resolve copies zeroed/partial EDRAM** because the per-flush `ResetState`+EDRAM-zero wiped source
  tiles. By PNG size: only **2** real-scene frames (>250 KB) in the gameplay window.

### rstab2 — persist fix (`dc3_persist_render_state=true` default, teardown first-flush-only), gpu=1, interval=200
- By PNG size: **5 CONSECUTIVE** real-scene frames (frames 6000–6800, 269–559 KB) vs rstab1's 2
  scattered. HUD-only frames also reduced. **The persist fix is a real but PARTIAL improvement** —
  KEEP it (correct in principle: stops the EDRAM-zero from corrupting resolves of persisted tiles).
- **Remaining core symptom (NOT fixed):** most gameplay frames are still HUD-only — the screen is
  split into two viewport halves (LEFT black clear, RIGHT blue clear) with the HUD move-cards drawn
  over both, and the **dancer/venue geometry intermittently absent from BOTH halves**. Real scene
  appears only in BURSTS (~5 frames, then reverts to HUD-only for ~10). Visual proof: rstab2
  `frame_6400.png` (full scene: male+female dancer, venue, lighting, cards) vs `frame_8000.png`
  (split black|blue + cards only, a faint horizon sliver, no dancers/venue).

### Next question (the real remaining bug)
Every flush DOES resolve to 0x1E830000 and re-uploads, so it's not present-side. The **deferred-draw
REPLAY produces the 3D scene geometry only in bursts**; otherwise the resolve's EDRAM source for the
scene region is empty (split clears + HUD survive because they're screen-space/self-contained). The
two-viewport split (black|blue) is the key structural clue.

**KEY LOG FINDING (rstab2, scene flush#32/frame6400/551KB vs HUD flush#40/frame8000/127KB):**
- **Identical resolve sets** — both flushes have exactly 24 REPLAY_COPYs to the SAME 14 distinct
  `dest_base` addresses, same counts. NO scene-specific resolve is missing on HUD frames.
- **Constant draw counts** — ~1260–1410 draws/flush across BOTH scene bursts (6000–6800) and HUD-only
  frames (7000–9000), zero correlation with scene presence.
- ⇒ **Draws + resolves replay byte-identically; the difference is purely in draw OUTPUT / resolve
  SOURCE content.** Rules out "missing draws/resolves." Points at **draw DEPENDENCIES**: (a) textures
  not resident (deferred replay holds `set_suppress_memory_watches(true)` — scene textures may not be
  re-fetched/uploaded most frames), (b) an intermediate render-to-texture the scene samples that's only
  populated in bursts, or (c) shader constants (skinning/bone matrices, camera) only valid in bursts.
- Burst cadence: ~5 captured frames ON (1000 VdSwaps) then ~10 OFF — a periodic dependency.
- **Texture residency WEAKENED (hyp a):** `RSTAB: LOAD` shows ~23–30 UPLOADs/flush, roughly CONSTANT
  across scene-burst (flush 30–34) and HUD-only (flush 35–45) flushes (19759 SKIP_STALE / 1264 UPLOAD
  overall; 339 distinct textures all reload at least once). Textures aren't the burst gate. ⇒ leading
  candidates are (b) intermediate RT the scene samples, or (c) per-draw render-state (depth-buffer not
  cleared so scene geometry depth-fails vs HUD which is depth-off; or camera/skinning constants stale).

Open for the burst-diagnose workflow: instrument the SCENE draws' resource bindings (textures resident?
which RT source?) + per-resolve `copy_dest_extent` to find what the scene resolve reads on HUD vs burst
flushes. The 14 dest_base addresses (0x1380A/0x1527E/0x158E6/0x1794F/0x1D3A7/0x1E1E8/0x1E324/0x1E325/
0x1E32B/0x1E498/0x1E830/0x1E844/0x1EF60/0x1F2F8) are candidate scene/RTT/HUD/shadow targets — map them.

### rstab3 — RSTAB2 ownership + draw-binding instrumentation (persist on), gpu=1, interval=200
Decision rule fired and **refuted BOTH rank-1 and rank-2** (synthesis branch 3):
- **Rank-1 (ownership) REFUTED:** `RSTAB2: RESOLVE_DUMP` shows the scene-region resolves (dest_base
  0x1E324/325/32B ×9 + 0x1E830000 ×1) find owning host RTs **every flush** — `scene_rects_sum=10`,
  INVARIANT across SCENE flushes (30,32,36,39,40) and HUD flushes (31,33,34,35,37,38,41–45). RTs are
  owned + dumped + resolved every flush. `first_rt_tiles`: the 0x1E32x intermediate RTs own EDRAM
  tile 0; the 0x1E830000 swap composite owns EDRAM **tile 608**.
- **Rank-2 (scene-draw presence) REFUTED:** `RSTAB2: REPLAY_DRAW` geometry-draw (idx≥64) histogram is
  IDENTICAL between SCENE flush 32 and HUD flush 33 — **~306 geometry draws bind color RT tile 608**
  (the scene composite) on both, plus ~32 draws to tile 0 (intermediate RTs) on both.
- ⇒ **The scene geometry draws EXECUTE, bind the correct RT (tile 608), which is owned + resolved to
  0x1E830000 — invariantly every flush — yet produce EMPTY output (split black|blue clear) on HUD
  flushes.** The cause is NOT missing draws/resolves/ownership. It is **render-state or DATA read at
  REPLAY time that is not in the per-draw register snapshot**: the deferred replay runs one frame AFTER
  recording, so the geometry's guest-memory vertex/constant data (camera view-proj, skinning/bone
  matrices) and/or depth/viewport state may be mutated by the game thread by the time the recorded
  draws replay → geometry transforms off-screen/degenerate or depth-fails → RT keeps its clear color.
  The persist fix helps (RT/EDRAM persists so a recent-good render can survive) but doesn't make the
  current frame's geometry valid. Burst cadence = window where the recorded draws' guest data is still
  valid before the game overwrites its (double/triple-buffered) constant/vertex buffers.

### Confirmed NON-causes (search bounded)
present-side staleness/LoadTextureData • double-buffer ping-pong • missing draws • missing/mis-targeted
resolves • missing texture uploads • RT ownership at resolve • scene-draw presence. ⇒ the remaining
cause is replay-time guest-data/render-state staleness (a deferred-capture architecture hazard).

### Refinement (rules out dynamic-vertex staleness; points to GLOBAL EDRAM state)
The ~306 geometry draws that bind the scene RT (EDRAM tile 608 → 0x1E830000) ARE the dancers + venue,
rendered directly into the swap composite. On HUD flushes all 306 execute but render nothing. CRUCIAL:
the **static venue disappears together with the skinned dancers** — so it is NOT dynamic-vertex-buffer
staleness (static venue verts don't change frame-to-frame). It's a **GLOBAL state affecting all
geometry uniformly**. Viewport/scissor/depth-control regs ARE in the per-draw register snapshot (so
restored), which leaves **EDRAM CONTENT not captured by the register snapshot**: the **depth buffer
tiles** (geometry depth-fails against stale depth → nothing writes) or an intermediate-RT/sample the
pass needs. This fits the persist fix partially helping (depth/RT EDRAM persists vs per-flush zero) and
the two-viewport black|blue split (two clear passes survive; the geometry layer over them is rejected).
**Decisive next experiment:** force-clear the scene depth buffer (or set depth-test to ALWAYS) before
the deferred replay and check if scene renders every flush — confirms depth-content staleness + is most
of the fix. (Lower prob: the composite samples an intermediate RT that's empty at replay.)

### rstab4depth — depth-test-disabled experiment → DEPTH REFUTED
Ran with `dc3_replay_depth_disable=true` (clears RB_DEPTHCONTROL.z_enable for every replayed draw).
Gameplay window: 5 SCENE / 13 HUD (~28%) — statistically identical to rstab3 (~30%). **Disabling the
depth test did NOT make the scene render every flush.** So geometry is NOT depth-rejected; the draws
produce NO fragments at all ⇒ vertices transformed **off-screen / degenerate** on HUD flushes.

### CONCLUSION (root cause, via elimination)
The ~306 scene geometry draws execute every flush but on HUD flushes are transformed to no coverage.
Since the STATIC venue and the dynamic dancers vanish TOGETHER (shared transform) and depth is not the
gate, the cause is a **shared vertex-transform CONSTANT (view-projection / camera matrix) read from
GUEST MEMORY at replay time that is STALE** — DC3's vertex shaders fetch the matrix from a guest
constant buffer (not the ALU constant registers, which ARE snapshotted), and the game thread runs ahead
and overwrites that buffer (double/triple-buffered) before `FlushDeferredDraws` replays the recorded
draws. Burst = the window where the recorded draws' constant buffer is still valid before overwrite.
This is the fundamental **deferred-capture-reads-mutated-guest-memory** hazard, localized to the
transform constant buffer. (Note: scene-frame count varies run-to-run 2→5→10, so the `dc3_persist_render_state`
"help" may be partly noise; keep it anyway — it correctly stops the EDRAM-zero corrupting persisted resolves.)

### Candidate fixes for the replay-time staleness (next)
1. **Snapshot guest data at RECORD time:** when deferring a draw, also copy the guest vertex + shader-
   constant memory ranges it reads (like the existing resolve-vertex snapshot at IssueDraw) and restore
   them before replay. Most correct, largest change.
2. **Record + replay in the SAME frame:** arm deferral at frame START (not at the prior swap) so the
   capture frame's own draws are recorded and replayed at its swap — guest memory consistent. Requires
   moving the deferral arm out of IssueSwap.
3. **Pragmatic (unblocks telemetry now):** IK/skeleton telemetry is CPU-side every frame regardless of
   render; for visual ground truth, capture at high frequency and KEEP only PNG>250KB frames (the valid
   bursts). Classify by PNG size, not nonzero_pct.

---

## ★ SOLVED — inline rendering (capture the right place). Validated 2026-06-03.
The architecture review (`dc3_render_pipeline_architecture.md`) showed we were capturing at the WRONG
place: the headless deferred-draw-replay re-renders the scene one frame late from a register snapshot
that omits the vertex-transform constant buffer (read live at replay after the game overwrote it). The
canonical per-frame image is the frontbuffer the game itself RESOLVES on its own timeline.

**Fix = `dc3_inline_render` cvar (NEW):** execute draws + resolves INLINE every frame (no defer/replay),
so the capture readback reads the game's own live-resolved frontbuffer. The deferral existed only to
dodge a CP-stall deadlock from COLD VkPipelineCache compiles — but that cache **persists to disk**
(`/tmp/claude/xenia_vulkan_pipeline_cache.bin`, ~4.3 MB, loaded at startup), so inline draws are warm.

**RESULT (inline1 run, warm cache):** NO deadlock (VdSwap → #8100), **15/15 gameplay captures are full
SCENE** (vs deferred's ~30%), at **~1.2 MB each** (vs bursts' 0.4–0.55 MB — much higher fidelity), zero
SIGSEGV. frame_6400 = both dancers + venue + lighting + move-cards, complete and correct. This is the
clean, deterministic per-frame capture the IK/skeleton telemetry needs. **Lever 2 (architecture review)
CONFIRMED.** The deferred-replay path + `dc3_persist_render_state` + all RSTAB/RSTAB2/depth diagnostics
are now superseded for the capture goal (keep as instrumentation; inline is the path).

**Productionization TODO:** (1) make `dc3_inline_render` the default (or auto-enable once cache warm);
(2) COLD-START robustness — a first-ever run (cold cache) may still CP-stall during menu warmup; either
keep deferral during warmup then switch to inline, or pre-warm. (3) Confirm inline holds across a longer
run / different songs. (4) Then capture IK telemetry against these clean frames.

## Run/diagnose commands (serialized — GPU runs are coordinator-supervised)

```bash
# Build
cd /home/free/code/milohax/xenia/build && make xenia-headless 2>&1 | tail -3
# Capture run (foreground + timeout; frames to <label>/frames/)
cd /tmp/xenia-boot-logs && timeout 160 bash run_boot.sh <label> 1 120   # gpu=1, capture interval
magick <label>/frames/frame_NNNN.ppm out.png                            # PPM -> PNG
# State/log probe (no capture)
timeout 130 bash nocap_boot.sh <label> 1 100
grep "gpState\|SIGSEGV\|FlushDeferredDraws\|VdSwap" <label>/run.log <label>/stdout.log
# Cleanup AFTER EVERY RUN (never pkill -f xenia — kills own shell)
pkill -9 -x xenia-headless
find /home/free/code/milohax/xenia /tmp -maxdepth 2 -name '*.core' -size +100M -delete
```

## Hard constraints (carry over)
- GPU access needs `dangerouslyDisableSandbox: true` (Vulkan ICD blocked in sandbox).
- GPU runs **serialized + supervised**; never fan out parallel xenia processes (GPU contention +
  ~6 GB cores + orphan procs). Code analysis fans out freely (read-only, no GPU).
- `pkill -9 -x xenia-headless` only. Run foreground with `timeout`. Delete cores after each run.
- GPU0 often held by sibling `rb3-native`; prefer `--vulkan_device=1` (gpu arg `1`).
