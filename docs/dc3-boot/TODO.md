# TODO: DC3 Boot Progression

*Last updated: 2026-02-25 (Session 37 — CRT gStringTable/gHashTable fix COMPLETE, now at post-main file system blockers)*

Items are ordered by priority. Check off as completed. Add new items each iteration.

---

## Completed: CRT `__xc` / `gStringTable` Initialization (Session 37)

- [x] **Disassemble and characterize `Symbol::PreInit`** [analysis]
  - Confirmed: `new StringTable(560000)` → `MemAlloc(20)` → returns 0x14 (heap not ready)
- [x] **Trace `PreInit` internal allocator behavior** [xenia + analysis]
  - `MemAlloc` returns its size arg when heap uninitialized; `MemInit` runs from `main()`, not `__xc`
- [x] **Fix `gStringTable` initialization path** [xenia]
  - Host-side StringTable + gHashTable construction via `SystemHeapAlloc`
  - Symbol::PreInit override blocks original (prevents MemAlloc on uninitialized heap)
- [x] **Revalidate CRT completion to `main()`** [test]
  - Zero "Wasted string table" spam, zero mOwnEntries asserts, `_cinit` completes, `main()` reached

---

## Current Priority (Iteration 5): Post-main Game Init — ACTIVE

Boot now reaches game initialization but hits file system assertions.

Primary execution docs (read first):
- `docs/dc3-boot/STATUS.md`
- `docs/dc3-boot/CONTINUATION_PLAN.md`
- `docs/dc3-boot/DEBUGGING_TIPS.md`

### Completed: File system assertions (Session 37 cont.)

- [x] **Fix File_Win.cpp drive assertion** [xenia]
  - Set gUsingCD=1 (direct write + CheckForArchive runtime override)
  - FileIsLocal assert bypass updated to current address (0x82B17980)
- [x] **Fix File.cpp iRoot assertion** [xenia]
  - Root cause: gUsingCD=0 caused HolmesFileShare() NULL fallback
  - Fixed by setting gUsingCD=1 and symlinking gen/ assets into build dir

### Tier 0: Current blockers

- [ ] **Fix object factory instantiation** [xenia + analysis]
  - `Couldn't instantiate class Mat` / `MetaMaterial` / `Cam`
  - sFactories map appears populated but lookup fails
  - Also seen in Sessions 29-32 — may have same root cause
  - Investigate factory registration / init order

- [ ] **Fix `Data is not Array` cascades** [analysis]
  - Multiple DataNode::Array() calls on non-array data
  - Likely from config parsing — DTB structure may not match expected schema
  - Could be downstream of missing config sections

### Tier 1: Resume prior post-CRT blockers (after Tier 0)

- [ ] **Re-check SetupFont literal corruption path (`setupfont_fix`)** [dc3-decomp + xenia]
  - Verify whether decomp build freshness/literal issue is still present after CRT fix

- [ ] **Re-check `Mat` / `MetaMaterial` instantiation failures** [analysis]
  - Continue allocator/factory return-path investigation only after clean file system progression

- [ ] **Re-check heap exhaustion / `MemInit` configuration** [analysis]
  - Reproduce without conflating with the earlier CRT/string-table blocker

### Tier 2: Infrastructure / tech debt

- [ ] **Automate Dc3Addresses from manifest** [xenia + dc3-decomp]
  - Add missing symbols (globals, CRT, allocators) to `HACK_PACK_STUBS` in `generate_xenia_dc3_patch_manifest.py`
  - Make `Dc3Addresses` non-constexpr, populate from manifest at runtime
  - Currently ~50 hardcoded addresses require manual MAP refresh on each XEX rebuild

---

## Deferred / Historical Backlog

Older iterations, completed milestones, and deferred post-CRT backlog were moved
to `docs/dc3-boot/ARCHIVED.md` to keep this file focused on the current blocker.

For active non-blocker debt tracking, also use:
- `docs/dc3-boot/HACK_RETIREMENT_MATRIX.md`
- `docs/dc3-boot/CONTINUATION_PLAN.md`
- `docs/dc3-boot/STATUS.md`
