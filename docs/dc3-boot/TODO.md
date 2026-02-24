# TODO: DC3 Boot Progression

*Last updated: 2026-02-24 (Iteration 3+, tooling workflow upgrades for parity/triage)*

Items are ordered by priority. Check off as completed. Add new items each iteration.

---

## Iteration 1: Find and Fix Heap Corruption — COMPLETE ✓

- [x] **Rebuild xenia-headless** ✓ Session 10
- [x] **Run bisect** ✓ Session 10 — Constructor #69 identified (0x82A9C138)
- [x] **Implement `--dc3_crt_skip_indices`** ✓ Session 10
- [x] **Eliminate heap corruption** ✓ Session 10
- [x] **Fix null vtable crash** ✓ Sessions 12-13 — resolved via guard page + JIT fixes
- [x] **Fix JIT SIGSEGV at XAM COM vtable (0x40002830)** ✓ Sessions 12-13
  - CallIndirect bounds check + resolve thunk fallback
- [x] **Fix volatile register clobber in JIT slow path** ✓ Sessions 12-13
  - Use resolve function thunk instead of direct C call
- [x] **Add no-op return stub for unresolvable functions** ✓ Sessions 12-13
- [x] **Fix ResolveFunction null target assertion** ✓ Sessions 12-13
- [x] **Fix Linux SysV ABI register ordering in CallIndirect** ✓ Sessions 12-13

## Iteration 2: Break NUI Callback Loop + Advance Past Init — COMPLETE ✓

- [x] **Cut over DC3 NUI/XBC stubs to resolver + guest overrides** [xenia] ✓ Session 11
- [x] **Break NUI callback loop** ✓ Sessions 14-16 (JIT crash cascade + dcbf no-op + soft faults)
- [x] **Investigate Thunk[0x83A00964] all zeros** — resolved, not a runtime blocker
- [x] **Rebuild dc3-decomp for consistent MAP+XEX** ✓ Session 16
- [x] **SIGBUS handler** ✓ Session 15 (exception_handler_posix.cc)
- [x] **Soft fault handler for unmapped guest memory** ✓ Session 15 (mmio_handler.cc)
- [x] **Guard page pre-mapping** ✓ Session 15 (dc3_hack_pack.cc — GPU writeback + top-of-address)
- [x] **dcbf/dcbst → no-op in JIT** ✓ Session 16 (x64_seq_memory.cc — eliminated 1.4M faults)
- [x] **clflush/prefetch fault handler** ✓ Session 16 (mmio_handler.cc — safety net)

## Iteration 3: First Frame — Get VdSwap Called

### Tier 0: CRITICAL PATH — Trace the Present/Swap failure

- [ ] **Find D3DDevice_Present/Swap in MAP and check if called** [analysis]
  - VdSwap is imported but never called during 40s run
  - Need to find the D3D present path: D3DDevice_Present → ... → VdSwap
  - Identify which function in the chain is missing or failing

- [ ] **Investigate frequent `guest 0x00000000` execution** [analysis]
  - Thread 6 JIT IP samples show frequent execution at address 0
  - These are null function pointer calls (stubs)
  - If a critical D3D function resolves to null, that breaks the present pipeline

- [ ] **Check D3D device initialization return values** [analysis]
  - D3DDevice_Create and setup functions may return stub values (0/-1)
  - Game may silently fail to create device, render targets, or swap chain
  - Look for D3DDevice_Create* in MAP and check their implementations

### Tier 1: Fix the Present path

- [ ] **Implement or stub missing D3D present functions** [xenia/dc3-decomp]
  - Once specific blocker identified, fix in the appropriate layer
  - Stub to return success in Xenia, or add to dc3-decomp if game-side

- [ ] **Verify first VdSwap call** [test]
  - Run with NullGPU IssueSwap logging
  - Confirm frame dimensions and frontbuffer pointer

### Tier 2: Activate idle threads + game progression

- [ ] **Investigate why Threads 1-5 never start** [analysis]
  - All 5 created with LR=0, SP at initial stack top
  - Likely worker threads waiting for signal from main thread
  - May activate once rendering pipeline completes

- [ ] **Check game state progression after first frame** [test]
  - Does game advance to loading screen?
  - Monitor for new function calls / thread activations

### Tier 1.5: Runtime Parity / Validation Infrastructure (high leverage)

- [x] **Add original-vs-decomp runtime parity gate** [xenia + test] ✓ Session 13
  - Scripted milestone/log comparison beyond NUI cutover gate
  - Track boot progression, recurring loop PCs, unresolved/no-op call patterns, and override hit counts
  - Implemented `tools/dc3_runtime_parity_gate.sh` (`hybrid` + `strict`)
  - Added explicit per-run manifest/symbol override env vars for clean-worktree parity validation
  - Goal: a stable regression signal for `dc3-decomp` / `jeff` changes

- [x] **Emit structured runtime telemetry (JSONL) for DC3 bring-up** [xenia] ✓ Session 13
  - Guest override hits
  - no-op stub hits / unresolvable calls
  - hot loop PCs / repeated callsites
  - Implemented cvar-gated JSONL telemetry (`dc3_runtime_telemetry_*`)
  - Use for original vs decomp diffing and prioritization

- [x] **Add telemetry diff ranking tool (JSONL -> blockers)** [xenia] ✓ Session 15
  - Implemented `tools/dc3_runtime_telemetry_diff.py`
  - Ranks decomp-vs-original deltas for override hits, unresolved stubs, and hot-loop PCs
  - Tolerates partial/crashed runs (missing `dc3_summary`) and still emits useful diffs

- [x] **Add guest crash/disasm helper (symbolized `PC/LR/CTR`)** [xenia] ✓ Session 21
  - Implemented `tools/dc3_guest_disasm.py`
  - Supports `--xenia-log` crash tuple extraction and XEX/PE/raw image inputs
  - Used by parity/trace tooling for postmortem artifacts

- [x] **Auto-triage common DC3 crash signatures** [xenia] ✓ Session 21
  - Implemented `tools/dc3_crash_signature_triage.py`
  - Labels invalid-SP / stack-underflow-prologue / trap-loop patterns from headless logs
  - Wired into parity gate artifact output (`orig` + `decomp`)

- [x] **Trace-on-break headless workflow wrapper** [xenia] ✓ Session 21
  - Implemented `tools/dc3_trace_on_break.sh`
  - Standardizes `--break_on_instruction` runs with telemetry + postmortem disasm outputs

- [x] **Milestone contract verdict + CRT impact triage in parity gate** [xenia] ✓ Session 21
  - Parity gate now prints milestone `PASS/WARN/FAIL` verdict (configurable policy)
  - Adds CRT-vs-milestone comparison summary to support constructor-impact prioritization

- [~] **GDB RSP Phase A protocol groundwork** [xenia] (in progress, Session 21)
  - Added `tools/dc3_gdb_rsp_mvp_mock.py` (crash-snapshot-backed mock server)
  - Added `tools/dc3_gdb_rsp_snapshot_bridge.sh` (one-command snapshot->RSP mock->GDB attach workflow)
  - Added Xenia-side structured crash snapshot output (`--dc3_crash_snapshot_path`) for tool-friendly postmortem inputs
  - Added Linux `xenia-headless` in-process RSP MVP listener (`--dc3_gdb_rsp_stub`) with Phase-A packet subset wired to `cpu::Processor` debugger APIs (Session 22)
  - Live smoke validated (Session 22): real `powerpc-none-eabi-gdb` attach + register snapshot fallback + guest memory read + detach on in-process headless stub
  - Current limit: headless build has no stack walker, so live pause/step/software breakpoints are disabled in fallback mode
  - Next: validate stack-walker-enabled `Pause`/`c`/`s`/`Z0` flow in a build/config with debugger pause support, then move the packet engine out of headless-only code into reusable Xenia-side guest-stub plumbing

- [ ] **Harden relink-sensitive DC3 stopgaps / probes** [xenia]
  - Replace or resolver-back the remaining hardcoded global addresses in `dc3_hack_pack.cc` where practical (example: `gConditional` sentinel currently requires fresh `default.map` refresh after relinks)
  - Keep invasive `ReadCacheStream` step override off by default (done) and split it into safer sub-modes if deeper DTB debugging is needed (`safe state log` vs `extra ReadImpl/Seek` perturbing probe)
  - Current restored RCS probe uses invasive `BufStream::ReadImpl/SeekImpl` overrides; investigate a lower-perturbation DTB probe path that preserves guest checksum-validator updates
  - Document any future relink-sensitive globals in `DEBUGGING_TIPS.md` with fresh-map lookup steps
  - Restore patch-manifest CRT sentinel freshness/validation so the CRT sanitizer can trust manifest values again (current recovery path uses map-synced constants when manifest is stale)
  - Replace temporary hardcoded MemMgr assert bypass addresses with manifest/symbol-backed resolution if the bypass remains needed for debugging
  - Add a true call-through `FindArray` logging path (current `log_only` mode intentionally leaves original behavior active without per-call logs)

### Tier 2: Reduce unresolved symbols (high leverage)

- [x] **Categorize all unresolved symbols** [dc3-decomp, Agent D] ✓ Session 10
  - 867 unique: 334 non-string + 533 string literals
  - P0: 26 CRT constructors (11 complex, 15 ChunkID ints)
  - P1: 195 `lbl_` labels, P2: 48 SEH handlers, P3: 19 merged, P4: 18 third-party
  - See `docs/dc3-boot/agent_d_symbol_categories.md`

- [x] **Analyze string literal COMDAT failure** [jeff, Agent C] ✓ Session 10
  - Root cause: jeff commit `cf01a80` + compiler hash mismatch
  - Fix: hash normalization in xex.rs (replace all `??_C@` hashes with `A`)
  - See `docs/dc3-boot/agent_c_comdat_analysis.md`

- [ ] **Fix COMDAT string extraction in jeff** [jeff]
  - Implement hash normalization fix identified by Agent C
  - Re-enable `.rdata` COMDAT extraction with vtable relocation protection
  - Target: resolve 533 `??_C@` string literal symbols

- [ ] **Fix 26 missing CRT constructor exports** [dc3-decomp/jeff]
  - 11 complex: TheMemcardMgr, ThePresenceMgr, TheVirtualKeyboard, gThreadAchievements,
    FilePath::sRoot, FilePath::sNull, gDataPointMgr, gWavMgr, gChecksumData, gEntries, gOverride
  - 15 simple: kListChunkID, kRiffChunkID, kMidiChunkID, etc.
  - Fix: export `??__E` symbols from asm .obj OR implement C++ initializers in decomp source
  - All have known correct MAP addresses (0x82A2E81C-0x82EE9D30)

- [ ] **Stub third-party library functions** [dc3-decomp]
  - libjpeg, curl, Ogg Vorbis, zlib -- ~18 missing symbols
  - Create stub .c files with no-op implementations
  - Add to build system

- [ ] **Fix lbl_ symbol exports** [dc3-decomp]
  - 195 `lbl_` assembly labels unresolved (clustered at 0x82F6xxxx, 0x830Exxxx)
  - Analyze: which are code vs data references
  - Fix splits.txt boundaries or add symbol exports where needed

### Tier 3: Stabilize xenia patches (quality of life)

- [x] **Automate MAP address extraction** [xenia, Agent E] ✓ Session 10
  - `tools/dc3_extract_addresses.py` validated: 70/70 symbols match
  - Generates C++ header with `constexpr` addresses
  - Production-ready, no fixes needed

- [ ] **Make CRT sanitizer threshold dynamic** [xenia]
  - Currently hardcoded kCodeStart = 0x822C0000
  - Should derive from PE section headers at runtime

- [x] **Commit and push xenia working tree** [xenia] ✓ Session 13
  - Merged DC3 headless debugging + NUI/XBC cutover work
  - Branch `headless-vulkan-linux` pushed to `origin`

- [~] **Extract non-NUI DC3 hacks into a title hack pack/module** [xenia] (in progress)
  - Extracted (stable): CRT/imports/debug/decomp stopgaps into `src/xenia/dc3_hack_pack.{h,cc}`
  - Completed second-pass extraction: `fake_kinect_data` skeleton injection + SkeletonUpdate binary patches now route via `ApplyDc3SkeletonHackPack(...)`
  - Keep NUI/XBC resolver path as the reference architecture
  - Goal: clearer ownership and easier hack retirement

- [x] **Create a hack retirement matrix (Xenia ↔ dc3-decomp/jeff)** [docs + xenia + decomp] ✓ Session 13 (initial scaffold)
  - For each workaround: reason, scope, current necessity, retirement trigger, owner
  - Initial matrix: `docs/dc3-boot/HACK_RETIREMENT_MATRIX.md`
  - Use this to prioritize decomp fixes that delete emulator hacks

- [~] **Promote patch manifest as canonical machine-readable contract** [dc3-decomp + xenia] (in progress)
  - Done: schema/version fields + `build_identity` block in emitter; parser schema/version validation + safe fallback on invalid manifests
  - Done: Xenia disables manifest target addresses on manifest/runtime fingerprint mismatch
  - Remaining: detect stale `.map` vs `.xex` pair mismatch (manifest may be schema-valid but semantically wrong)
  - `.map` remains for human investigation

### Tier 4: Post-boot progression (after CRT init works)

- [ ] **Handle CharacterProvider::Text() null vtable crash** [xenia/dc3-decomp]
  - Session 8 blocker, likely to recur after CRT init is fixed
  - Fix: construct missing global (TheGameData, etc.) or stub the call chain

- [ ] **Assess how far past main() the game gets** [test + observe]
  - App::Init, SystemConfig loading, resource loading
  - Each stage will likely hit new unresolved symbols
  - Iterative: fix crash, run, observe next crash, fix

---

## Concurrency Notes

### Wave 1 — COMPLETE ✓
| Agent | Repo | Task | Status |
|-------|------|------|--------|
| A | xenia | CRT bisect script | ✓ `tools/dc3_crt_bisect.sh` |
| B | dc3-decomp | "Sord" trace | ✓ `docs/dc3-boot/agent_b_sord_trace.md` |
| C | jeff | COMDAT string analysis | ✓ `docs/dc3-boot/agent_c_comdat_analysis.md` |
| D | dc3-decomp | Symbol categorization | ✓ `docs/dc3-boot/agent_d_symbol_categories.md` |
| E | xenia | MAP extractor validation | ✓ `docs/dc3-boot/agent_e_validation.md` |

### Sequential Gate — COMPLETE ✓
- ~~Merge Agent A's bisect script~~ ✓
- ~~Rebuild xenia-headless~~ ✓
- ~~Run bisect~~ ✓ Constructor #69 + NUI 98-330
- ~~Implement NUI constructor nullification~~ ✓
- ~~Fix JIT indirect call crashes~~ ✓ Sessions 12-13
- **Game runs stably** ✓ Exit code 124 (timeout), zero crashes

### Current Focus: Trace VdSwap / Present Pipeline
- Find D3DDevice_Present and D3DDevice_Swap in MAP
- Check if they're in XDK .obj files or need decomp implementation
- Trace call chain: Game::PostUpdate → ? → Present → VdSwap
- Identify why the chain breaks (stub return, null function, missing state)

---

## Completed Items

| Date | Iteration | Item |
|------|-----------|------|
| 2026-02-22 | 1 | Agent A: CRT bisect script created |
| 2026-02-22 | 1 | Agent B: "Sord" traced — corrupted heap metadata, not string |
| 2026-02-22 | 1 | Agent C: COMDAT string failure diagnosed (jeff cf01a80 + hash mismatch) |
| 2026-02-22 | 1 | Agent D: 867 symbols categorized with fix strategies |
| 2026-02-22 | 1 | Agent E: MAP extractor validated (70/70 match) |
| 2026-02-22 | 1 | **HEAP CORRUPTION SOLVED**: bisect found constructor #69, NUI 98-330 also needed |
| 2026-02-22 | 1 | Implemented `--dc3_crt_skip_indices` cvar + fixed bisect_max semantics |
| 2026-02-23 | 2 | **JIT INDIRECT CALL FIX**: CallIndirect bounds check + resolve thunk fallback |
| 2026-02-23 | 2 | Fixed volatile register clobber (use thunk, not direct C call from JIT) |
| 2026-02-23 | 2 | No-op return stub for unresolvable functions (XAM, null, garbage) |
| 2026-02-23 | 2 | Fixed ResolveFunction null target assertion → graceful no-op |
| 2026-02-23 | 2 | Fixed Linux SysV ABI register ordering bug in CallIndirect |
| 2026-02-23 | 2 | Indirection table bounds checks in AddIndirection + PlaceGuestCode |
| 2026-02-23 | 2 | **GAME RUNS STABLY**: 30s timeout, exit 124, zero crashes, 4.4MB output |
| 2026-02-23 | 2 | Created DEBUGGING_TIPS.md — JIT architecture + debugging knowledge |
| 2026-02-23 | 2 | **DC3 NUI/XBC CUTOVER**: resolver + guest extern overrides defaulted and validated |
| 2026-02-23 | 2 | Removed legacy DC3 NUI/XBC byte-patch fallback after gate validation |
| 2026-02-23 | 2 | Added/passed `tools/dc3_nui_cutover_gate.sh` (default + strict, original + decomp) |
| 2026-02-23 | 2 | Pushed merged Xenia branch `headless-vulkan-linux` to `origin` |
| 2026-02-23 | 2 | Added `tools/dc3_runtime_parity_gate.sh` (hybrid + strict original/decomp parity checks) |
| 2026-02-23 | 2 | Added DC3 JSONL runtime telemetry (override/unresolved/hot-loop/milestone/summary events) |
| 2026-02-23 | 2 | Added `tools/dc3_runtime_telemetry_diff.py` and parity-gate per-run manifest/symbol overrides |
| 2026-02-23 | 2 | Added initial `HACK_RETIREMENT_MATRIX.md` with active workaround inventory + triggers |
| 2026-02-23 | 2 | Extracted stable non-NUI DC3 hack-pack module (`dc3_hack_pack`) for CRT/imports/debug/decomp stopgaps |
| 2026-02-23 | 2 | Added parity-gate manifest/XEX preflight integrity checks and extracted `fake_kinect_data` skeleton injection into `ApplyDc3SkeletonHackPack` |
| 2026-02-23 | 2 | Caught and reverted two extraction semantic drifts (full-image writable + `.text` zero-word `blr` patching) via cutover/parity validation |
| 2026-02-23 | 2 | Hardened DC3 patch manifest contract: schema/version validation + manifest-fingerprint target gating in Xenia |
| 2026-02-23 | 2 | **SIGBUS handler**: Added SIGBUS to exception_handler_posix.cc |
| 2026-02-23 | 2 | **Soft fault handler**: Zero dest register + advance past faulting load (mmio_handler.cc) |
| 2026-02-23 | 2 | **Guard page pre-mapping**: GPU writeback 0x7F000000 + top-of-address guard (dc3_hack_pack.cc) |
| 2026-02-23 | 2 | **JIT crash cascade**: CALL null guard, CALL_INDIRECT constants, null machine_code, null indirection, PPC scanner bounds, HIR builder 1MB cap |
| 2026-02-23 | 2 | **dcbf/dcbst → no-op in JIT**: Eliminated 1.4M clflush faults/run (x64_seq_memory.cc) |
| 2026-02-23 | 2 | **clflush/prefetch fault handler**: Cache hint instruction detection in mmio_handler.cc |
| 2026-02-23 | 2 | **THREAD 6 ALIVE**: Game executes Game::PostUpdate → D3D rendering, CS counts growing steadily |
