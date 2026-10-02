# milohax Xenia fork: docs index

This repo is a fork of [xenia-project/xenia](https://github.com/xenia-project/xenia)
that runs Dance Central 3 (`373307D9`) and Rock Band 3 (`45410914`, incl. RB3DX)
headless on Linux. Everything under `docs/` that is **not** listed here is
upstream's (`building.md`, `cpu.md`, `gpu.md`, `kernel.md`, ...).

**Ground rules for this fork:** keep the diff against upstream small, put
title-specific code under `src/xenia/titles/`, default every mitigation cvar to
upstream behaviour and opt in per title or per run, and check every change with
the regression harness before landing it.

Two of those rules are enforced by the harness's static scenario (S0). A title
ID (`373307D9`, `45410914`, or `kTitleDc3`/`kTitleRb3`) outside
`src/xenia/titles/` fails S0 unless
[`tools/fork-regress/scenarios/S0.title-id-allowlist`](../../tools/fork-regress/scenarios/S0.title-id-allowlist)
lists it with a reason. S0 also lists every compiled-in cvar default that
differs from the baseline.

## Start here

| Doc | What it is |
|---|---|
| [cleanup/FORK_CLEANUP_PLAN.md](cleanup/FORK_CLEANUP_PLAN.md) | **Current plan.** Classification of every fork change, target layout, lanes. |
| [`tools/fork-regress/README.md`](../../tools/fork-regress/README.md) | The regression harness (scenarios S0-S6, baselines, load gate). Every lane runs it. |
| [cleanup/DC3_HACK_GAP_ANALYSIS.md](cleanup/DC3_HACK_GAP_ANALYSIS.md) | What each DC3 hack masks, and the real Xenia fix that would replace it. |

## Build and run

```bash
./xb premake
make -C build xenia-headless config=checked_linux -j12   # headless runner
make -C build xenia-app config=checked_linux -j12        # windowed app (binary: xenia)
```

Both apps link the same `xenia-core` and `xenia-kernel`. There is no separate
headless compile, and no `XE_HEADLESS_BUILD` define. Headless behaviour is
decided at runtime, in two ways:

- `display_window_ == nullptr` in `emulator.cc`, which also feeds
  `TitleLaunchContext::headless`;
- `cvars::headless` in the XAM dialogs. `xenia-headless` forces `--headless`
  as a command-line value, so a config file cannot turn it off.

Only `xenia-headless` links the title modules (`xenia-titles*`). To confirm,
run `nm -C build/bin/Linux/Checked/xenia | grep -E 'Dc3|Rb3'`. It shows only
the per-title cvar profile in `titles/title_profile.cc`, which is core.

Run scenarios through the harness rather than by hand
(`flock /home/free/tmp/fork-regress.lock tools/fork-regress/run.sh <bin> <out> --scenarios S1`).
A by-hand run must pass a private `--storage_root`, or Xenia rewrites the shared
`~/.local/share/Xenia/xenia.config.toml`.

## By area

### Cleanup (`cleanup/`)

| Doc | |
|---|---|
| [FORK_CLEANUP_PLAN.md](cleanup/FORK_CLEANUP_PLAN.md) | The plan (2026-10). |
| [DC3_HACK_GAP_ANALYSIS.md](cleanup/DC3_HACK_GAP_ANALYSIS.md) | DC3 hack -> masked Xenia gap -> real fix. |
| [fork-cleanup-review.md](cleanup/fork-cleanup-review.md) | 2026-08-25 review of `frag-alloc-trace`. Superseded as a worklist; source comments still cite its finding IDs (C1, C4, C16, ...). |

### Core: cpu, kernel, profile (`core/`)

| Doc | |
|---|---|
| [core/TITLE_PROFILE.md](core/TITLE_PROFILE.md) | The per-title cvar profile (`titles/title_profile.cc`): when it applies, precedence, and the evidence for each DC3/RB3 mitigation it sets. |
| [core/GUEST_EXCEPTIONS.md](core/GUEST_EXCEPTIONS.md) | Guest SEH / C++ exception dispatch: today's `RtlRaiseException`/`RtlUnwind` behaviour and the design for real dispatch (gap G13). Designed, not implemented. |

### DC3 (`dc3/`)

| Doc | |
|---|---|
| [dc3/BASELINE.md](dc3/BASELINE.md) | DC3 original `debug.xex` on Xenia: measured baseline, re-measured by Lane B (2026-10-02) -- the FAILs the old hacks hid, S1 results. |
| [dc3/PATCH_MANIFEST.md](dc3/PATCH_MANIFEST.md) | Every DC3 original-layout hack by `--dc3_disable_hacks` id: what it changes, what it masks, retired or kept. |
| [dc3/SPIKE_LOG.md](dc3/SPIKE_LOG.md) | The DTA-evaluation-channel spike log (the oracle). |
| [`dc3/`](dc3/) `run_dc3_oracle.sh`, `analyze_run.py`, `xenia.dc3-oracle.defaults.toml` | The oracle run script, analyzer and all-defaults toml (the harness has its own copies in `tools/fork-regress/`). |
| [dc3/dc3_nui_fingerprints.txt](dc3/dc3_nui_fingerprints.txt) | NUI resolver fingerprint cache (data). Nothing auto-probes it any more; pass `--dc3_nui_layout_fingerprint_cache_path` explicitly. |
| [decomp_layout_hack_pack.md](decomp_layout_hack_pack.md) | Rebuild workflow for the kept decomp-layout hack pack (`src/xenia/titles/dc3/decomp/`), salvaged from the old `CLAUDE.md`. |

### RB3 (`rb3/`)

| Doc | |
|---|---|
| [rb3/jit-fault-wiki/WORKSTREAM-rb3-on-xenia-bringup.md](rb3/jit-fault-wiki/WORKSTREAM-rb3-on-xenia-bringup.md) | **Canonical** RB3 / RB3DX bring-up narrative (boot -> title -> menus -> gameplay, §8c/§8v-§8x). |
| [rb3/jit-fault-wiki/INDEX.md](rb3/jit-fault-wiki/INDEX.md) | Wiki index: the `guest-membase + 0x100000000` fault investigation, pages 00-07. |
| [rb3/jit-fault-wiki/00-source-map.md](rb3/jit-fault-wiki/00-source-map.md) ... [07-fix-and-verification.md](rb3/jit-fault-wiki/07-fix-and-verification.md) | Source map, evidence (01 carries a stale banner), address translation, guest code, upstream/canary, divergence, root cause, fix. |
| [BRIEF-main-hub-load-stall.md](rb3/jit-fault-wiki/BRIEF-main-hub-load-stall.md), [CRASH-REPORT-main-hub-oom.md](rb3/jit-fault-wiki/CRASH-REPORT-main-hub-oom.md), [PLAN-splash-confirm-gate.md](rb3/jit-fault-wiki/PLAN-splash-confirm-gate.md) | RB3DX main_hub / splash investigations. |
| [`tools/rb3/`](../../tools/rb3/README.md) | Dirty-disc / same-instrument patchers and scripted-input sequences (from `rb3-verify/`). |

### GPU / headless capture (`gpu/`)

| Doc | |
|---|---|
| [gpu/headless_render_pipeline_architecture.md](gpu/headless_render_pipeline_architecture.md) | Where in the Xenos pipeline a headless capture should read the frame (and why deferred replay reads stale memory). |
| [gpu/headless_render_stabilization.md](gpu/headless_render_stabilization.md) | The alternating-3D-resolve investigation behind `headless_persist_render_state` / `headless_inline_render`. |

Headless capture cvars (all inert with a presenter; see the banner in both docs):
`--dump_frames_path`, `--headless_capture_interval`, and the opt-ins
`--headless_capture_only_draws`, `--headless_skip_submission_wait`,
`--headless_async_pipelines` (all default false = upstream behaviour).

### Debugging (`debug/`)

| Doc | |
|---|---|
| [debug/gdb_debugging.md](debug/gdb_debugging.md) | Guest PowerPC debugging through the headless GDB-RSP server; [dc3.gdbinit](debug/dc3.gdbinit), [dc3_rsp_client.py](debug/dc3_rsp_client.py). |
| [debug/DEBUGGING_TIPS.md](debug/DEBUGGING_TIPS.md) | General guest-debugging techniques (partly historical, Feb-2026). |

### Archive (`archive/`)

Historical, banner-marked, not current guidance:
[dc3-boot-2026-02/](archive/dc3-boot-2026-02/) (the Feb-2026 decomp-layout boot
campaign: STATUS, TODO, GOAL, CONTINUATION_PLAN, ARCHIVED, HACK_RETIREMENT_MATRIX,
agent_a..e), [DC3_HEADLESS_CHANGE_AUDIT_2026-02-20.md](archive/DC3_HEADLESS_CHANGE_AUDIT_2026-02-20.md),
[DC3_NUI_ROADMAP.md](archive/DC3_NUI_ROADMAP.md), and RB3:
[08-boot-to-menu.md](archive/rb3/08-boot-to-menu.md), [09-rb3dx-title-to-menu.md](archive/rb3/09-rb3dx-title-to-menu.md)
(superseded by WORKSTREAM §8c/§8v-§8x), [rb3-bringup-notes.md](archive/rb3/rb3-bringup-notes.md),
[rb3-same-instrument-verify.md](archive/rb3/rb3-same-instrument-verify.md).
Archived tools: [`tools/archive/`](../../tools/archive/README.md).

## Moved paths (2026-10-02)

Source comments in other lanes' files still cite some old paths; this is the map.

| Old path | New path |
|---|---|
| `docs/fork-cleanup-review.md` | `docs/fork/cleanup/fork-cleanup-review.md` |
| `docs/jit-fault-wiki/*` | `docs/fork/rb3/jit-fault-wiki/*` (08, 09 -> `docs/fork/archive/rb3/`) |
| `docs/dc3-boot/{STATUS,TODO,GOAL,...}.md` | `docs/fork/archive/dc3-boot-2026-02/` |
| `docs/dc3-boot/DEBUGGING_TIPS.md` | `docs/fork/debug/DEBUGGING_TIPS.md` |
| `docs/dc3_gdb_debugging.md`, `docs/dc3.gdbinit`, `docs/dc3_rsp_client.py` | `docs/fork/debug/gdb_debugging.md`, `.../dc3.gdbinit`, `.../dc3_rsp_client.py` |
| `docs/dc3_render_pipeline_architecture.md`, `docs/dc3_render_stabilization.md` | `docs/fork/gpu/headless_render_*.md` |
| `docs/DC3_HEADLESS_CHANGE_AUDIT_2026-02-20.md`, `docs/DC3_NUI_ROADMAP.md`, `docs/rb3-*.md` | `docs/fork/archive/` |
| `rb3-verify/patch/`, `rb3-verify/scripts/` | `tools/rb3/patch/`, `tools/rb3/input/` |
| `tools/{dc3_nui_cutover_gate.sh,dc3_crt_bisect.sh,dc3_extract_addresses.py,analyze_poolalloc.py,dc3_gdb_rsp_snapshot_bridge.sh}` | `tools/archive/` |
| `docs/dc3-boot/agent_e_extracted_addresses.txt` | deleted (stale generated header) |
| `docs/dc3-oracle/{BASELINE.md,SPIKE_LOG.md,run_dc3_oracle.sh,analyze_run.py,xenia.dc3-oracle.defaults.toml}` | `docs/fork/dc3/` (BASELINE.md rewritten) |
| `docs/dc3-boot/dc3_nui_fingerprints.txt` | `docs/fork/dc3/dc3_nui_fingerprints.txt` (no longer auto-probed) |

Cited in source but not in this repo: `docs/W5_X8CE_CALL_EFFECTS.md` and
`docs/research/02-xenia-capture.md` (`cpu_flags.cc`, `ppc_translator.cc`,
`milo_trace.h`) are in the sibling `milo-trace` repo
(`../milo-trace/docs/`).
