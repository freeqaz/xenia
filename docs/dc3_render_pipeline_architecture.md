# DC3 Render Pipeline Architecture — and Where to Capture a Frame

Audience: an engineer who needs a clean per-frame screenshot of DC3 (`debug.xex`) running
**headless** under Xenia/Vulkan on Linux (branch `headless-vulkan-linux`), to use as visual
ground truth alongside CPU-side IK/skeleton telemetry.

This doc answers one question: **are we capturing at the right place in the pipeline?** Short
answer up front: **no.** The current headless "deferred-draw replay at swap" mechanism re-renders
the scene one frame late against guest memory the game thread has already overwritten. The
already-finished frame is sitting in memory at swap time; we should read *that* instead of
re-rendering. Details and the cheapest path to fix it are below.

All claims are cited to `file:line` in this repo and were read against the actual source.

---

## Render flow walkthrough

How DC3's GPU work becomes a presentable frontbuffer image, step by step.

1. **One ring buffer, one worker thread, strict order.** The GPU command processor (CP) runs a
   single worker thread that drains the PM4 ring buffer in order
   (`command_processor.cc:200-254`, `ExecutePrimaryBuffer`). *Every* GPU action is a PM4 packet
   processed in sequence on this thread: draws, the EDRAM→memory resolve, the sync events the
   title spin-waits on (`PM4_EVENT_WRITE` / `PM4_WAIT_REG_MEM`), and the swap itself
   (`PM4_XE_SWAP`). For a given frame the draws/copies come **first** in the ring; the
   `XE_SWAP` packet is **last**. This ordering is the load-bearing fact for capture timing.

2. **Draws write into EDRAM (host render targets).** The guest binds an EDRAM region as the
   active color RT via `RB_SURFACE_INFO` / `RB_COLOR_INFO` (color base is in *tiles*, not a CPU
   address) and optionally depth via `RB_DEPTH_INFO` (`registers.h:670-897`). `IssueDraw`
   (`vulkan_command_processor.cc:2664`) feeds the render backend, which writes fragments into
   those EDRAM tiles. In Xenia, EDRAM is emulated as host VkImage render targets owned by
   `VulkanRenderTargetCache`. The HUD, the static venue, and the dancers all land in EDRAM
   first — nothing is in a presentable memory image yet.

3. **Resolve copies EDRAM → shared memory (the GPU mirror of guest RAM).** The guest issues a
   resolve (a PM4 copy). `IssueCopy` (`vulkan_command_processor.cc:3328`) calls
   `render_target_cache_->Resolve(...)`. `draw_util.cc:804` `GetResolveInfo` decodes
   `RB_COPY_CONTROL` / `RB_COPY_DEST_BASE` / `RB_COPY_DEST_PITCH/INFO`
   (`registers.h:853-897`) into a source EDRAM tile span and a destination extent in guest
   physical memory. The actual copy is a **compute shader** that reads the EDRAM emulation buffer
   and **writes the detiled pixels into `shared_memory.buffer()`** at offset
   `copy_dest_base` (`vulkan_render_target_cache.cc:1126-1189`), then calls
   `MarkRangeAsResolved` (`:1187`) to flag those pages GPU-written.
   **Critical:** in the Vulkan backend the resolve destination is the GPU-side shared-memory
   buffer (the host mirror of guest physical RAM), **not** guest physical RAM itself. See
   [The role of EDRAM](#the-role-of-edram).

4. **VdSwap names the finished frontbuffer.** The guest presents by calling `VdSwap`
   (`xboxkrnl_video.cc:356`). It receives a `fetch_ptr` = the D3D9 frontbuffer texture-header
   fetch constant, translates its virtual base to a physical address
   (`:396-407`), writes that fetch constant into `SHADER_CONSTANT_FETCH_00`, and emits a
   `PM4_XE_SWAP` packet carrying `frontbuffer_physical_address` + width + height
   (`:429-443`). `VdSwap` asserts the format is `k_8_8_8_8` or
   `k_2_10_10_10_AS_16_16_16_16` and colorspace RGB(0) (`:411-414`). Semantics: *"scan out the
   texture at this physical address, described by fetch constant 0."* There is no separate
   scanout DMA — the frontbuffer **is** the resolved texture, addressed by the fetch constant.

5. **Swap consumes the resolved frontbuffer.** The CP reaches the `XE_SWAP` packet:
   `ExecutePacketType3_XE_SWAP` (`command_processor.cc:931`) reads `frontbuffer_ptr`/width/height
   (`:946-950`) and calls `IssueSwap` (`:953`). In the **normal (presenter) path**,
   `VulkanCommandProcessor::IssueSwap` (`vulkan_command_processor.cc:1347`) takes the
   `presenter != nullptr` branch and calls `texture_cache_->RequestSwapTexture(...)` (`:1781`).
   `RequestSwapTexture` (`vulkan_texture_cache.cc:876`) reads **texture fetch constant 0** (the
   one `VdSwap` just installed, `:880`), builds a `TextureKey`, `FindOrCreateTexture`, and
   `LoadTextureData` (`:898`) — which pulls the bytes from **shared memory** with correct
   untiling/swizzle/format. The presenter then runs a gamma-apply fullscreen pass into its
   guest-output image (`:1787` `RefreshGuestOutput`).

6. **The image leaves the engine.** With a real window, `PaintAndPresent` blits to a swapchain.
   Without one, `Presenter::CaptureGuestOutput(RawImage&)` (`presenter.h:325`; Vulkan impl
   `vulkan_presenter.cc:242`) consumes the latest guest-output mailbox image, does
   `vkCmdCopyImageToBuffer` into a host-visible buffer, and converts to 8bpc pixels — **no
   swapchain, no window required**.

End to end (normal path): guest command stream → draws into EDRAM (host RTs) → resolve compute
shader → shared memory at `copy_dest_base` → `VdSwap` names fetch-0/frontbuffer →
`RequestSwapTexture` detiles it → presenter guest-output image → `CaptureGuestOutput`. **None of
these steps re-reads the scene's vertex-transform constants after the frame was rendered.**

---

## The role of EDRAM

**What it is.** EDRAM is the Xbox 360's on-die 10 MiB tiled framebuffer — *not* main memory. It
is documented as "an opaque block of memory accessible by the RB stage" that performs the
output-merger (color write/blend, depth/stencil) and resolve, "laid out as 2048 tiles on 80x16
32bpp MSAA samples" (`xenos.h:225-249`; `kEdramTileWidthSamples=80`, `kEdramTileHeightSamples=16`,
`kEdramTileCount=2048`, `kEdramSizeBytes` at `xenos.h:414-418`).

**How it's written.** Every draw's fragments land in EDRAM tiles, addressed by
`RB_COLOR_INFO.color_base` / `RB_DEPTH_INFO.depth_base` *in tiles* (`registers.h`), never by a
CPU address. In Xenia this is emulated by `VulkanRenderTargetCache` host VkImage render targets
plus an `edram_buffer()`.

**How it's read.** EDRAM is read only by (a) the RB output-merger during subsequent draws and
(b) the **resolve** operation. There is no way to "read EDRAM as a CPU image" — you must resolve
it out first. In Xenia the resolve compute shader reads the EDRAM emulation and writes into
`shared_memory.buffer()` (`vulkan_render_target_cache.cc:1126-1189`).

**Where the final per-frame image canonically lives.** *Not in EDRAM.* The presentable image is
the **resolve output**: a tiled texture in the GPU shared-memory buffer (the host mirror of guest
physical memory) at `copy_dest_base`, identified each frame by **`VdSwap`'s
`frontbuffer_physical_address` / texture fetch constant 0**. EDRAM tiles are upstream, transient
scratch that gets overwritten by the next frame's draws. The durable, addressable per-frame image
is the resolved frontbuffer in shared memory.

**Backend nuance that breaks the naive capture.** Because the resolve writes to
`shared_memory.buffer()` (a GPU buffer) and **not** to guest physical RAM, reading guest RAM at
`frontbuffer_ptr` returns **zeros** in this backend. The IssueSwap code says exactly this:
invalidating the swap texture would force a re-upload "from guest physical memory ... which is
zeros, destroying the resolve data." So any capture that `TranslatePhysical(frontbuffer_ptr)`s and
reads bytes (the base-class raw dump, below) reads zeros here. The *correct* read of the
frontbuffer goes through the **texture cache** (`RequestSwapTexture`), which loads from shared
memory and detiles via the fetch-0 layout.

---

## Normal present vs our headless hack

### Normal (presenter) path — inline, zero staleness

When a presenter exists, `deferred_draws_enabled_` is **false** on this path, so draws and copies
execute **inline** the moment the CP reaches them (`vulkan_command_processor.cc:2711`/`2761`
guard the deferral; without the flag, draws run immediately). The vertex shaders fetch the
view-projection / skinning matrices from a **guest constant buffer in shared memory** while those
constants are **still the ones the title wrote for this frame**. Then the resolve runs inline,
writing the finished image to shared memory. At `XE_SWAP`, `RequestSwapTexture` loads that just-
resolved image and the presenter renders it. **No replay, no second read of guest memory, no
cross-frame gap.** The frame the presenter shows is exactly the one the game produced.

### Our headless hack — deferred record/replay across a frame boundary

Headless has **no presenter**: the offscreen-presenter creation in `GraphicsSystem::Setup` is
entirely inside `#ifndef XE_HEADLESS_BUILD` (`graphics_system.cc:66-87`), so
`graphics_system_->presenter()` is `nullptr` and `IssueSwap` takes the headless branch
(`vulkan_command_processor.cc:1352-1370`). To synthesize a screenshot, the hack:

1. **Arms deferral** at the swap *before* the capture frame: sets
   `deferred_draws_enabled_ = true` / `headless_render_frame_ = true`
   (`vulkan_command_processor.cc:1431-1432`, re-armed `:1762-1763`).
2. **Records** the next frame's draws instead of executing them: each `IssueDraw` /
   `IssueCopy` pushes a `DeferredDrawState` snapshotting the **register file**, shader pointers,
   prim type, and index info (`:2761-2785` for draws; `:2711-2743` for copies).
3. **Replays** them at the capture swap via `FlushDeferredDraws` (`:5094`): it memcpy-restores
   each draw's register snapshot and re-issues the draw, then reads back the swap texture.

**Why this is the wrong place — the staleness hazard.** The snapshot for a regular DRAW captures
only the register file (ALU/state regs). It does **not** capture the guest constant buffer that
the vertex shader fetches the **view-projection / skinning matrices** from — that buffer is read
from shared memory **at replay time**, one frame late. The game thread runs a frame ahead and has
already overwritten that (double/triple-buffered) constant buffer, so on most replays the geometry
transforms to no coverage. This is exactly the observed symptom: the static venue vanishes
**together** with the dancers, and the scene only "bursts" through in ~30% of captures (the frames
where the constant buffer happened not to have been overwritten yet).

**The codebase already knows this hazard exists** — and only half-fixed it. The *copy/resolve*
record path explicitly snapshots the resolve rectangle's vertex data out of guest memory because
"it may be overwritten by subsequent frames before we replay"
(`vulkan_command_processor.cc:2726-2740`, `resolve_vertex_data`). The *draw* record path
(`:2761-2785`) has **no equivalent snapshot** of the scene's transform constants. That asymmetry
is the bug.

**Why the deferral exists at all (so we don't reintroduce a different bug).** It is **not** an
architectural requirement — it is a **pipeline-compile-latency workaround**. On a cold
`VkPipelineCache`, `vkCreateGraphicsPipelines` takes 10–100 ms per new pipeline; if that runs
inline on the CP thread, the CP can't advance to the `EVENT_WRITE_SHD` / `WAIT_REG_MEM` packets
the game thread spin-waits on, the title's watchdog fires, and `VdSwap` stops being called
(`vulkan_pipeline_cache.cc:380-388`, `:496-509`). The same file documents that with a **warm**
`VkPipelineCache`, compiles are <1 ms and "the synchronous path is fast enough" (`:386-388`), and
`warmup_wait_` deliberately falls through to the synchronous inline path. So the deferral batches
draws to the swap packet to dodge the *first-compile* stall — it has nothing to do with where the
frame image lives.

**Verdict: we are capturing at the wrong place.** We re-render the scene at swap time from a
register snapshot, then read live guest constants that have since changed. The finished frame the
game already resolved is available; we are ignoring it and reconstructing a stale one.

---

## Capture levers

De-duplicated and ranked. "Sidesteps staleness?" means: does it avoid the
deferred-replay-reads-mutated-guest-memory hazard?

| # | Lever | Where it taps in | Sidesteps staleness? | Feasibility | Key cons |
|---|-------|------------------|----------------------|-------------|----------|
| **1** | **Offscreen (windowless) presenter + `CaptureGuestOutput`** — run the *normal* present path | Enable presenter in headless build: `graphics_system.cc:66-87` (offscreen else-branch already written, just `#ifndef XE_HEADLESS_BUILD`-gated); normal path `vulkan_command_processor.cc:1772-2097`; capture `vulkan_presenter.cc:242` | **YES** — draws+resolve run inline; swap reads the already-resolved frontbuffer; no replay, no live-constant read | **Medium** (code+build). Offscreen branch + `CaptureGuestOutput` already exist and are used by Xenia's own capture tooling; presenter is surface/swapchain-free (`vulkan_presenter.cc:526-553`, `vulkan_provider.cc:106-110`) | Must compile presenter into the `XE_HEADLESS_BUILD` target and verify no other UI/window dependency is pulled in; inherits the cold-pipeline-stall caveat (draws run inline) — needs a warm `VkPipelineCache`; output is gamma-applied 8bpc |
| **2** | **No deferral — render inline every frame, read swap texture at `VdSwap`** (`RequestSwapTexture`, no replay) | Stop arming deferral (`vulkan_command_processor.cc:1431-1432`/`1762-1763`); let `IssueDraw`/`IssueCopy` run inline; read swap image via the existing headless readback block (`:1456-1531`) | **YES** — geometry's constant buffer is read in the *same* frame the title wrote it; no record/replay split | **High** *iff* pipelines are warm | On a **cold** `VkPipelineCache`, inline first-compile stalls the CP thread → `VdSwap` stops (the original deadlock). Must pre-warm / persist the pipeline cache (`vulkan_pipeline_cache.cc:386-388`). `force_all_draws` is **not** this — it still defers+replays at swap |
| **3** | **Read the already-resolved frontbuffer via texture cache + GPU readback** (no presenter) | `RequestSwapTexture` (`vulkan_texture_cache.cc:876`) → `vkCmdCopyImageToBuffer` → PPM. Headless readback plumbing already present at `vulkan_command_processor.cc:1456-1531` | **YES** *only if* draws run inline (i.e. combine with lever 2). The swap-texture read itself is stale-free; the staleness comes from deferral | **Medium** | Essentially lever 2's readback half; with deferral kept it inherits the same hazard. Reading raw `shared_memory.buffer()` instead of the swap image would require manual detile/swizzle — the texture-cache path already does that correctly, so go through `RequestSwapTexture` |
| **4** | **Base-class raw guest-RAM PPM dump** (`dump_frames_path` cvar) | `command_processor.cc:955-995`; gated off by `!HandlesFrameDump()`, and the Vulkan backend returns `HandlesFrameDump()==true` (`vulkan_command_processor.h:267`,`741`) | **In principle yes** (reads the resolved frontbuffer, not constants) — **but broken on this backend** | **Low (as-is)** | Resolve writes to `shared_memory.buffer()`, **not** guest RAM, so `TranslatePhysical(frontbuffer_ptr)` reads **zeros** (`vulkan_command_processor.cc:1520-1525`). Also assumes linear big-endian ARGB8; real frontbuffer is tiled. Subsumed by lever 3 |
| **5** | **Resolve-time tap** — snapshot bytes when the game resolves the frontbuffer | `vulkan_render_target_cache.cc:1126-1189` (`copy_dest_base`); correlate with `VdSwap` `frontbuffer_physical_address` | **YES, strongest** — captures at the exact instant the game produced the image, on its own timeline | **Low-to-medium** | Must match resolve dest to the *next* `VdSwap` frontbuffer (double-buffering, `command_processor.h:41-55`); needs detile; frontbuffer is occasionally a CPU-written/file-loaded surface, not a resolve dest — would miss those frames. Little benefit over 1/2/3 since the bytes persist in shared memory until the next frame anyway |
| **6** | **Snapshot the scene's transform constant buffer at RECORD time** (keep deferral, make it correct) | Mirror the resolve-vertex snapshot (`vulkan_command_processor.cc:2726-2740`) for regular draws (`:2761-2785`) | **YES** if the right guest ranges are copied | **Medium-to-low** | Requires identifying *which* guest constant-fetch addresses the scene VS uses, per draw, and copying them at record time. Strictly more work than just removing deferral (levers 1/2) once pipelines are warm |
| **7** | **Offline trace replay** (`trace_gpu_stream` → `trace_dump`) | `command_processor.cc:103-134`,`871-888`; `trace_dump.cc:43-123` | **YES** for offline replay (memory frozen in the trace) | **Medium offline / low for live** | Two-pass; **not** synchronized with live CPU-side IK telemetry; trace EDRAM dump is a TODO (`vulkan_command_processor.cc` `InitializeTrace`). Good as an offline validation oracle, weak as the live feed |
| **8** | **Cadence-only tweaks** (`headless_capture_interval`, vsync) | `gpu_flags.cc:54`; `graphics_system.cc` vsync timer | **NO** | High | Captures the *same* stale buffer more often. Does not address staleness at all |

---

## Recommendation

**We are capturing at the wrong place.** Stop replaying recorded draws at swap and re-reading
mutated guest constants. Instead, read the frontbuffer the game **already resolved** during its
normal rendering — the canonical per-frame image — at `VdSwap` time.

**Pursue Lever 1 (offscreen/windowless presenter + `CaptureGuestOutput`) as the primary fix.**
Why it is the cleanest:

- It runs the **production present path** (`vulkan_command_processor.cc:1772-2097`), which renders
  draws inline and reads the just-resolved frontbuffer via `RequestSwapTexture` — battle-tested,
  format-correct untiling, gamma ramp included. Zero new image-decode code.
- The capture mechanism (`Presenter::CaptureGuestOutput`, `vulkan_presenter.cc:242`) needs **no
  swapchain and no window** — it copies an internal VkImage to a host buffer. The presenter
  constructor itself does not require a surface (`vulkan_provider.cc:106-110`,
  `vulkan_presenter.cc:526-553`).
- The offscreen path **already exists** in the tree, with the literal comment "May be needed for
  offscreen use, such as capturing the guest output image" (`graphics_system.cc:78-79`). It is
  dormant only because the whole block is `#ifndef XE_HEADLESS_BUILD`.

If a full presenter wire-up is too heavy for the immediate need, **Lever 2/3 (remove deferral,
read the swap texture inline)** is a smaller change that reuses the existing headless readback
buffer — but it requires a **warm `VkPipelineCache`** to avoid the original CP-stall deadlock, and
it loses the presenter's gamma/scale handling.

Both 1 and 2/3 share one prerequisite worth de-risking first: **inline drawing needs warm
pipelines.** That is the single gating risk; everything else is plumbing.

### Concrete first steps

1. **Confirm the gate is the only blocker.** Verify `vulkan_command_processor.h:267`
   `HandlesFrameDump()` returns `true` headless (it does — `headless_frame_dump_`), and that the
   presenter `nullptr` is solely due to the `#ifndef XE_HEADLESS_BUILD` at
   `graphics_system.cc:66`. (Both confirmed by reading; this is a sanity check before editing.)
2. **Prototype Lever 1.** Allow presenter creation in the headless build (loosen the
   `XE_HEADLESS_BUILD` guard around `graphics_system.cc:66-87`, or take the `app_context_==nullptr`
   offscreen branch) and confirm `VulkanPresenter::Create` succeeds when the instance/device were
   created without surface/swapchain extensions. With a presenter present, `IssueSwap`
   automatically takes the normal branch (`vulkan_command_processor.cc:1352`), and inline drawing
   re-enables because the headless skip is gated on `!graphics_system_->presenter()`
   (`vulkan_command_processor.cc:2693`/`2704`/`2753`).
3. **Drive capture.** Call `presenter->CaptureGuestOutput(RawImage&)` on a timer/capture thread at
   the desired cadence (`headless_capture_interval`, `gpu_flags.cc:54`) and write PPM/PNG. The
   mailbox consume path is lock-protected for cross-thread use (`vulkan_presenter.cc:247`).
4. **De-risk the stall.** Before relying on inline draws, **pre-warm the pipeline cache** (run a
   few discard frames or load a persisted `VkPipelineCache`) so first-compile is <1 ms
   (`vulkan_pipeline_cache.cc:386-388`). If a windowed run already renders the gameplay scene
   without deadlock, the offscreen presenter will too.

If Lever 1 hits a hard `XE_HEADLESS_BUILD` linkage wall, **fall back to Lever 6** (snapshot the
scene's transform constant buffer at record time, mirroring the existing
`resolve_vertex_data` snapshot at `vulkan_command_processor.cc:2726-2740`) as a surgical fix that
keeps the current deferral architecture but closes the staleness hole.

---

## Open questions / unknowns to verify before committing

1. **Does instantiating `VulkanPresenter` in the `XE_HEADLESS_BUILD` config pull in other
   surface/UI dependencies that won't link or init without a window/app_context?** The mailbox +
   `CaptureGuestOutput` path is surface-independent (`vulkan_presenter.cc:242-434`,`526-553`), but
   `VulkanPresenter::Create`'s full member init should be read to confirm nothing touches
   surface-only state. (Gating risk for Lever 1.)
2. **Can DC3's pipeline set be fully pre-warmed** (persisted `VkPipelineCache` or N discard
   frames) so inline draws never stall the CP thread on the gameplay scene? The cache claims
   <1 ms warm (`vulkan_pipeline_cache.cc:386-387`), but this needs an empirical warm-cache inline
   run. (Single gating risk shared by Levers 1 and 2/3.)
3. **In headless mode today, does the game's resolve to the frontbuffer actually run during the
   normal frame, or does the deferral suppress/redirect it** so shared memory at `frontbuffer_ptr`
   is empty? The headless copy path skips copies when `!headless_render_frame_`
   (`vulkan_command_processor.cc:2704`). Confirm that once draws run inline (presenter present),
   the resolve to `copy_dest_base == frontbuffer` populates the swap texture. (Crux of whether
   Levers 1/3 "just work.")
4. **Is the DC3 frontbuffer `k_8_8_8_8` or `k_2_10_10_10_AS_16_16_16_16`, and is it tiled?**
   `VdSwap` asserts one of those two (`xboxkrnl_video.cc:411-414`). `RequestSwapTexture` handles
   both via the texture cache; the base-class raw dump does not. Confirm which DC3 uses so the PPM
   isn't scrambled (only matters if you bypass the texture cache).
5. **Does DC3 double-buffer the frontbuffer** (two alternating physical addresses per `VdSwap`)?
   `command_processor.h:41-55` `SwapState` implies front/back. Capturing at swap is correct either
   way (each `VdSwap` names the finished buffer), but a resolve-time tap (Lever 5) must identify
   *which* resolve feeds the next swap.
6. **For Lever 6 specifically: which guest constant-fetch range holds the scene VS's
   view-projection / skinning matrices?** Needed to snapshot it at record time. Identify the
   constant fetch addresses the gameplay vertex shaders use (mirror the resolve-vertex logic at
   `vulkan_command_processor.cc:2726-2740`).
7. **Fidelity:** `CaptureGuestOutput` applies the gamma ramp and 10bpc→8bpc conversion
   (`vulkan_presenter.cc:419-428`). Confirm gamma-applied 8bpc is acceptable for IK-telemetry
   overlay, or whether a pre-gamma raw swap-texture readback (Lever 3) is preferred.
