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

- Boot/runtime progress is real: game runs for long durations and scripted input fires.
- Primary blocker is still rendering correctness in deferred mode.
- Branch state is split into:
  - One large committed branch delta vs upstream (`6394d2a7f`)
  - A large uncommitted investigation layer on top (render/deadlock diagnostics + shim tweaks)
- Current approach is directionally sound, but risk is high because functional fixes and heavy diagnostics are mixed in hot paths.

## Runtime Snapshot (Latest Session Context)

From the most recent 120-second extended boot test:

- Frames remained black at sampled frames 500/1000/1500/2000 (0% non-zero pixels).
- Runtime remained stable: 2000+ swaps over 120s (~33 FPS).
- Scripted input fired (`A` and `START` keystroke events observed).
- Only 3 draws executed (around swaps 11-13) with `render_frame=false`.
- Frontbuffer pointer stayed fixed at `0x1EBC8000`.

Interpretation:

- This behavior is consistent with non-`force_all_draws` mode not scheduling RENDER+DEFER correctly for capture windows in that run.
- Existing evidence still indicates deferred draw content quality is wrong even when scheduling works (B channel dominated by `0x3F`), while inline draws have better color but deadlock around frame 12.

Primary runtime references for this snapshot (full map above):

- [`../../dc3-decomp/docs/runtime/XENIA_HEADLESS_STATUS.md`](../../dc3-decomp/docs/runtime/XENIA_HEADLESS_STATUS.md): current rendering investigation and status decisions.
- [`../../dc3-decomp/docs/runtime/BOOT_ANALYSIS.md`](../../dc3-decomp/docs/runtime/BOOT_ANALYSIS.md): boot-stage timeline and run evidence.
- [`../../dc3-decomp/docs/runtime/SCRIPTED_INPUT_TESTING.md`](../../dc3-decomp/docs/runtime/SCRIPTED_INPUT_TESTING.md): scripted input behavior and tested sequences.

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

- `HEAD`: `6394d2a7f` (`headless-vulkan-linux`)
- `upstream/master...HEAD`: `0 behind / 1 ahead`

Net size:

- `61 files changed, 3328 insertions, 177 deletions`

Largest subsystem impact (approx):

- `gpu`: 19 files, +1159/-55
- `kernel`: 20 files, +925/-94
- `app`: 4 files, +727/-0
- `hid`: 3 files, +176/-9
- `cpu`: 6 files, +165/-5

High-impact committed areas:

- New headless app binary and main loop
- Vulkan headless capture and deferred replay scaffolding
- Async pipeline cache behavior for headless
- Scripted nop input
- NUI/XAM/Xboxkrnl shims for DC3 boot
- Linux/JIT thunk ABI fixes
- Filesystem and memory mapping platform fixes
- PE override support

### B. Uncommitted working tree delta

Net size:

- `28 files changed, 961 insertions, 366 deletions`

Largest in-progress files:

- `src/xenia/gpu/vulkan/vulkan_command_processor.cc` (+471/-284)
- `src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc` (+171/-1)
- `src/xenia/hid/nop/nop_input_driver.cc` (+109/-8)
- `src/xenia/base/threading_posix.cc` (+33/-1)

Uncommitted focus areas:

- Render scheduling/replay refinement
- Readback resource pre-allocation + debug dumps
- Shared memory watch suppression integration
- Additional deadlock diagnostics (threading/wait logging/backtrace capture)
- Input keystroke edge behavior
- Extra logging in XAM/xboxkrnl paths

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
- The project has moved from zero-render visibility to meaningful boot/render instrumentation.

### Where this is still fragile ("swiss cheese" risk)

1. Rendering correctness is unresolved in the mode we need most.
- Symptom: deferred path gives wrong color content.
- Impact: blocks meaningful boot/menu validation.

2. Deadlock diagnosis code is currently mixed into production threading/signal paths.
- `src/xenia/base/threading_posix.cc:1190` captures backtraces in a signal handler.
- `backtrace()` is not async-signal-safe; this can perturb behavior.

3. Hot-path logging is very heavy in timing-sensitive systems.
- `src/xenia/gpu/vulkan/vulkan_command_processor.cc`
- `src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc`
- `src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc`
- Risk: instrumentation changes scheduler behavior and masks/reorders races.

4. Functional and diagnostic edits are not cleanly separated.
- Hard to bisect regressions.
- Hard to know what is required vs temporary.

5. A few quality issues should be cleaned as part of stabilization.
- `src/xenia/app/xenia_headless_main.cc:265` always returns `EXIT_SUCCESS` from `main`.
- `src/xenia/app/xenia_headless_main.cc:73` defines `headless_async_draws` but it is currently unused.
- `src/xenia/gpu/command_processor.cc:34` has duplicated `DECLARE_string(dump_frames_path)`.

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
  --target=/home/free/code/milohax/dc3-decomp/orig/373307D9/default.xex \
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

## Appendix: Current Git Snapshot Used For This Audit

- Branch: `headless-vulkan-linux`
- Branch commit vs upstream: `6394d2a7f`
- Committed delta: `61 files, +3328/-177`
- Uncommitted delta: `28 files, +961/-366`
- Untracked: `docs/DC3_NUI_ROADMAP.md`
- Dirty submodules: `cxxopts`, `date`, `fmt`, `imgui`, `premake-core`
