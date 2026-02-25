# TODO: DC3 Boot Progression

*Last updated: 2026-02-25 (Sessions 33-36 blocker shift: CRT `__xc` / `gStringTable` / `Symbol::PreInit`)*

Items are ordered by priority. Check off as completed. Add new items each iteration.

---

## Current Priority (Iteration 4): Fix CRT `__xc` / `gStringTable` Initialization — ACTIVE

Current blocker summary:
- Boot is blocked before `main()` in `_cinit` C++ constructor iteration (`__xc` loop).
- `Symbol::PreInit` is being called from the injected PPC trampoline at `__xc[0]`, but it produces `gStringTable=0x14` (garbage), not a valid heap pointer.
- Work on VdSwap/Present is **deferred** until CRT init completes again.

Primary execution docs (read first):
- `docs/dc3-boot/CONTINUATION_PLAN.md`
- `docs/dc3-boot/STATUS.md`
- `docs/dc3-boot/DEBUGGING_TIPS.md`
- `docs/dc3-boot/ARCHIVED.md` (if prior iterations/history is needed)

### Tier 0: Blocker diagnosis (must complete before post-`main()` work)

- [ ] **Disassemble and characterize `Symbol::PreInit` (`0x82556E70`)** [analysis]
  - Identify allocator calls / internal `bl` targets / early-fail paths
  - Confirm expected argument semantics for `(560000, 80000)` against decomp source + disasm
  - Use `dc3_guest_disasm.py` and/or `powerpc-none-eabi-objdump` / `powerpc-none-eabi-gdb`

- [ ] **Trace `PreInit` internal allocator behavior** [xenia + analysis]
  - Add temporary logging overrides to functions `PreInit` calls via `bl` (allocator/new helpers)
  - Capture args + return values + heap readiness indicators
  - Determine whether `gStringTable=0x14` is allocation failure, bad args, or partial write/corruption

- [ ] **Fix `gStringTable` initialization path** [xenia]
  - Choose approach based on evidence:
    - timing fix for `PreInit`,
    - host-side `StringTable` construction,
    - or alternate lazy init implemented fully in host override (no same-address forwarding)
  - Respect JIT override constraint: no "override then clear and forward to same addr" strategy

- [ ] **Revalidate CRT completion to `main()`** [test]
  - Confirm no "Wasted string table" spam
  - Confirm `_cinit` finishes and `main()` is reached

### Tier 1: Resume post-CRT blockers (only after Tier 0 passes)

- [ ] **Re-check SetupFont literal corruption path (`setupfont_fix`)** [dc3-decomp + xenia]
  - Verify whether decomp build freshness/literal issue is still present after CRT fix

- [ ] **Re-check `Mat` / `MetaMaterial` instantiation failures** [analysis]
  - Continue allocator/factory return-path investigation only after clean CRT progression

- [ ] **Re-check heap exhaustion / `MemInit` configuration** [analysis]
  - Reproduce without conflating with the earlier CRT/string-table blocker

---

## Deferred / Historical Backlog

Older iterations, completed milestones, and deferred post-CRT backlog were moved
to `docs/dc3-boot/ARCHIVED.md` to keep this file focused on the current blocker.

For active non-blocker debt tracking, also use:
- `docs/dc3-boot/HACK_RETIREMENT_MATRIX.md`
- `docs/dc3-boot/CONTINUATION_PLAN.md`
- `docs/dc3-boot/STATUS.md`
