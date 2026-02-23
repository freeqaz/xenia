# TODO: DC3 Boot Progression

*Last updated: 2026-02-23 (Iteration 2, post-NUI/XBC cutover + validation)*

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

## Iteration 2: Break NUI Callback Loop + Advance Past Init

### Tier 0: CRITICAL PATH — NUI Callback Dispatch Loop

- [x] **Cut over DC3 NUI/XBC stubs to resolver + guest overrides** [xenia] ✓ Session 11
  - Default path now uses semantic resolution + guest extern overrides
  - Legacy DC3 NUI/XBC byte-patch fallback removed after validation
  - Strict signatures-only portability gate now covers original `59/59` and decomp `85/85`

- [ ] **Break NUI callback dispatch loop at 0x834B1F40** [xenia]
  - Game stuck calling garbage function pointers (0x38600000, 0x00000000) in linked list
  - Callback list at object 0x83C16B40, offset 0x20
  - Options: stub dispatch function, empty the list, or limit no-op stub call count
  - Need to identify the dispatch function from MAP and decide best approach

- [ ] **Investigate Thunk[0x83A00964] all zeros** [xenia]
  - Import thunk at this address is all zeros instead of expected `sc 2; blr`
  - May indicate import resolution failure for this specific thunk

### Tier 1: After Loop Is Broken

- [ ] **Run boot test and observe next crash/hang** [test]
  - Once callback loop exits, what's the next blocker?
  - Iterative: fix, run, observe, fix

- [ ] **Rebuild dc3-decomp for consistent MAP+XEX** [dc3-decomp]
  - Current MAP may not match XEX — need consistent pair for address lookups
  - Update manifest (`xenia_dc3_patch_manifest.json`) from the rebuilt outputs
  - Use `tools/dc3_extract_addresses.py` for non-NUI address diagnostics only

### Tier 1.5: Runtime Parity / Validation Infrastructure (high leverage)

- [x] **Add original-vs-decomp runtime parity gate** [xenia + test] ✓ Session 13
  - Scripted milestone/log comparison beyond NUI cutover gate
  - Track boot progression, recurring loop PCs, unresolved/no-op call patterns, and override hit counts
  - Implemented `tools/dc3_runtime_parity_gate.sh` (`hybrid` + `strict`)
  - Goal: a stable regression signal for `dc3-decomp` / `jeff` changes

- [x] **Emit structured runtime telemetry (JSONL) for DC3 bring-up** [xenia] ✓ Session 13
  - Guest override hits
  - no-op stub hits / unresolvable calls
  - hot loop PCs / repeated callsites
  - Implemented cvar-gated JSONL telemetry (`dc3_runtime_telemetry_*`)
  - Use for original vs decomp diffing and prioritization

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

- [ ] **Extract non-NUI DC3 hacks into a title hack pack/module** [xenia]
  - Move CRT/skeleton/debug/decomp workaround logic out of `src/xenia/emulator.cc`
  - Keep NUI/XBC resolver path as the reference architecture
  - Goal: clearer ownership and easier hack retirement

- [x] **Create a hack retirement matrix (Xenia ↔ dc3-decomp/jeff)** [docs + xenia + decomp] ✓ Session 13 (initial scaffold)
  - For each workaround: reason, scope, current necessity, retirement trigger, owner
  - Initial matrix: `docs/dc3-boot/HACK_RETIREMENT_MATRIX.md`
  - Use this to prioritize decomp fixes that delete emulator hacks

- [ ] **Promote patch manifest as canonical machine-readable contract** [dc3-decomp + xenia]
  - `.map` remains for human investigation
  - Manifest should carry semantic targets + CRT sentinels + build identity + hash metadata
  - Add runtime `.text` hash field in decomp build workflow when available

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

### Current Focus: NUI Callback Loop
- Identify dispatch function at 0x834B1F40 from MAP
- Stub or patch the callback list to break the loop
- Re-test and observe next blocker
- Keep NUI/XBC resolver `strict` mode green as a portability regression target while iterating

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
| 2026-02-23 | 2 | Added initial `HACK_RETIREMENT_MATRIX.md` with active workaround inventory + triggers |
