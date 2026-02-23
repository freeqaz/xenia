# DC3 Headless Change Audit (2026-02-20)

## Scope

This document is a research snapshot of the current Xenia headless work for DC3.

- Xenia repo: `/home/free/code/milohax/xenia`
- Target binary under test: `/home/free/code/milohax/dc3-decomp/orig/373307D9/default.xex`
- Mainline baseline for this repo: `upstream/master` (local branch name is `master`, not `main`)
- Working baseline for in-progress work: current uncommitted working tree on `headless-vulkan-linux`

No code changes are proposed in this doc; this is architecture + change inventory + roadmap.

## Reference Map (Agent Quick Links)

Use this section as the primary cross-doc index for investigation sessions.

### DC3-Decomp Runtime and Session Docs

| Doc | Summary | Use When |
|---|---|---|
| [`../../dc3-decomp/docs/INDEX.md`](../../dc3-decomp/docs/INDEX.md) | Master sitemap for all decomp/runtime docs. | Starting a session and finding canonical docs quickly. |
| [`../../dc3-decomp/docs/runtime/XENIA_HEADLESS_STATUS.md`](../../dc3-decomp/docs/runtime/XENIA_HEADLESS_STATUS.md) | Main rolling status: rendering investigation, root-cause notes, debug flags, roadmap. | Need current ground truth for headless/DC3 runtime behavior. |
| [`../../dc3-decomp/docs/runtime/BOOT_ANALYSIS.md`](../../dc3-decomp/docs/runtime/BOOT_ANALYSIS.md) | Boot progression timeline, thread architecture, known run commands. | Need boot-stage evidence and expected timeline landmarks. |
| [`../../dc3-decomp/docs/runtime/SCRIPTED_INPUT_TESTING.md`](../../dc3-decomp/docs/runtime/SCRIPTED_INPUT_TESTING.md) | Actual scripted-input implementation and DC3 navigation strategy. | Validating input scripts, key mappings, and timing assumptions. |
| [`../../dc3-decomp/docs/sessions/2026-02-18-xenia-screenshot-breakthrough.md`](../../dc3-decomp/docs/sessions/2026-02-18-xenia-screenshot-breakthrough.md) | Milestone narrative: from black frames to first correct boot animation captures. | Need historical context for what finally worked. |
| [`../../dc3-decomp/docs/sessions/2026-02-18-xenia-frame-capture-attempts.md`](../../dc3-decomp/docs/sessions/2026-02-18-xenia-frame-capture-attempts.md) | Capture approaches tried and their failure modes. | Avoiding previously failed capture strategies. |
| [`../../dc3-decomp/docs/sessions/2026-02-18-vulkan-headless-rendering.md`](../../dc3-decomp/docs/sessions/2026-02-18-vulkan-headless-rendering.md) | Vulkan headless rendering architecture and capture path decisions. | Re-checking design rationale for current Vulkan path. |
| [`../../dc3-decomp/docs/sessions/2026-02-18-vulkan-performance-investigation.md`](../../dc3-decomp/docs/sessions/2026-02-18-vulkan-performance-investigation.md) | Performance findings and bottleneck analysis under headless Vulkan. | Diagnosing regressions in throughput/latency. |

### Local Xenia Docs

| Doc | Summary | Use When |
|---|---|---|
| [`DC3_NUI_ROADMAP.md`](DC3_NUI_ROADMAP.md) | Historical DC3 Kinect/NUI shim backlog and early blocker analysis. | Auditing NUI/XAM/Xboxkrnl stub intent and historical assumptions. |
| [`gpu.md`](gpu.md) | High-level Xenia GPU subsystem architecture and concepts. | Need subsystem context before editing Vulkan command paths. |
| [`kernel.md`](kernel.md) | Kernel object/system model overview. | Need quick kernel semantics while reviewing shim behavior. |
| [`cpu.md`](cpu.md) | CPU/JIT and module-loading overview. | Need context for thunk/ABI/PE override implications. |
| [`instruction_tracing.md`](instruction_tracing.md) | Instruction tracing notes and usage guidance. | Planning low-level CPU-side debugging sessions. |

### Cross-Repo XEX / Linking Reference Pack

Use this when the question is "what does XEX/Xenia actually do?" versus "what did we think it does?"

#### Canonical Runtime Behavior (highest trust)

| Source | Summary | Use When |
|---|---|---|
| [`../src/xenia/kernel/util/xex2_info.h`](../src/xenia/kernel/util/xex2_info.h) | Canonical Xenia XEX2 structs/enums: header keys, import library layout, security info, compression types. | Verifying exact field definitions and offsets. |
| [`../src/xenia/cpu/xex_module.cc`](../src/xenia/cpu/xex_module.cc) | Actual import resolution behavior (`record_type` variable/thunk), PE override flow, exception directory notes. | Explaining why a given XEX/import pattern passes or fails at runtime. |
| [`kernel.md`](kernel.md) | Loader/shim model in plain language (imports become syscalls, then export linkage). | Connecting low-level import parsing to high-level kernel behavior. |
| [`cpu.md`](cpu.md) | Guest memory map including XEX regions (`0x80000000+`) and ABI/JIT transitions. | Debugging address assumptions, thunk addresses, or VA mapping issues. |

#### DC3 Build + Packaging Pipeline (project truth)

| Source | Summary | Use When |
|---|---|---|
| [`../../dc3-decomp/scripts/build/build_xex.py`](../../dc3-decomp/scripts/build/build_xex.py) | Current XEX2 packer: parses original optional headers, copies import data, generates thunk section, patches import library VAs. | Auditing what our produced `default.xex` really contains. |
| [`../../dc3-decomp/scripts/build/decompress_xex.py`](../../dc3-decomp/scripts/build/decompress_xex.py) | Decompress/extract helper used to inspect original PE/import records (`0x600+` region). | Re-validating import marker data and compression assumptions. |
| [`../../dc3-decomp/scripts/build/fix_pdata.py`](../../dc3-decomp/scripts/build/fix_pdata.py) | `.pdata -> .pdatN` workaround script for LNK1223. | Understanding current exception-handling correctness tradeoff. |
| [`../../dc3-decomp/scripts/build/compare_pe.py`](../../dc3-decomp/scripts/build/compare_pe.py) | Anchor-based PE comparison and drift tracking against original map/function layout. | Quantifying layout drift and PE override risk. |
| [`../../dc3-decomp/tools/ghidra/import-xex.sh`](../../dc3-decomp/tools/ghidra/import-xex.sh) | Repeatable headless Ghidra import workflow with MAP symbol injection. | Rebuilding analysis projects from scratch with consistent symbols. |
| [`../../dc3-decomp/docs/plans/THUNK_SECTION_IMPLEMENTATION.md`](../../dc3-decomp/docs/plans/THUNK_SECTION_IMPLEMENTATION.md) | Implemented import/thunk strategy (347 thunks, 707 imports resolved). | Import resolution regressions or design recall. |
| [`../../dc3-decomp/docs/sessions/2026-02-12-pdata-role-in-x360-linking.md`](../../dc3-decomp/docs/sessions/2026-02-12-pdata-role-in-x360-linking.md) | Deep `.pdata` + `IMAGE_CE_RUNTIME_FUNCTION_ENTRY` analysis and runtime implications. | Any EH/unwind/linker `.pdata` discussion. |
| [`../../dc3-decomp/docs/sessions/2026-02-20-clean-link-plan.md`](../../dc3-decomp/docs/sessions/2026-02-20-clean-link-plan.md) | Actionable plan to remove `.pdat0` workaround and fix jeff `.pdata` emission. | Planning clean-link / runtime-correct exception metadata work. |
| [`../../dc3-decomp/docs/sessions/2026-02-19-xex-workstreams.md`](../../dc3-decomp/docs/sessions/2026-02-19-xex-workstreams.md) | Workstream view for COMDAT, `.pdata`, and section-layout quality. | Prioritizing XEX quality work beyond immediate boot tests. |
| [`../../dc3-decomp/docs/sessions/2026-02-11-xexp-patch-generation-investigation.md`](../../dc3-decomp/docs/sessions/2026-02-11-xexp-patch-generation-investigation.md) | XEXP delta patch format research and tooling gaps. | Evaluating distribution strategies (`.xexp` vs full XEX vs dev-only patching). |
| [`../../dc3-decomp/docs/tools/XEXLOADERWV.md`](../../dc3-decomp/docs/tools/XEXLOADERWV.md) | Practical XEXLoaderWV usage guide for Ghidra, including `.pdata` and import symbol behavior. | Onboarding RE/debug sessions against original XEX. |
| [`../../dc3-decomp/docs/plans/PYGHIDRA_MCP_XEX_SUPPORT.md`](../../dc3-decomp/docs/plans/PYGHIDRA_MCP_XEX_SUPPORT.md) | pyghidra-mcp integration notes for XEX language detection and import flow. | MCP/Ghidra automation setup and troubleshooting. |
| [`../../dc3-decomp/docs/reference/FREE60_XEX_FORMAT.md`](../../dc3-decomp/docs/reference/FREE60_XEX_FORMAT.md) | Quick Free60-derived XEX format cheat sheet. | Fast orientation before deeper code-level validation. |

#### Cross-Implementation Parsers (excellent for triangulation)

| Source | Summary | Use When |
|---|---|---|
| [`../../XenonRecomp/XenonUtils/xex.h`](../../XenonRecomp/XenonUtils/xex.h) | Clean XEX enums/structs including import/thunk types and optional header IDs. | Cross-checking struct intent against Xenia. |
| [`../../XenonRecomp/XenonUtils/xex.cpp`](../../XenonRecomp/XenonUtils/xex.cpp) | Practical decrypt/decompress/import handling flow in another codebase. | Validating parser assumptions with independent implementation behavior. |
| [`../../XEXLoaderWV/XEXLoaderWV/src/main/java/xexloaderwv/XEXHeader.java`](../../XEXLoaderWV/XEXLoaderWV/src/main/java/xexloaderwv/XEXHeader.java) | Ghidra loader parser for optional headers, imports, compression/decryption paths. | RE workflow parity checks and import parsing sanity checks. |
| [`../../XEXLoaderWV/XEXLoaderWV/src/main/java/xexloaderwv/XEXLoaderWVLoader.java`](../../XEXLoaderWV/XEXLoaderWV/src/main/java/xexloaderwv/XEXLoaderWVLoader.java) | Load-spec detection (`XEX2`), `.pdata` option, `.xexp` patch integration behavior. | Understanding how analysis tooling interprets XEX variants. |
| [`../../jeff/src/util/xex.rs`](../../jeff/src/util/xex.rs) | dtk/jeff XEX parsing + import record processing + `.pdata`/EH symbol extraction. | Diagnosing split/link pipeline correctness problems. |
| [`../../jeff/src/util/xex_imports.rs`](../../jeff/src/util/xex_imports.rs) | Ordinal-to-name lookup tables used during import symbol reconstruction. | Resolving unknown ordinals or import naming mismatch. |
| [`../../recompiler/dev/src/xenon_decompiler/xexformat.h`](../../recompiler/dev/src/xenon_decompiler/xexformat.h) | Legacy but useful alternative header/enum definitions for XEX parsing. | Third-opinion checks when definitions appear inconsistent. |
| [`../../xbox-reversing/templates/xbox-360/XEX2.bt`](../../xbox-reversing/templates/xbox-360/XEX2.bt) | 010 template covering XEX variants, security info, directory parsing. | Binary-level inspection of raw XEX headers and optional blocks. |
| [`../../xbox-reversing/templates/xbox-360/XEX2OptionalHeaders.bt`](../../xbox-reversing/templates/xbox-360/XEX2OptionalHeaders.bt) | Detailed optional header and import descriptor template definitions. | Confirming import table/header blob shapes in hex-level analysis. |

#### Binary Corpus / Ground-Truth Artifacts

| Source | Summary | Use When |
|---|---|---|
| [`../../dc3-decomp/orig/373307D9/default.xex`](../../dc3-decomp/orig/373307D9/default.xex) | Primary runtime target under test in this audit. | Repro runs and runtime behavioral comparisons. |
| `../../milo-executable-library/dc3/9.16.12 (Final Debug)/ham_xbox_r.map` | Ground-truth map for section layout/function ordering (`.text`, `.pdata`, `.xidata`). | Validating section sizes, drift, and link/layout hypotheses. |
| [`../../milo-executable-library/dc3/README.md`](../../milo-executable-library/dc3/README.md) | Quick metadata on available DC3 debug/no-checksum variants. | Choosing cross-build references for sanity checks. |

#### Reference Precedence (when sources disagree)

1. Xenia runtime code (`xex2_info.h`, `xex_module.cc`)
2. Current DC3 build scripts/docs (`build_xex.py`, session docs)
3. Independent parser implementations (XenonRecomp, XEXLoaderWV, jeff)
4. Free60/wiki-style references

## Executive Summary

- Boot/runtime progress is real: game runs for long durations, 16+ threads, ~33 FPS.
- **Deferred draw rendering partially fixed** (2026-02-20 session): cache invalidation fix produces 151K bright pixels with real rendered content (R=255 warm tones), up from flat B=0x3F.
- **Decomp XEX boot progression** (2026-02-21 sessions): 4 CRT root causes fixed, decomp-specific NUI patch table created, JIT IP sampler built.
- **BSS section size fix** (2026-02-21): Root cause of FixedSizeAlloc crash identified in jeff's COFF emitter. Fix: `append_section_bss()` call added.
- **COMDAT data section fix** (2026-02-22 Session 8): Vtable relocations were silently dropped for .rdata/.data COMDAT regions. Fix: restrict COMDAT to code sections only. FixedSizeAlloc::RawAlloc crash resolved.
- **Clean rebuild + address regen** (2026-02-22 Session 9): Full rebuild with fixed jeff. CRT sanitizer now only nullifies 26/390 entries (down from 320). 364 CRT constructors execute. 62 NUI patches applied. Zero page removed. Heap corruption crash in `RtlpCoalesceFreeBlocks` during CRT init is current blocker.
- All diagnostic logging gated behind cvars (`headless_verbose_diagnostics`, `headless_thread_diagnostics`), defaulting to off.
- Dead code and signal safety issues cleaned up.
- Branch state: all work committed as `a224a6846` on `headless-vulkan-linux` (3 commits ahead of upstream), plus uncommitted decomp XEX support changes.

## Runtime Snapshot (2026-02-20 Session)

### Experiment Results

| ID | Mode | Duration | Threads | Captures | Pixel Quality |
|---|---|---|---|---|---|
| E00 | Null GPU | 30s | 4 (system) | N/A | N/A — baseline stability confirmed |
| E10 | Vulkan force_all_draws | 60s | 16+ | 3 (frames 100/200/300) | Flat R=2,G=1,B=2 — nearly black |
| E20 | Vulkan deferred draws | 60s | 16+ | 3 (frames 100/200/300) | **151K bright pixels, 525 unique RGB, max R=255** |

### E20 Frame Analysis (frame_0100.ppm)

- 283K non-background pixels across full 1280x720 frame
- Brightest pixels: R=255 G=232 B=127 (warm gold/yellow tones)
- Content concentrated at x=[500-720], y=[260-490] — likely boot animation character/logo
- Row variation peaks at rows 240-480 with 500+ unique colors per row
- Overall frame still dark — possible gamma/format issue in readback path

### Key Fix: Deferred Draw Cache Invalidation

Root cause of B=0x3F: `FlushDeferredDraws()` used raw `memcpy` to restore register state, bypassing `VulkanCommandProcessor::WriteRegister()`. This left `current_constant_buffers_up_to_date_` and `texture_bindings_in_sync_` stale, causing draws to render with wrong constant data and textures.

Fix: After memcpy, explicitly invalidate both caches:
- `current_constant_buffers_up_to_date_ = 0`
- `texture_cache_->ResetTextureBindingsInSync()`

### Remaining Issues

1. **E10 flat color**: force_all_draws + deferred mode produces flat R=2,G=1,B=2. Different failure mode from E20 — possibly all draws are identically wrong, or capture timing misses the rendered frame.
2. **E20 dark overall**: While content is visible with 151K bright pixels, most of the frame is very dark. Could be gamma ramp, format conversion, or readback endianness issue.
3. **Inline draw deadlock**: Still present at frame 12 (not tested this session).

## Architecture Notes (Current Code)

### 1. Headless bootstrap and process model

- Entry point and CLI flags are in `src/xenia/app/xenia_headless_main.cc:66`.
- Headless wrapper lifecycle is in `src/xenia/app/emulator_headless.cc:43`.
- Launch flow:
  - Parse flags
  - Build emulator with nop audio + selected GPU + nop input
  - Start emulator thread
  - Run until timeout or exit
- Timeout path currently uses `_Exit(0)` in `src/xenia/app/emulator_headless.cc:123` to bypass shutdown assertions.

### 2. GPU render/capture control loop

Core flow is in `src/xenia/gpu/vulkan/vulkan_command_processor.cc:1327`.

Related external context:

- [`../../dc3-decomp/docs/runtime/XENIA_HEADLESS_STATUS.md`](../../dc3-decomp/docs/runtime/XENIA_HEADLESS_STATUS.md) (Rendering Investigation section)
- [`../../dc3-decomp/docs/sessions/2026-02-18-vulkan-headless-rendering.md`](../../dc3-decomp/docs/sessions/2026-02-18-vulkan-headless-rendering.md)

- `IssueSwap` drives:
  - deferred draw flush
  - submission finalization
  - capture scheduling
  - readback + PPM write
- Per-frame capture gating uses:
  - `headless_capture_interval`
  - `headless_render_frame_`
  - `deferred_draws_enabled_`
- Draw path is in `src/xenia/gpu/vulkan/vulkan_command_processor.cc:2553`.
  - non-render frames can skip draws/copies
  - deferred state snapshots draw/copy register state for replay
- Deferred replay is in `src/xenia/gpu/vulkan/vulkan_command_processor.cc:4976`.
  - restores register state
  - replays draws and resolves
  - restores original register/shader state

### 3. Shared memory watch behavior (critical to deadlock/stability)

- Page-watch suppression flag added to shared memory:
  - `src/xenia/gpu/shared_memory.h:88`
  - `src/xenia/gpu/shared_memory.cc:308`
- Deferred flush toggles this suppression:
  - set true before replay: `src/xenia/gpu/vulkan/vulkan_command_processor.cc:4999`
  - set false after replay: `src/xenia/gpu/vulkan/vulkan_command_processor.cc:5099`

Intent:

- avoid mprotect watch churn and SIGSEGV handler lock contention during one-shot deferred rendering.

### 4. Scripted input path

- Flag wiring: `src/xenia/app/xenia_headless_main.cc:77`
- Parser + timed state engine: `src/xenia/hid/nop/nop_input_driver.cc:51`
- XInput keystroke edge emission: `src/xenia/hid/nop/nop_input_driver.cc:209` and `src/xenia/hid/nop/nop_input_driver.cc:259`

Related external context:

- [`../../dc3-decomp/docs/runtime/SCRIPTED_INPUT_TESTING.md`](../../dc3-decomp/docs/runtime/SCRIPTED_INPUT_TESTING.md)

### 5. DC3-relevant kernel/XAM behavior

- NUI device status is forced connected in `src/xenia/kernel/xam/xam_nui.cc:41`.
- NUI signin broadcasts notifications in `src/xenia/kernel/xam/xam_ui.cc:565`.
- NUI device selector returns a dummy device through overlapped completion in `src/xenia/kernel/xam/xam_ui.cc:603`.
- Ob type checking now compares against export variable addresses in `src/xenia/kernel/xboxkrnl/xboxkrnl_ob.cc:85`.
- XAudio dummy driver/tic behavior is in `src/xenia/kernel/xboxkrnl/xboxkrnl_audio.cc:59`.
- Synchronous read completion behavior is in `src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:245`.

Related external context:

- [`DC3_NUI_ROADMAP.md`](DC3_NUI_ROADMAP.md) (historical implementation list; partially outdated but useful as a checklist)
- [`../../dc3-decomp/docs/runtime/BOOT_ANALYSIS.md`](../../dc3-decomp/docs/runtime/BOOT_ANALYSIS.md)

### 6. PE override mechanism

- Full PE section override + import re-patch path is in `src/xenia/cpu/xex_module.cc:1054`.
- This is still address-layout sensitive and currently blocked by linker/layout mismatch for decomp images.

Related external context:

- [`../../dc3-decomp/docs/runtime/XENIA_HEADLESS_STATUS.md`](../../dc3-decomp/docs/runtime/XENIA_HEADLESS_STATUS.md) (PE override blocker notes)

## Change Inventory

### A. Committed branch delta vs upstream/master

Branch state:

- `HEAD`: `e024ff31a` (`headless-vulkan-linux`)
- `upstream/master...HEAD`: `0 behind / 2 ahead`

Commit history:

1. `6394d2a7f` — Initial headless emulator mode with Vulkan support for Linux
2. `e024ff31a` — Stabilization: gate diagnostics, fix deferred draw rendering, cleanup

Net size (total branch):

- `89 files changed, ~4900 insertions, ~570 deletions` (approx)

High-impact committed areas:

- New headless app binary and main loop
- Vulkan headless capture and deferred replay scaffolding
- **Deferred draw cache invalidation fix** (constant buffer + texture binding)
- Async pipeline cache behavior for headless
- Scripted nop input
- NUI/XAM/Xboxkrnl shims for DC3 boot
- Linux/JIT thunk ABI fixes
- Filesystem and memory mapping platform fixes
- PE override support
- Diagnostic cvars (`headless_verbose_diagnostics`, `headless_thread_diagnostics`)
- Dead code removal (MonitorMainThreadPC, unsafe backtrace)
- Quality fixes (EXIT_SUCCESS bug, duplicate declarations, unused cvars)

### B. Uncommitted working tree delta

**xenia** (this repo):
- `src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc` — CRT critical section diagnostics and auto-init for pre-initialized CS with lock_count=0

**jeff** (cross-repo: `../../jeff/`):
- `src/util/split.rs` — `split_crt_initializers()` for .CRT entry co-location + `??__E*` COMDAT exclusion
- `src/util/xex.rs` — `.CRT` → `.CRT$XCU` section rename in COFF emission

### C. Dirty submodules (not yet integrated)

Current dirty submodules:

- `third_party/cxxopts`
- `third_party/date`
- `third_party/fmt`
- `third_party/imgui`
- `third_party/premake-core`

These include local compatibility tweaks and heavy header edits. They increase integration risk until explicitly upstreamed or reverted.

## Confidence / Risk Audit

### What looks sound

- Baseline architecture direction (headless + selective rendering + replay + capture) is coherent.
- DC3-specific boot unblockers (NUI/audio/object type handling) are plausible and evidence-backed.
- **Deferred draw rendering now produces real content** — cache invalidation fix confirmed working.
- Diagnostic logging properly gated behind cvars, defaulting to off.
- Signal handler safety fixed (backtrace removed, RIP capture preserved).
- Quality issues resolved (EXIT_SUCCESS bug, unused cvars, duplicate declarations).

### Where this is still fragile

1. **Rendering quality still incomplete** — E20 captures show real content but overall frame is dark.
   - Possible gamma ramp, format conversion, or readback path issue.
   - Need comparison with inline draw output to isolate.

2. **E10 (force_all_draws) produces flat color** — different failure mode from deferred.
   - All draws may be rendering identically wrong, or capture timing issue.

3. **Inline draw deadlock** — still untested this session.
   - Frame-12 deadlock may still block reference comparisons.

4. **Dirty submodules** — 5 submodules with local modifications.
   - Low-risk compiler/build fixes but increase integration complexity.

### Resolved risks (from previous audit)

- ~~Deadlock diagnosis code mixed into signal handler~~ — FIXED: `backtrace()` removed
- ~~Hot-path logging in timing-sensitive paths~~ — FIXED: gated behind cvars
- ~~Quality issues (EXIT_SUCCESS, duplicate declarations)~~ — FIXED
- ~~Deferred draws produce B=0x3F everywhere~~ — FIXED: cache invalidation

## Roadmap (Priority Order)

### Priority 0: Repro discipline and observability hygiene (immediate)

- Freeze a small run matrix and keep it constant for comparisons:
  - `force_all_draws=true` capture run
  - non-force deferred run
  - optional inline-only diagnostic run
- Gate verbose logs behind cvars and default them off.
- Save artifacts per run (log + frame set + config line).

Exit criteria:

- Repeated runs produce comparable metrics with minimal observer effect.

### Priority 1: Rendering correctness (critical path)

Parallel track A: deferred path quality fix.

- Use Vulkan validation (`--vulkan_validation`) while replaying deferred draws.
- Compare resolve/copy register state at defer time vs replay time.
- Verify swap texture source correctness before copy-to-staging.
- Validate that copy deferral/replay ordering preserves EDRAM semantics.

Parallel track B: inline deadlock root cause.

- Re-test inline mode with minimal logging.
- Identify exact wait edge causing frame-12 deadlock (submission fence, WAIT_REG_MEM progression, or sync write visibility).
- If inline can be stabilized, it is a quality reference and fallback path.

Exit criteria:

- Non-black captures with expected color characteristics in non-force deferred mode.
- 120s run with periodic captures and no deadlock.

### Priority 2: Stabilize and de-risk kernel/shim layer

- Keep only behavior required for DC3 boot progress.
- Convert tactical logging into controlled diagnostics.
- Add a short compatibility matrix for stubs used by DC3 (NUI, XAM UI, audio, object manager, I/O).

Exit criteria:

- Boot behavior unchanged with diagnostics mostly disabled.

### Priority 3: Branch hygiene and patch slicing

- Split changes into:
  - core functional fixes
  - diagnostics
  - experimental-only changes
- Resolve submodule dirtiness explicitly.
- Keep a small, reviewable patch stack.

Exit criteria:

- Clean narrative from baseline to current behavior.

### Priority 4: PE override activation (after rendering is trustworthy)

- Revisit decomp PE layout alignment once rendering/capture is stable.
- Validate section/function address parity requirements for override safety.

Exit criteria:

- Override run reaches same boot landmarks as original with reliable frame evidence.

## Experiment Matrix (Next Session)

Use this matrix to keep runs comparable and isolate one variable at a time.

Common setup for all Vulkan experiments:

- Binary: `/home/free/code/milohax/xenia/build/bin/Linux/Checked/xenia-headless`
- Target: `/home/free/code/milohax/dc3-decomp/orig/373307D9/default.xex`
- Duration: `--headless_timeout_ms=120000`
- Scripted input: `--scripted_input='5s:A,7s:START'`
- Capture cadence: `--headless_capture_interval=100` (unless noted)
- Per-run output folder: `/tmp/dc3-exp/<ID>/`

| ID | Mode / Variable | Command Delta | Goal | Pass Criteria |
|---|---|---|---|---|
| E00 | Null GPU baseline | `--gpu=null` | Confirm game-thread/input stability without render load | 120s run, no crash, swap cadence roughly stable |
| E10 | Vulkan full-draw baseline | `--gpu=vulkan --force_all_draws=true` | Establish reference capture behavior with maximum draw execution | Non-black captures and stable 120s run |
| E11 | Vulkan full-draw + validation | E10 + `--vulkan_validation=true` | Catch API/sync errors in known-good scheduling mode | No validation errors that correlate with bad frames |
| E20 | Vulkan deferred (target mode) | `--gpu=vulkan --force_all_draws=false` | Validate current intended RENDER+DEFER behavior | Non-black captures in capture windows and stable 120s run |
| E21 | Vulkan deferred + validation | E20 + `--vulkan_validation=true` | Check deferred replay path for GPU API/sync violations | No validation errors and output quality matches E20 |
| E30 | Inline draw deadlock probe | Local diagnostic toggle: force inline draws (disable deferral for test) | Reproduce frame-12 deadlock with minimal noise | Stall point identified with specific wait/sync site |
| E40 | Frontbuffer/source audit | E20 with short run (`--headless_timeout_ms=30000`) and dense capture (`--headless_capture_interval=10`) | Verify swap source, pointer behavior, and capture timing | Pointer/source behavior is explainable and captures align with expected render windows |
| E50 | Pipeline cache sensitivity | Run E20 twice: cold cache then warm cache | Determine whether cache state changes correctness vs only performance | Same qualitative output across cold/warm runs |

Suggested run command template:

```bash
mkdir -p /tmp/dc3-exp/<ID>
/home/free/code/milohax/xenia/build/bin/Linux/Checked/xenia-headless \
  --target=/home/free/code/milohax/dc3-decomp/orig-assets/default.xex \
  --dump_frames_path=/tmp/dc3-exp/<ID>/ \
  --headless_capture_interval=100 \
  --headless_timeout_ms=120000 \
  --scripted_input='5s:A,7s:START' \
  <EXTRA_FLAGS> \
  > /tmp/dc3-exp/<ID>/run.log 2>&1
```

For E50 cold-cache run, remove Vulkan pipeline cache before the first run:

```bash
rm -f /tmp/claude/xenia_vulkan_pipeline_cache.bin
```

Artifacts to collect per experiment:

- `run.log`
- `frame_*.ppm`
- quick per-frame non-zero pixel summary
- counts from logs: swaps, deferred draws flushed, validation errors (if enabled)

## Suggested Next Session Checklist

1. Run a controlled deferred capture test with Vulkan validation and minimal extra logs.
2. Run an inline diagnostic test with deadlock-focused tracing only.
3. Compare one known frame across modes (register snapshots + capture output).
4. Mark each current uncommitted change as `keep`, `gate`, or `drop`.
5. Start splitting diagnostics from functional changes before adding new behavior.

## debug.xex Findings and NuiInitialize Blocker (2026-02-20 Late Session)

### Summary

The debug.xex (DC3 debug build) halts early on `NuiInitialize` failure, while the retail default.xex continues past it. This means debug.xex cannot be used for runtime comparison without a NUI initialization shim or bypass.

### Observations

- **NuiInitialize returns error `0x8301000b`**: The Kinect/NUI subsystem fails to initialize (expected in emulation).
- **Debug build treats this as fatal**: Shows a blue Milo debug console screen with "Program ended". The retail build logs the error but continues.
- **52 captures collected**: All 52 frame captures from the debug.xex run show the identical crash/halt screen (blue Milo debug console).
- **Build info visible on debug screen**: Build 120916, Plat: xbox, SystemConfig: config/ham_preinit_keep.dta

### CLI Corrections Discovered

The initial crash was not a packaging issue. The real problem was incorrect CLI flag names:
- `--headless_timeout` does not exist; correct flag is `--headless_timeout_ms`
- `--capture_start_frame` does not exist
- XEX path must use `--target=` prefix, not positional argument

Correct minimal invocation:
```bash
./xenia-headless --gpu=vulkan --headless_timeout_ms=90000 \
    --dump_frames_path=/tmp/frames/ --headless_capture_interval=100 \
    --target=/path/to/default.xex
```

### SDL2-compat Crash Path

On Arch Linux, `sdl2` has been replaced by `sdl2-compat` (an SDL3 shim). When Xenia attempts to display error dialogs, the path SDL2-compat -> SDL3 -> zenity crashes if zenity is not installed. Installing `zenity` prevents this crash. Not required for normal headless operation where no error dialogs are triggered.

### Impact on Headless Testing

- **default.xex (retail)**: Remains the primary test target. DC logo boot animation renders correctly with deferred draw cache fix.
- **debug.xex**: Blocked until NuiInitialize is shimmed to return success, or the debug build's error handling is bypassed. The current `XamShowNuiSigninUI` stub is not sufficient because `NuiInitialize` itself fails before sign-in is attempted.

### Potential Fix

The `NuiInitialize` import (from `xam.xex`) needs to return success (`0x00000000`) instead of the current failure. This is in the XAM NUI shim layer (`src/xenia/kernel/xam/xam_nui.cc`). The existing `NuiInitialize` stub may be returning an error status or not handling the debug build's specific initialization sequence.

## Decomp XEX CRT Deadlock Investigation (2026-02-21 Session)

### Problem Statement

The decomp XEX boots in xenia-headless, resolves all 707 imports, patches 60 NUI stubs, and enters the main loop — then **hangs at `RtlEnterCriticalSection`** (LR=`0x830DBFC8`). The stack has only 3 frames with `0xBEBEBEBE` return addresses (uninitialized stack markers), indicating the hang occurs very early in CRT startup, before the game's `main()` is reached.

### Root Cause: XapiProcessLock Pre-Initialized with lock_count=0

**Address**: `0x830DBFC8` maps to `XapiCallThreadNotifyRoutines` (starts at `0x830DBFA4`) from `xregisterthreadnotifyroutine.obj`.

**CRT startup chain**: `mainCRTStartup` (0x830DB5B8) → `XapiInitProcess` → `XapiCallThreadNotifyRoutines` → `_mtinit` → `_rtinit` → `_cinit` → `main`

**Root cause**: `XapiProcessLock` at `0x82F0844C` is a pre-initialized `X_RTL_CRITICAL_SECTION` in `.data`. Jeff only captures 8 bytes of symbol extent for it (the `X_DISPATCH_HEADER` type and signal_state fields). The critical `lock_count` field at byte offset 0x10 ends up as 0 (zero-initialized) instead of -1 (`0xFFFFFFFF`).

With `lock_count=0`, xenia's `RtlEnterCriticalSection` implementation does `atomic_inc(&cs->lock_count)` which returns 1 (non-zero), causing it to enter `xeKeWaitForSingleObject` which waits forever — no thread actually holds the lock.

**Key detail**: `XapiInitProcess` does NOT call `RtlInitializeCriticalSection` for `XapiProcessLock` — it relies entirely on pre-initialized `.data` section values. This is a known Xbox 360 pattern mentioned in xenia's own source comments.

### CRT Path Audit Result

All 97 unresolved `lbl_*` symbols are in game logic/physics code sections. **None** are in the CRT startup address range (`0x830DB000`–`0x830DD000`). All `bl` instructions in CRT functions resolve correctly to valid targets.

The `??__E*` CRT dynamic initializer symbols (26 unresolved per the original plan) are **not present in the current linker output** — they may have been resolved in a prior build iteration or the count was from an earlier analysis.

### Changes Made

#### 1. Xenia: CRT Critical Section Diagnostics + Auto-Init

**File**: `src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc`

Added cvar `crt_critical_section_diagnostics` (default: false) that enables:
- Logging of all `RtlInitializeCriticalSection` and `RtlInitializeCriticalSectionAndSpinCount` calls with caller LR and thread ID
- Detection of uninitialized CS (type != 1) or CS with lock_count=0 (should be -1 after proper init)
- Auto-initialization of detected bad CS before proceeding, preventing deadlock
- Periodic logging (first 50 calls + every 1000th) for runtime monitoring

This is the immediate fix for the deadlock — it catches the XapiProcessLock case and any similar pre-initialized critical sections.

#### 2. Jeff: CRT Initializer Co-location

**File**: `jeff/src/util/split.rs`

Added `split_crt_initializers()` function that:
- Iterates `.CRT` section entries (4-byte function pointers with ADDR32 relocations)
- For each relocation, finds the target initializer function (typically `??__E*` symbols)
- Ensures both the `.CRT` entry and target function are placed in the same compilation unit
- Marks referenced symbols as `force_active` to prevent dead-stripping

Also excluded `??__E*` symbols from COMDAT marking (they must not be deduplicated).

Called from `update_splits()` when a non-empty `.CRT` section is found.

#### 3. Jeff: .CRT → .CRT$XCU Section Rename

**File**: `jeff/src/util/xex.rs`

In `write_coff()`, `.CRT` sections are renamed to `.CRT$XCU` so the MSVC linker places them correctly in the CRT initializer array between the `$XCA` (start sentinel) and `$XCZ` (end sentinel) markers.

### Validation Status

- **Not yet built or tested** — shell environment was unavailable during the session
- Xenia changes compile-ready (C++ with existing includes/patterns)
- Jeff changes need `cargo build --release` in the jeff repo
- Full validation requires: jeff rebuild → dc3-decomp rebuild → XEX package → boot test

### Next Steps

1. Build xenia with diagnostics enabled, boot decomp XEX with `--crt_critical_section_diagnostics=true`
2. Rebuild jeff and relink decomp to verify CRT co-location fix
3. Confirm decomp XEX gets past CRT init (no `RtlEnterCriticalSection` hang)
4. Look for first `VdSwap` call (rendering loop entered)

## Decomp XEX CRT Fixes and Linux Platform Audit (2026-02-21 Session 4)

### Summary

Resolved the CRT init deadlock chain (4 separate root causes) and two critical Linux platform bugs. The decomp XEX now boots past CRT startup and crashes in game initialization code — significant progress from the original CRT hang.

### Boot Progress (Run 25)

| Metric | Value |
|--------|-------|
| Imports resolved | 707 (100%) |
| NUI functions patched | 60 |
| RtlEnterCriticalSection calls | 99 (balanced with 99 leaves) |
| RtlInitializeCriticalSection calls | 2 |
| SIGSEGV count | 1 (properly handled) |
| Stack usage | ~100KB of 1024KB (healthy) |
| Crash PC | 0x8362906C (game code, past CRT init) |
| Thread state at crash | Suspended by emulator crash handler |

### Fixes Applied

#### Fix 1: Critical Section wait_list Initialization

**File**: `src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc`

**Root cause**: `xeRtlInitializeCriticalSection` and `xeRtlInitializeCriticalSectionAndSpinCount` did not initialize `wait_list_flink` and `wait_list_blink` in the `X_DISPATCH_HEADER`. When `StashHandle()` later wrote `kXObjSignature` (0x58454E00 = "XEN\0") into these fields, it corrupted the `XapiThreadNotifyHead` list at 0x83B0F03C.

**Fix**: Initialize wait_list fields as self-referential empty circular list (matching real Xbox 360 kernel behavior):
```cpp
uint32_t wait_list_ptr = cs_ptr + offsetof(X_DISPATCH_HEADER, wait_list_flink);
cs->header.wait_list_flink = wait_list_ptr;
cs->header.wait_list_blink = wait_list_ptr;
```

#### Fix 2: CRT Thread Notify Function Stubs

**File**: `src/xenia/emulator.cc` (DC3 title-specific patches)

Stubbed `XapiCallThreadNotifyRoutines` at both call sites (0x830DBC18 and 0x830DBC88) with `li r3, 0; blr`. These functions iterate the thread notify list which was corrupted, and they're not essential for decomp XEX boot.

#### Fix 3: NUI Static Initializer Adjacency Bug

**File**: `src/xenia/emulator.cc` (NUI patch table)

**Root cause**: The NUI patch at 0x82A24AB0 (`NuiSpeechEmulateRecognition`) wrote `li r3, 0; blr` into the body of the adjacent function at 0x82A24A98. That function's `bl __savegprlr` set LR=0x82A24AA0, and when the patched `blr` at 0x82A24AB0 executed, it returned to 0x82A24AA0 (inside the same function), creating infinite recursion that overflowed any stack size.

**Fix**: Added 0x82A24A98 to the NUI patch list as a standalone stub.

#### Fix 4: Main Thread Stack Size

**File**: `src/xenia/emulator.cc` + `src/xenia/kernel/user_module.h`

Increased main thread stack from 256KB (XEX default) to 1024KB. Added `set_stack_size()` to `UserModule`. The 256KB stack was insufficient for the CRT startup chain even without infinite recursion.

#### Fix 5: QueryProtect Linux Implementation

**File**: `src/xenia/base/memory_posix.cc`

**Root cause**: `QueryProtect()` was a stub that always returned false. This caused the MMIO handler to use uninitialized `cur_access`, return true ("handled") without fixing anything, and the faulting instruction would retry infinitely.

**Fix**: Implemented by parsing `/proc/self/maps` to find the mapping containing the address and extracting its rwx permissions.

#### Fix 6: MMIO Handler QueryProtect Return Check

**File**: `src/xenia/cpu/mmio_handler.cc`

**Root cause**: The MMIO handler's `ExceptionCallback` did not check `QueryProtect`'s return value before using `cur_access`. When QueryProtect returned false, `cur_access` was uninitialized.

**Fix**: Added return value check: `if (memory::QueryProtect(...) && cur_access != ...)`.

#### Fix 7: Unhandled SIGSEGV Crash Handler

**File**: `src/xenia/base/exception_handler_posix.cc`

**Root cause**: When no registered exception handler handles a signal, the signal handler function returned without action. The kernel re-executed the faulting instruction, triggering the same signal again — infinite loop with no crash dump or diagnostic output.

**Fix**: When no handler handles the exception, restore the original signal handler (saved during `Install()`) and return. The instruction re-executes, the original handler (or SIG_DFL) produces a proper crash/core dump.

### Linux Platform Audit Results

Comprehensive audit of Linux-specific stubs and incomplete implementations.

#### CRITICAL (Fixed This Session)

| Issue | File | Status |
|-------|------|--------|
| `QueryProtect()` always returns false | `memory_posix.cc:116` | **FIXED** |
| Unhandled SIGSEGV infinite loop | `exception_handler_posix.cc:239` | **FIXED** |

#### CRITICAL (Still Open)

| Issue | File | Impact |
|-------|------|--------|
| `AlertableSleep()` doesn't implement alertability | `threading_posix.cc:170-177` | I/O callbacks won't interrupt sleep; games using overlapped I/O may deadlock |

#### HIGH (Not Blocking DC3 Boot)

| Issue | File | Impact |
|-------|------|--------|
| `StackWalker::Create()` returns nullptr | `stack_walker_posix.cc:17-21` | No stack traces for debugging/crash reports |
| `LookupUnwindInfo()` returns nullptr | `x64_code_cache_posix.cc:24` | Exception unwinding across JIT code broken |
| `ChunkedMappedMemoryWriter::Open()` returns nullptr | `mapped_memory_posix.cc:132-137` | Trace recording disabled |
| `EnableAffinityConfiguration()` empty | `threading_posix.cc:140-141` | Thread pinning config noop |
| `WaitMultiple()` deadlock risk (issue #1677) | `threading_posix.cc:245-249` | Intermittent deadlock when threads suspended during wait |

#### MEDIUM

| Issue | File | Impact |
|-------|------|--------|
| `Protect()` asserts `out_old_access` is null | `memory_posix.cc:107-114` | Cannot query old permissions during protect |
| `FileHandle::OpenExisting()` error handling TODO | `filesystem_posix.cc:187-191` | Imprecise error reporting |

### Current Guest Crash (Next Investigation Target)

The guest crashes at PC=0x8362906C, which is in game initialization code (well past CRT init). Register state shows r9=0xFFFFFFFF82000000 (sign-extended address for RODATA section at 0x82000000). The crash is a SIGSEGV writing to the read-only section.

This could be:
1. A decomp code issue (writing to RODATA)
2. A section permissions mismatch between original and decomp XEX
3. An address calculation issue in the JIT

The emulator properly catches this crash, prints a full register dump, and suspends the thread (confirming Fixes 5-7 work correctly).

### Files Modified (Cumulative Uncommitted)

| File | Changes | Purpose |
|------|---------|---------|
| `src/xenia/app/emulator_headless.cc` | +60 lines | Diagnostic thread: SIGSEGV counter, IAT dump, thunk code dump, PPC stack walk, register dump, heap query |
| `src/xenia/base/exception_handler.h` | +5 lines | GetSigsegvCount/GetLastFaultAddress/GetLastFaultRip declarations |
| `src/xenia/base/exception_handler_posix.cc` | +40 lines | SIGSEGV tracking + unhandled signal crash fix |
| `src/xenia/base/memory_posix.cc` | +35 lines | QueryProtect implementation via /proc/self/maps |
| `src/xenia/cpu/mmio_handler.cc` | +2 lines | QueryProtect return value check |
| `src/xenia/cpu/processor.cc` | +3 lines | ResolveFunction diagnostic for 0x83A00964 |
| `src/xenia/emulator.cc` | +80 lines | DC3 CRT stubs, NUI patch fix, stack size increase |
| `src/xenia/kernel/user_module.h` | +1 line | set_stack_size() accessor |
| `src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc` | +90 lines | CS wait_list init, diagnostic counters, auto-init |
| `src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.h` | +5 lines | Diagnostic counter accessor declarations |

## Decomp NUI Patch Corruption and Heap Crash (2026-02-21 Session 5)

### Summary

Identified and fixed the decomp XEX game thread spin loop (100% CPU hang). Root cause: NUI stub patches were using original retail XEX addresses, corrupting unrelated decomp code. Created decomp-specific NUI patch table with correct addresses from the decomp MAP file. The game now progresses significantly further (RtlEnterCS=186 vs 101) before crashing in `RtlpCoalesceFreeBlocks` with heap corruption.

### Boot Progress (Run 26+)

| Metric | Before (Session 4) | After (Session 5) |
|--------|--------------------|--------------------|
| RtlEnterCriticalSection calls | 99 | 186 |
| RtlLeaveCriticalSection calls | 99 | 186 |
| RtlInitializeCriticalSection calls | 2 | 10 |
| SIGSEGV count | 1 | 1 |
| Game thread state | Spinning at 100% CPU | Crashes in heap code |
| Crash function | gPropPaths (infinite loop) | RtlpCoalesceFreeBlocks (heap corruption) |

### Problem 1: Game Thread Spin Loop at 100% CPU

After Session 4's CRT fixes, the game thread entered a JIT spin loop consuming 100% CPU. PPCContext registers are stale during JIT execution (only update at kernel call boundaries), so conventional diagnostics showed stale LR=0x82A24B74.

#### JIT IP Sampler Implementation

**File**: `src/xenia/app/emulator_headless.cc`

Since PPCContext is stale during JIT execution, implemented a signal-based JIT instruction pointer sampler:

1. **SIGUSR2 handler** (`JitIpSampleHandler`): Captures x86 RIP from the signal's `ucontext_t`, stores in `g_sampled_rip` atomic.
2. **Sampling**: Diagnostic thread sends `pthread_kill(pt, SIGUSR2)` to the game thread, waits briefly for the handler to fire.
3. **Host→Guest mapping**: Uses `code_cache->LookupFunction(host_rip)` to find the JIT function, then `MapMachineCodeToGuestAddress(host_rip)` to get the exact guest PPC address.

This is guarded by `#ifdef __linux__` (uses `ucontext.h`, `pthread.h`, `signal.h`).

**Result**: All samples mapped to guest 0x82A24B74 and 0x82A24B80, within `??__EgPropPaths@@YAXXZ` (Object.obj) — a CRT static initializer.

### Problem 2: NUI Patches Corrupting Decomp Code

#### Root Cause

All 59 NUI patches used addresses from the **original retail XEX**. The decomp XEX has a completely different function layout (linker placed functions at different addresses). The patches wrote `li r3, 0; blr` on top of unrelated decomp code.

**Specific corruption**: The `NuiSpeechEnable` patch at 0x82A24B88 overwrote the `gPropPaths` static initializer body, creating an infinite loop:
```
0x82A24B74: mr r4,r3           ; (original decomp code)
0x82A24B78: b 0x82A24B80       ; jumps to...
0x82A24B80: lis r11,...         ; (original decomp code)
0x82A24B84: addi r28,...        ; (original decomp code)
0x82A24B88: li r3, 0           ; ← PATCHED (was part of gPropPaths)
0x82A24B8C: blr                ; ← PATCHED → returns to LR=0x82A24B74 → infinite loop
```

#### Fix: Decomp Layout Detection + Decomp Patch Table

**File**: `src/xenia/emulator.cc`

**Step 1 — Layout detection**: Two-pass zero-padding check. Count how many original patch addresses contain `0x00000000` in the loaded XEX. If > 25%, the XEX uses decomp layout:
```cpp
int zero_count = 0;
for (const auto& patch : patches) {
    auto* mem = memory_->TranslateVirtual<uint8_t*>(patch.address);
    if (mem && xe::load_and_swap<uint32_t>(mem) == 0x00000000) {
        zero_count++;
    }
}
bool is_decomp_layout = (zero_count > total_patches / 4);
```

**Step 2 — Decomp patch table**: Created `decomp_patches[]` array with ~44 NUI function entries using correct addresses from the decomp MAP file (`dc3-decomp/build/373307D9/default.map`). Example entries:
```cpp
NuiPatch decomp_patches[] = {
    {0x83681898, kLiR3_0, kBlr, "NuiInitialize"},
    {0x8367F45C, kLiR3_0, kBlr, "NuiShutdown"},
    {0x83672DC8, kLiR3_0, kBlr, "NuiSkeletonTrackingEnable"},
    // ... 41 more entries ...
};
```

**Step 3 — Active table selection**: At patch time, select between original and decomp tables:
```cpp
const NuiPatch* active_patches = is_decomp_layout ? decomp_patches : patches;
```

**Result**: All decomp patches apply correctly (55/55 in original session, refined to 44 in decomp table). Each target has valid function prologues (mostly `7D8802A6` = `mflr r12`), confirming correct addresses.

### Problem 3: Heap Corruption in RtlpCoalesceFreeBlocks

With correct NUI patches applied, the game progresses further but crashes in the CRT heap manager:

| Detail | Value |
|--------|-------|
| Crash function | `RtlpCoalesceFreeBlocks` at 0x830DC6AC |
| Crash type | SIGSEGV — NULL pointer read |
| Fault address | Guest 0x00000000 |
| r31 | 0x536F7264 ("Sord" in ASCII) — used as heap entry pointer |
| r25 | 0xFEEEFEEE (Windows debug fill for freed memory) |
| RtlEnterCS count | 186 (significant progress from 101) |
| RtlInitCS count | 10 |

**Analysis**: The heap is corrupted — a string value ("Sord", likely from "Sword" or "Sort" + 'd') has overwritten heap management structures. `0xFEEEFEEE` is the Windows debug heap fill pattern for freed memory, indicating a use-after-free or double-free scenario. This crash occurs during CRT static initialization (before game `main()`).

**Likely causes**:
1. **NUI stubs returning S_OK without initializing expected state**: Some NUI functions may be expected to populate global structures that later code reads/frees. Returning success without doing so leaves dangling pointers.
2. **Decomp code bug**: A buffer overflow or incorrect memory allocation in a CRT static initializer.
3. **Missing NUI function stubs**: Some NUI functions not in our patch table may have been called and performed partial initialization.

### Files Modified (This Session)

| File | Changes | Purpose |
|------|---------|---------|
| `src/xenia/app/emulator_headless.cc` | +50 lines | JIT IP sampler (SIGUSR2 handler, pthread_kill sampling, code_cache mapping) |
| `src/xenia/emulator.cc` | +120 lines | Decomp layout detection, decomp NUI patch table (~44 entries), active table selection |

### Next Investigation Steps

1. **Try returning E_FAIL from NuiInitialize** instead of S_OK — prevent downstream NUI usage that may expect initialized state
2. **Check if original retail XEX also crashes here** — isolate decomp bug vs xenia bug
3. **Add missing NUI function stubs** that may perform partial initialization
4. **Trace the heap corruption source** — use watchpoints on the corrupted heap entry to find the writer

## Jeff BSS + COMDAT Fixes (2026-02-21 Session 6)

### Summary

Two jeff COFF emitter bugs were identified and fixed. A third root cause for the `gPoolCapacity` crash was discovered: the source-compiled `FilePath.obj` replaces the split version without defining `gPoolCapacity`, causing `/FORCE` to resolve it to address 0 (= 0x82000000, RODATA).

### Fix 1: BSS Section Size

**File**: `jeff/src/util/xex.rs` (line ~1563)

BSS sections were created via `add_section()` but `append_section_bss()` was never called. The COFF BSS section had `SizeOfRawData = 0`.

```rust
if sect.kind != ObjSectionKind::Bss {
    cur_coff.append_section_data(sect_id, &data, sect.align);
} else {
    cur_coff.append_section_bss(sect_id, sect.size, sect.align);
}
```

**Result**: FilePath.obj `.bss` section now has RawSize=24 (was 0).

### Fix 2: COMDAT Data Section Extraction

**File**: `jeff/src/util/xex.rs` (line ~1520)

The `object` crate's COFF writer emits all section symbols before regular symbols, breaking the COFF spec requirement that a COMDAT symbol immediately follows its section symbol. This caused the MSVC linker to silently drop `.data$dup` and `.rdata$dup` COMDAT sections.

**Before**: All section kinds extracted to COMDAT (`.text$dup`, `.rdata$dup`, `.data$dup`)
**After**: Only code sections extracted to COMDAT (`.text$dup`, `.text$x`). Data stays in parent sections.

**Evidence**: COFF dump of FilePath.obj showed section symbols #8..#14 grouped before COMDAT symbols #16+, violating the COFF spec. 404 LNK2019 unresolved externals included 110 string literals from dropped `.rdata$dup` COMDAT sections.

**Impact**: 3,351 new LNK4006 warnings (duplicate symbols) — harmless with `/FORCE`. String literals and data globals that were silently dropped now stay in parent sections and resolve correctly.

### Root Cause for gPoolCapacity: Source Object Replacement

**Discovery**: The split `obj/system/utl/FilePath.obj` defines `gPoolCapacity` and `gPoolAllocInitted`. But it is NOT linked — the source-compiled `src/system/utl/FilePath.obj` replaces it. The source version does not define these variables.

**Linker behavior**: With `/FORCE:UNRESOLVED`, unresolved external symbols resolve to address 0. In the PE, address 0 maps to RVA 0x00000000 → guest 0x82000000 (image base / RODATA section).

**Link input analysis**:
- Total objects linked: 2,224 (1,972 split + 252 source-compiled)
- FilePath.obj: source-compiled version used (does not define gPoolCapacity)
- PoolAlloc.obj: split version used (references gPoolCapacity as UNDEF)
- Result: LNK2019 unresolved external for `?gPoolCapacity@@3HA`

### Xenia Change: RODATA Workaround Updated

**File**: `src/xenia/emulator.cc`

The RODATA workaround (making 0x82000000-0x82410000 writable) is **retained** with updated comments explaining the true root cause: unresolved BSS globals from source-object replacement resolve to the image base via `/FORCE`.

### NUI Patch Table Staleness

The COMDAT fix changed the link layout significantly (image: 33.5MB → 28.2MB). The hardcoded NUI patch addresses in `emulator.cc` are now stale and must be regenerated from the new MAP file before decomp XEX testing.

### Test Results

- **Without NUI patch corruption**: Game boots past CRT, runs 10 seconds without crash (null GPU)
- **With stale NUI patches**: Immediate SIGSEGV (patches corrupt random code at wrong addresses)

### Files Modified

| File | Change |
|------|--------|
| `jeff/src/util/xex.rs:~1563` | BSS `append_section_bss()` call |
| `jeff/src/util/xex.rs:~1520` | Only extract code sections to COMDAT |
| `xenia/src/xenia/emulator.cc` | RODATA workaround updated with correct root cause comment |

### Next Steps

| Priority | Action | Location |
|----------|--------|----------|
| HIGH | Define `gPoolCapacity`/`gPoolAllocInitted` in dc3-decomp FilePath.cpp | dc3-decomp |
| HIGH | Regenerate NUI patch table from new MAP file | xenia emulator.cc |
| MEDIUM | Audit other LNK2019 unresolved for source-replacement gaps | dc3-decomp |
| LOW | Fix `object` crate fork COMDAT symbol ordering for COFF | rjkiv/object |

## Session 7 — NUI Patch Regen + gPoolCapacity + CRT Init Fixes (2026-02-21)

### Context

Session 6 applied jeff COFF fixes (BSS section + COMDAT data extraction). Game booted 10s
without crash when NUI patches didn't interfere, but the NUI patch table had stale addresses.

### Work Completed

**NUI Patch Table Regenerated:**
- All 56 `decomp_patches[]` entries updated from current MAP file
- Every patch confirmed hitting valid function prologues (not zero-padding)
- Added `NuiMetaCpuEvent` at `0x834127E0` (was missing from decomp table)
- `XapiCallThreadNotifyRoutines` stubs updated to `0x82F51108` / `0x82F51178`
- Comments updated with new data addresses (`XapiThreadNotifyRoutineList` = `0x8366F35C`, `XapiProcessLock` = `0x8366F340`)

**gPoolCapacity Investigation — splits.txt boundary fix:**
- Root cause: `splits.txt` had `.bss` boundary at `0x830E5730`, putting `gPoolCapacity` (`0x830E5728`) and `gPoolAllocInitted` (`0x830E572C`) in FilePath.cpp instead of PoolAlloc.cpp
- Fix: moved boundary to `0x830E5728` in `dc3-decomp/config/373307D9/splits.txt`
- After rebuild: both symbols now in MAP at writable BSS (`0x83890A40`, `0x83890A44`)
- **Not a jeff bug** — splits.txt is manually authored; jeff reads boundaries, doesn't generate them. Jeff could add validation (detect when user boundaries bisect symbols) as a feature improvement.

**Additional Fixes Discovered During Testing:**

| Fix | File | Description |
|-----|------|-------------|
| Null guard for `RtlImageXexHeaderField` | `xboxkrnl_rtl.cc` | Game passes null XEX header; returns 0 instead of SIGSEGV |
| Crash dump assertion softened | `emulator.cc:582` | `assert_not_null(guest_function)` → log host PC + guest LR |
| CRT initializer table sanitizer | `emulator.cc` | Scans `__xc_a..__xc_z`, nullifies 26/390 entries at image base |
| Zero page mapping (guest 0x0-0x10000) | `emulator.cc` | CRT constructors survive null-pointer dereferences |

### Boot Progression

| Stage | Status | Notes |
|-------|--------|-------|
| XEX load + import resolution | ✅ | 302 thunks, 470 variables resolved |
| NUI function stubbing | ✅ | 56/56 decomp patches applied correctly |
| XapiCallThreadNotifyRoutines | ✅ | Both variants stubbed at new addresses |
| CRT `_cinit` C++ constructors | ✅ | 26 unresolved entries nullified, null deref guard |
| `RtlImageXexHeaderField` | ✅ | Null header guard prevents crash |
| `FixedSizeAlloc::Refill()` | ❌ | Calls null function pointer (CTR=0) at `0x83345B54` |

### Current Blocker: `FixedSizeAlloc::Refill()` null callback

- Guest PC: `0x83345B54` = `bctrl` in `FixedSizeAlloc::Refill()` (PoolAlloc.obj)
- CTR = 0 → calling null function pointer
- Call stack: `mainCRTStartup` → `_cinit` → `__xc_a` constructors → ... → `FixedSizeAlloc::Refill()`
- The refill callback in the FixedSizeAlloc object was never set (likely depends on `gPoolAllocInitted` / `gPoolCapacity` being properly initialized BEFORE this constructor runs)

### Files Modified (This Session)

| File | Change |
|------|--------|
| `xenia/src/xenia/emulator.cc` | NUI decomp patches (56 addresses), Xapi stubs, CRT sanitizer, zero page, crash dump |
| `xenia/src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc` | Null guard for RtlImageXexHeaderField |
| `dc3-decomp/config/373307D9/splits.txt` | .bss boundary fix: 0x830E5730 → 0x830E5728 |

### Next Steps (post-session 7)

| Priority | Action | Location |
|----------|--------|----------|
| HIGH | Fix FixedSizeAlloc::Refill() null callback — investigate why refill function pointer is null | dc3-decomp PoolAlloc.cpp |
| HIGH | Check CRT constructor ordering — gPoolCapacity init may need to run before FixedSizeAlloc ctors | dc3-decomp .CRT sections |
| MEDIUM | Investigate remaining 404 LNK2019 unresolved externals for runtime impact | dc3-decomp link |
| MEDIUM | Consider removing RODATA workaround now that gPoolCapacity is in BSS | xenia emulator.cc |
| LOW | Feature request: jeff split validation (warn when user boundaries bisect symbols) | jeff split.rs |

---

## Session 8 (2026-02-22): Data COMDAT vtable fix + boot milestone

### Problem

`FixedSizeAlloc::Refill()` crashed at guest PC `0x83345B54` calling `RawAlloc()` via vtable dispatch with
CTR=0. The vtable at `0x82174C58` (MultiMesh.obj) contained null entries `[0, 0]` for both the scalar
deleting destructor and `RawAlloc`.

Four functions were missing from the MAP:
1. `?RawAlloc@FixedSizeAlloc@@MAAPAHH@Z` (virtual method)
2. `?RawAlloc@ReclaimableAlloc@@MAAPAHH@Z` (virtual override)
3. `??_GFixedSizeAlloc@@UAAPAXI@Z` (scalar deleting destructor)
4. `??_GReclaimableAlloc@@UAAPAXI@Z` (scalar deleting destructor)

### Investigation

1. **COFF structural analysis:** All COMDAT sections in PoolAlloc.obj and MultiMesh.obj are
   structurally identical (same flags `0x60301020`, `SELECT_ANY`, `assoc_sect=0`). No difference
   between "working" and "broken" sections.

2. **No duplicate definitions:** Scanned all 2,223 .obj files — each of the four symbols is defined
   in exactly one .obj file with no duplicates.

3. **No LNK2019 reported:** The linker does not report these as unresolved. With `/FORCE:UNRESOLVED`,
   it silently resolves them to zero.

4. **Smoking gun: vtable has no relocations.** MultiMesh.obj's `.rdata` section contains the vtable
   at offset 0 with raw bytes `00 00 00 00 00 00 00 00` and **zero relocations at offsets 0 and 4**.
   The assembly file (`.s`) correctly shows `.4byte "?RawAlloc@FixedSizeAlloc@@MAAPAHH@Z"` — the
   split phase identified the references — but the COFF writer dropped the relocations.

### Root Cause

Jeff's `write_coff()` (xex.rs) treats ALL global symbols as COMDAT regions, including data symbols
in `.rdata`/`.data` sections. When a vtable entry in `.rdata` falls within a COMDAT region:

1. The relocation fixup phase zeroes the vtable data (correct — COFF addend convention)
2. The relocation loop's COMDAT branch claims the relocation (offset is within a COMDAT region)
3. BUT the COMDAT extraction to `.rdata$dup` silently fails (zero `.rdata$dup` sections across all
   2,223 .obj files — the extraction path runs but produces nothing usable)
4. The non-COMDAT branch skips the relocation (`in_comdat = true`)
5. **Result: relocation dropped.** The vtable entry is zeroed with no COFF relocation to fix it up.

### Fix (jeff/src/util/xex.rs)

Changed the `comdat_regions` building loop to only include code sections:

```rust
// Before:
if sect.kind == ObjSectionKind::Bss { continue; }

// After:
if sect.kind != ObjSectionKind::Code { continue; }
```

This prevents `.rdata`/`.data` symbols from being treated as COMDAT regions. Vtable relocations now
stay in the parent section and are properly written to the COFF output.

**Impact:**
- All four functions now appear in the MAP at non-zero addresses
- LNK4006 count unchanged (3,351) — data COMDAT extraction was already broken
- LNK2019 count unchanged (1,220) — these are genuinely missing implementations

### Fix (xenia/src/xenia/emulator.cc)

The CRT sanitizer at line 1618 writes to CRT table entries to nullify invalid entries. The CRT
table at `0x836BBAF8` is in the image's `.CRT` section which may be read-only. Added
`crt_heap->Protect()` to make CRT table pages writable before nullifying.

### Boot Progression

After both fixes, the game now:
1. Loads XEX and resolves imports (VdSwap, VdInitializeRingBuffer, etc.)
2. Patches 56/56 NUI functions
3. Completes CRT initialization (87 valid constructors run, 303/390 nullified)
4. Starts Main XThread
5. **Crashes at guest address 0** — null function pointer call from another unresolved vtable entry

This is a **major milestone**: the game boots past CRT init and starts executing game code.
The previous crash (FixedSizeAlloc::RawAlloc) is fully resolved.

### Files Changed

| File | Change |
|------|--------|
| `jeff/src/util/xex.rs` | Data COMDAT region exclusion: only code sections get COMDAT treatment |
| `xenia/src/xenia/emulator.cc` | CRT table page protection before nullification |

### Next Steps

| Priority | Action | Location |
|----------|--------|----------|
| HIGH | Identify which null vtable call crashes the main thread (look up guest LR in MAP) | xenia diagnostic thread |
| HIGH | Investigate remaining ~400 LNK2019 unresolved externals — many will cause null vtable crashes | dc3-decomp link |
| MEDIUM | Consider removing RODATA workaround now that vtable entries are properly linked | xenia emulator.cc |
| LOW | Feature request: jeff split validation (warn when user boundaries bisect symbols) | jeff split.rs |

## Session 9 (2026-02-22): Clean Rebuild + Address Regen + JIT Fixes

### Context

Session 8 fixed the jeff COMDAT data section bug. Session 9 performs a clean rebuild with the
fixed jeff, regenerates all hardcoded patch addresses from the new MAP file, and removes the
zero page mapping to surface null-vtable crashes more clearly.

### Work Completed

#### 1. Clean Rebuild with Fixed Jeff

Full rebuild of dc3-decomp using the COMDAT-fixed jeff. Linker error comparison:

| Error Type | Pre-fix | Post-fix | Change |
|---|---|---|---|
| LNK2019 | 404 | 404 | Same |
| LNK2001 | 818 | 818 | Same |
| LNK4006 | 3,351 | 411 | **-2,940** |
| String literals | 749 | 749 | Same |
| LNK2013 (REL14) | 17 | 3 | **-14** |

The massive LNK4006 drop confirms the COMDAT fix eliminated most duplicate symbol warnings.
String literal errors persist — a separate issue from data COMDAT extraction.

#### 2. Patch Address Regeneration

The rebuild changed all function addresses. Every hardcoded address was regenerated from the
current MAP file:

- **NUI patch table**: All 62 decomp entries updated (was 56, +6 new camera property stubs)
- **XapiCallThreadNotifyRoutines stubs**: `0x82F51108/0x82F51178` → `0x830DBBB0/0x830DBC20`
- **CRT initializer tables**: `0x836BBAF8-0x836BC120` → `0x83B0BB20-0x83B0C148`
- **XapiThreadNotifyRoutineList**: `0x8366F35C` → `0x83B14C3C`
- **XapiProcessLock**: `0x8366F340` → `0x83B14C34`

#### 3. Zero Page Mapping Removed

Removed the `AllocFixed(0, 0x10000, ...)` zero page mapping that was masking null-pointer
dereferences in guest code. The crash now surfaces immediately at the faulting `lwz` instruction
instead of silently reading zero from the mapped page.

#### 4. JIT Assertion Fixes

Two xenia JIT assertions triggered by the much larger guest code surface (364 CRT constructors
vs 70 previously):

**Arena::Alloc chunk size**: Guest functions larger than the 4MB arena chunk size caused an
assertion failure. Fixed by dynamically allocating oversized chunks when needed.

**File**: `src/xenia/base/arena.cc`

**Finalization pass label IDs**: Guest functions with >65535 basic blocks hit a label ID
assertion. The label name format was limited to 4 hex digits. Fixed by expanding to 8 hex
digits and removing the assertion (label IDs are already `uint32_t`).

**File**: `src/xenia/cpu/compiler/passes/finalization_pass.cc`

#### 5. RODATA Workaround Assessment

The RODATA workaround (`Protect 0x82000000-0x82410000` as RW) is **still needed**:
- 26 CRT constructor entries resolve to 0x82000000 (image base) via /FORCE:UNRESOLVED
- ~120 genuine LNK2019 unresolved symbols reference RODATA-range addresses
- Removing the workaround would crash on writes to these unresolved globals

### Boot Progression

| Stage | Status | Notes |
|-------|--------|-------|
| XEX load + import resolution | ✅ | 323 thunks, 501 variables |
| NUI function stubbing | ✅ | **62/62** decomp patches (up from 56) |
| XapiCallThreadNotifyRoutines | ✅ | Stubs at new addresses |
| CRT `_cinit` sanitizer | ✅ | **26/390 nullified** (down from 320!) |
| CRT constructors run | ✅ | **364** constructors execute (up from 70) |
| Main XThread starts | ✅ | VdSwap imported |
| RtlEnterCS / RtlLeaveCS | ✅ | 186 / 185 (balanced minus 1 held) |
| `RtlpCoalesceFreeBlocks` | ❌ | **Heap corruption crash** |

**Key improvement**: CRT sanitizer now only nullifies 26 entries (down from 320). The COMDAT
fix correctly resolved 294 more CRT constructors, which is a massive increase in code coverage.

### Current Blocker: Heap Corruption in CRT Init

| Detail | Value |
|--------|-------|
| Crash function | `RtlpCoalesceFreeBlocks` at 0x830DC644 (rtlheap.obj) |
| Crash PC | 0x830DC970 |
| Fault type | SIGSEGV — READ at guest address 0x00000000 |
| r30 / r31 | 0x536F7264 ("Sord") — string data in heap entry pointer |
| r25 | 0xFEEEFEEE — Windows debug fill (freed memory marker) |
| r9 | 0x536F726C ("Sorl") — more string corruption |

**Root cause**: One or more of the 364 CRT constructors is corrupting the Xbox heap metadata.
The corrupted entries contain ASCII string data ("Sord" = likely from "SortDirection" or similar),
indicating a buffer overflow or use-after-free in a static initializer that writes string data
over heap management structures.

This is the **same heap corruption pattern** observed in Session 5 (r30=0x536F7264, r25=0xFEEEFEEE),
confirming a persistent bug in the decomp's CRT initialization. The bug was masked in Session 5
by stale NUI patches corrupting unrelated code.

### Files Modified

| File | Changes | Purpose |
|------|---------|---------|
| `src/xenia/emulator.cc` | NUI patches (62 addrs), Xapi stubs (2 addrs), CRT tables (4 addrs), zero page removed | Address regen + zero page removal |
| `src/xenia/app/emulator_headless.cc` | XapiThreadNotifyRoutineList addr | Address regen |
| `src/xenia/base/arena.cc` | Dynamic oversized chunk allocation | JIT fix for large functions |
| `src/xenia/cpu/compiler/passes/finalization_pass.cc` | Label ID format expansion | JIT fix for functions with >64K blocks |

### Next Steps

| Priority | Action | Location |
|----------|--------|----------|
| HIGH | Identify which CRT constructor corrupts the heap (binary search by nullifying ranges) | xenia emulator.cc |
| HIGH | Investigate the "Sord" string pattern — find which obj produces it | dc3-decomp MAP + .obj files |
| MEDIUM | Consider adding guest heap integrity checks at CRT constructor boundaries | xenia emulator.cc |
| LOW | Automate NUI patch address extraction from MAP to avoid stale addresses | xenia build scripts |

---

## Appendix: Current Git Snapshot

- Branch: `headless-vulkan-linux`
- HEAD: `a224a6846`
- Commits ahead of upstream: 3
- Uncommitted source changes: ~20 files (see tables in session sections above)
- Dirty submodules: `cxxopts`, `date`, `fmt`, `imgui`, `premake-core`
- Game assets: `/home/free/code/milohax/dc3-decomp/orig-assets/default.xex`
