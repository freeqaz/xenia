# Goal: Boot Dance Central 3 Decomp in Xenia on Linux

## Success Criteria

1. **DC3 decomp XEX loads and reaches `main()`** — CRT static initialization completes without heap corruption
2. **DC3 decomp XEX reaches `App::Init()`** — game initialization begins, resource loading starts
3. **DC3 decomp XEX renders a frame** — Vulkan GPU backend produces visible output
4. **DC3 decomp XEX reaches menu** — game is interactive

## Repos Involved

| Repo | Path | Role |
|------|------|------|
| **xenia** | `~/code/milohax/xenia` | Xbox 360 emulator (this repo) |
| **dc3-decomp** | `~/code/milohax/dc3-decomp` | DC3 decompilation project |
| **jeff** | `~/code/milohax/jeff` | Custom dtk/COFF toolchain for dc3-decomp |

## Process: Recursive OODA Loop

Each iteration follows the same cycle:

```
OBSERVE  →  What happens when we run the test?
ORIENT   →  What does it mean? What's the root cause?
DECIDE   →  What's the highest-leverage fix?
ACT      →  Implement the fix, rebuild, test again
```

### How to iterate

1. Read `STATUS.md` for current observations and blockers
2. Read `TODO.md` for current actionable tasks
3. Execute the highest-priority TODO items
4. Rebuild (dc3-decomp and/or xenia as needed)
5. Run the boot test
6. Update `STATUS.md` with new observations
7. Update `TODO.md` — check off completed items, add new ones
8. Repeat from step 1

## Historical Architecture Baseline (2026-02-23)

Historical snapshot note:
- This section captures the 2026-02-23 baseline and is useful for context.
- For the **current active blocker**, use `STATUS.md` + `CONTINUATION_PLAN.md` first.
- Recommended working set:
  - `docs/dc3-boot/CONTINUATION_PLAN.md`
  - `docs/dc3-boot/STATUS.md`
  - `docs/dc3-boot/DEBUGGING_TIPS.md`
  - `docs/dc3-boot/ARCHIVED.md` (historical context only)

## Current Active Blocker (2026-02-25)

- The decomp boot is currently blocked in CRT C++ constructor iteration (`_cinit` `__xc` loop), before `main()`.
- `Symbol::PreInit` is injected via a PPC trampoline at `__xc[0]` and does execute, but it produces `gStringTable=0x14` (garbage) instead of a valid `StringTable*`.
- This causes subsequent `Symbol::Symbol` calls to pass an invalid `this` into `StringTable::Add`, leading to crash/loop behavior.
- The highest-leverage next work is therefore:
  1. disassemble and trace `Symbol::PreInit`,
  2. identify allocator/internal call failure (or argument/timing issue),
  3. fix `gStringTable` initialization so CRT `__xc` completes and `main()` is reached.

Critical debugging constraint (must respect):
- Guest override "intercept then delegate to same address" is not reliable once JIT call sites are compiled, because the x64 JIT bakes the extern handler pointer into generated host code.
- Prefer:
  - pre-JIT PPC injection,
  - full host implementation override,
  - forwarding to a different address,
  - or host-side guest memory construction.

Historical 2026-02-23 blocker details were moved to `docs/dc3-boot/ARCHIVED.md`.

### DC3 NUI/XBC path is now cut over

- DC3 NUI/XBC patch targeting is no longer raw-address only.
- Xenia now uses a resolver pipeline for these targets:
  - patch manifest (`xenia_dc3_patch_manifest.json`)
  - symbol manifest (`symbols.txt`-style)
  - signature resolver
  - catalog fallback (`hybrid` mode only)
- Eligible simple stubs are applied as **guest extern overrides** (no guest byte patching).
- Legacy DC3 NUI/XBC byte-patch fallback has been removed after validation.

### Why this matters for decomp work (not just emulator cleanup)

1. **Relink/address drift stops breaking runtime bring-up**
   - Decomp layouts change often; resolver+signature matching absorbs that churn.
   - This reduces time spent refreshing patch addresses in `emulator.cc`.
2. **Xenia becomes a stable validation harness for decomp progress**
   - We can compare boot/init behavior across original vs decomp builds even as addresses move.
   - This makes runtime regression testing practical while `dc3-decomp` and `jeff` are still changing.
3. **Faster decomp iteration**
   - Fewer emergency emulator edits means more time on missing constructors, splits, COMDATs, and link fixes.
4. **Safer experimentation**
   - Resolver behavior is tested and fail-closed, which reduces false positives from brittle patch matching.

### Validated NUI/XBC cutover outcomes

- Default (`hybrid`) path:
  - original XEX: `patched=0 overridden=59`
  - decomp XEX: `patched=0 overridden=85`
- Strict signatures-only path (no manifest/symbols):
  - original XEX: `59/59` resolved via signatures (`strict_rejects=0`)
  - decomp XEX: `85/85` resolved via signatures (`strict_rejects=0`)
- Validation tooling:
  - `tools/dc3_nui_cutover_gate.sh`
  - `xenia-core-tests "[dc3_nui_patch_resolver]"`

### Build Commands

```bash
# DC3-Decomp rebuild
cd ~/code/milohax/dc3-decomp && ninja link 2>&1 | tee /tmp/link.txt && python3 scripts/build/build_xex.py

# Xenia rebuild (preferred workflow in this repo)
cd ~/code/milohax/xenia && ./xb build --config=debug --target=xenia-headless --no_premake -j $(nproc)

# Test
rm -f /dev/shm/xenia_* 2>/dev/null
~/code/milohax/xenia/build/bin/Linux/Debug/xenia-headless \
    --gpu=null --target=~/code/milohax/dc3-decomp/build/373307D9/default.xex \
    --headless_timeout_ms=20000 2>&1 | tee /tmp/test.log
```

## Roadmap (Next Milestones)

### 1. Extract remaining DC3 hacks into a title hack pack (Xenia)

- Move non-NUI DC3-specific logic out of `src/xenia/emulator.cc`:
  - CRT sanitizer/workarounds
  - skeleton/debug/assertion loop workarounds
  - decomp runtime stopgaps (zero-page/RODATA/etc.)
- Why:
  - keeps `emulator.cc` maintainable
  - makes each workaround explicit, testable, and easier to retire

### 2. Build a decomp runtime parity gate (original vs decomp)

- Add scripted comparisons for boot progression/milestones and key telemetry.
- Track differences in:
  - repeated no-op stub calls
  - unresolved function patterns
  - override hit counts
  - loop hotspots / recurring PCs
- Why:
  - gives a concrete signal for whether a `dc3-decomp` / `jeff` change improved runtime behavior

Current status:
- Implemented `tools/dc3_runtime_parity_gate.sh`
- Validated on 2026-02-23 in both `hybrid` and `strict`
- Supports explicit per-run manifest/symbol overrides (`DC3_{ORIG,DECOMP}_{MANIFEST_PATH,SYMBOL_MAP_PATH}`)
- Adds manifest/XEX preflight integrity checks (schema/targets/build-label + stale manifest warnings)
- Hard checks currently enforce NUI/XBC override counts + strict signature coverage
- Higher-level parity diffs now include symbolized function-grouped summaries
  - `tools/dc3_runtime_telemetry_diff.py` provides symbolized caller/target rankings and a "top divergent functions" summary
- Parity gate can emit one-pass investigation artifacts
  - `DC3_PARITY_SYMBOLIZE=1` emits symbolized telemetry diff + crash disasm artifacts
  - `DC3_PARITY_TRIAGE=1` emits crash signature triage artifacts (`orig` / `decomp`)
- Milestone comparison is now explicit in parity output
  - milestone contract verdict (`PASS/WARN/FAIL`) with policy control (`DC3_PARITY_MILESTONE_POLICY`)
  - CRT-vs-milestone triage summary to support constructor-impact prioritization

Related debugging helpers:
- `tools/dc3_guest_disasm.py` (symbolized PPC disasm around `PC/LR/CTR`, supports `--xenia-log`)
- `tools/dc3_trace_on_break.sh` (headless `--break_on_instruction` workflow wrapper)
- `tools/dc3_crash_signature_triage.py` (log-based crash signature auto-labeling)
- `tools/dc3_gdb_rsp_mvp_mock.py` (Phase 4 protocol groundwork: crash-snapshot-backed GDB RSP mock)
- `tools/dc3_gdb_rsp_snapshot_bridge.sh` (captures/uses a crash log and launches the RSP mock with a ready-to-run GDB attach command)
- `--dc3_crash_snapshot_path=/path/to/crash_snapshot.json` (Xenia structured crash snapshot artifact; future-proof input for Phase 4 tools)
- `--dc3_gdb_rsp_stub=true` + `--dc3_gdb_rsp_port=<port>` (Linux `xenia-headless` in-process Phase 4 MVP RSP listener for live guest state)

Runbook note:
- `docs/dc3-boot/DEBUGGING_TIPS.md` now includes a "Tooling Runbook (Post-Debugging Leverage Loop)" section documenting the recommended escalation order:
  parity gate -> crash disasm/triage -> trace-on-break -> RSP (snapshot/live).
- `docs/dc3-boot/CONTINUATION_PLAN.md` contains the current blocker-focused execution plan (CRT / `gStringTable` / `Symbol::PreInit`) and verified tool commands/cvars.
- DTB/config parsing investigations should start non-invasively; the `ReadCacheStream` step probe is now opt-in via `--dc3_debug_read_cache_stream_step_override=true` because it can perturb checksum/parser behavior.

### 3. Promote manifest as the canonical Xenia input from `dc3-decomp`

- Keep `.map` for humans/debugging.
- Use `xenia_dc3_patch_manifest.json` as the machine-readable contract.
- Include:
  - semantic target addresses
  - CRT sentinels
  - build label
  - static artifact hash
  - runtime-loaded `.text` hash (when known)
- Why:
  - deterministic Xenia integration
  - less linker-format parsing fragility

### 4. Hack retirement matrix (Xenia + decomp linked)

- Track each Xenia workaround with:
  - reason
  - current resolver/patch mechanism
  - current necessity
  - retirement condition in `dc3-decomp` / `jeff`
- Why:
  - prevents temporary compatibility hacks from becoming permanent
  - directly ties emulator changes to decomp deliverables

Current status:
- Initial matrix scaffold added: `docs/dc3-boot/HACK_RETIREMENT_MATRIX.md`
- Next step is to split grouped decomp/debug stubs into individual entries during hack-pack extraction

## New Ideas From Recent Work

1. **Use Xenia as a differential tracer**
   - Export structured runtime events (JSONL) for unresolved calls, no-op stub hits, guest overrides, and hot loop PCs.
   - Compare original vs decomp traces to prioritize decomp fixes.
2. **Generate a unified runtime support manifest from `dc3-decomp`**
   - Extend the patch manifest with runtime support metadata (CRT sentinels, shims, build IDs).
   - Lets Xenia configure title-specific compatibility behavior deterministically from one file.
3. **Constructor impact triage**
   - Rank CRT constructors by effect on boot progression using sanitizer skips + runtime telemetry.
   - Use that ranking to prioritize `??__E` export/fix work in `dc3-decomp` / `jeff`.
4. **Keep `strict` mode as a regression target**
   - `hybrid` remains the best daily workflow.
   - `strict` should be run regularly to preserve resolver portability and catch regressions early.

### Concurrency Strategy

These repos have independent build pipelines. Many analysis tasks are pure reads.
When working across repos, launch parallel subagents:

- **xenia agents**: Instrument emulator, fix runtime crashes, update patch addresses
- **dc3-decomp agents**: Analyze MAP/symbols, fix splits, stub missing functions
- **jeff agents**: Fix COFF generation bugs, improve symbol handling

Agents that only read files can always run in parallel. Agents that write files
within the same repo must be sequenced.

## Key Files

### Xenia
| File | Purpose |
|------|---------|
| `src/xenia/emulator.cc` | DC3 patches: NUI stubs, CRT sanitizer, RODATA workaround |
| `src/xenia/app/emulator_headless.cc` | Headless main loop, diagnostic thread |
| `src/xenia/base/arena.cc` | JIT arena allocator (oversized chunk fix) |
| `src/xenia/cpu/compiler/passes/finalization_pass.cc` | JIT label ID fix |
| `docs/DC3_HEADLESS_CHANGE_AUDIT_2026-02-20.md` | Full session audit trail |

### DC3-Decomp
| File | Purpose |
|------|---------|
| `build/373307D9/default.map` | Address lookups for all symbols |
| `build/373307D9/default.xex` | Decomp XEX under test |
| `config/373307D9/splits.txt` | Symbol split boundaries |
| `config/373307D9/objects.json` | Object matching status |

### Jeff
| File | Purpose |
|------|---------|
| `src/util/xex.rs` | COFF generation (BSS fix, COMDAT fix applied) |
| `src/util/split.rs` | CRT initializer co-location |
