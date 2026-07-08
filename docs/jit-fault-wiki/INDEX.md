# Xenia 0x100000000 JIT-Fault Knowledge Wiki

Working wiki for root-causing and fixing the systemic host fault at
**guest-membase + 0x100000000** (exactly 4 GiB) that blocks Rock Band 3
(clean TU5 `0x8275026C`, retail base `0x8226045C`) and Dance Central 3
(`0x82311A94`) on this fork (`headless-vulkan-linux`).

**Convention:** one page per topic, cross-link with relative markdown links.
Every claim carries a `file:line` citation (Xenia source) or a quoted log/
disassembly artifact. If you learn something that contradicts a page, EDIT the
page and note the correction inline — don't fork a second page.

## Status: FIXED (2026-07-08)

Root cause and fix are settled. The fault is a **NULL-guard-policy mismatch**, not
a JIT/EA-computation bug: guest code stores to / reads from architecturally-special
guest page 0, which real Xbox 360 backs with memory but Xenia's `protect_zero`
guard makes fatal. **Shipped** commit `fb864e3e` (`headless-vulkan-linux`): a
general, cvar-gated hardware-faithful zero-page backing in `memory.cc` (+ Linux host
null-guard), plus `Rtl*` extern-call logging. RB3 clean-TU5 boots past the
`0x8275026C` fault with `--protect_zero=false` (verified A/B-identical); DC3 is
non-regressed (default `protect_zero=true` keeps the new path inert). Remaining
frontier is a **separate** host-side teardown `SIGABRT` (`xobject.cc:52`
`handles_.empty()`), out of scope for this fault. Details in
[07](07-fix-and-verification.md); root cause in [06](06-root-cause.md).

## Boot-to-menu phase: BLOCKED at a guest content/flow gate (2026-07-08)

Past the CPU fault, RB3 clean TU5 boots, streams all 10 arks, and renders the
ESRB splash + a full blue-orb LOADING scene (~77% non-zero px) — then at ~13 s
stops issuing draws and at ~18 s runs a **clean guest `App::Shutdown()`**;
`main_hub` never renders. Root-caused as a **guest-side App::Run flow-quit after
the warm-up scene** — timeout-independent (18015 ms @600 s vs 18014 ms @120 s),
crash-free, and NOT a capture/dialog/save/movie/dirty-disc bug (`XamShowDirtyDisc
ErrorUI` never fires). Only file miss is `update:\gen\patch_xbox.hdr` (LOLZ
placeholder; genuine TU5 update not on disk). **Shipped** `292ea0c18` (two
DC3-safe default-OFF diag cvars) + `0e70cebbe`/finalize (docs); no XEX gate-patch
(branch not located, blind NOP risks regression). Same-instrument 2-controller
A/B **not demonstrated** — both builds die at the same pre-menu gate. DC3
non-regressed. Full detail + next step in [08](08-boot-to-menu.md).

## Pages

| Page | Contents | Status |
|---|---|---|
| [00-source-map.md](00-source-map.md) | Where everything lives in the xenia tree | ✅ |
| [01-symptom-and-evidence.md](01-symptom-and-evidence.md) | Exact fault logs per title, guest crash PCs, boot-dir artifacts | ✅ |
| [02-address-translation.md](02-address-translation.md) | Guest→host address scheme; **why `last_fault=0x100000000` = guest NULL deref, not address-wrap** | ✅ |
| [03-guest-code-analysis.md](03-guest-code-analysis.md) | Disassembly of the faulting PPC sequences + canary patch sites; function identification | ✅ — RB3/TU5/DC3 share one compiled SEH-install routine; canary patches decoded (retail=skip-the-store, TU5=retarget-the-call); TU5's observed crash site is a sibling of, not causally proven linked to, the SEH-install site |
| [04-upstream-and-canary.md](04-upstream-and-canary.md) | **Canary's "Skip SEH usage" RB3 patch matches our crash PCs exactly**; no emulator-source fix exists upstream | ✅ |
| [05-fork-divergence.md](05-fork-divergence.md) | Fork-only commits; EA path verified byte-identical to upstream (not fork-induced) | ✅ |
| [06-root-cause.md](06-root-cause.md) | **The guest-SEH gap** + **FINAL synthesis (2026-07-08)**: two-mechanism/one-signature family; page-0 accessibility is the single proven lever (E1); canary per-title patch refuted (E2) | ✅ |
| [07-fix-and-verification.md](07-fix-and-verification.md) | **SHIPPED & VERIFIED (2026-07-08, `fb864e3e`): general cvar-gated zero-page backing** — generalizes DC3's page-0 R/W remap + Linux host null-guard into `memory.cc`, gated by `protect_zero` (default true; RB3 boots with `--protect_zero=false`). + Option C loud `Rtl*` extern logging. RB3 TU5 `0x8275026C` ELIMINATED (A/B identical); DC3 non-regression VERIFIED (default path inert). Chosen over per-title patch (E2-refuted) and write-only soft-fault (unproven for the page-0 read). Full options A–D survey retained. New frontier = host-side teardown SIGABRT (out of scope). | ✅ SHIPPED |
| [08-boot-to-menu.md](08-boot-to-menu.md) | **Boot-to-menu phase.** RB3 clean TU5 boots past the zero-page fault, streams all 10 arks, and renders the ESRB splash + blue-orb LOADING scene (~77% px) — but **`main_hub` never renders**: at ~13 s draws stop, and at ~18 s the guest runs a clean `App::Shutdown()`. Root-caused as a **guest-side App::Run flow-quit** — timeout-independent (18015 ms @600 s vs 18014 ms @120 s), crash-free, NOT a capture bug (L3), NOT dialog/save/movie (L4), and NOT a dirty-disc bail (`XamShowDirtyDiscErrorUI` never fires). Only file miss = `update:\gen\patch_xbox.hdr` (LOLZ placeholder). **Shipped:** `292ea0c18` two DC3-safe default-OFF diag cvars (`--rb3_trace_shutdown`, `--rb3_mount_update`); `0e70cebbe` docs. No XEX patch (gate branch not located; blind NOP risks regression). Same-instrument A/B **not demonstrated** (both builds die at the identical pre-menu gate). DC3 non-regressed. Next: instrument the App::Run loop-break directly to find the validation branch. | ⚠️ BLOCKED — content/flow gate |

## Ground rules for the fix

- Must be **DC3-safe**: DC3 currently boots to gameplay on this fork; any CPU
  change must not regress it. RB3 native/web ports are unaffected (different
  codebase) but DC3-on-Xenia is an active harness.
- Prefer a **general emulator fix** (correct PPC 32-bit EA semantics) over a
  per-title hack, per project direction ("improve the emulator overall").
- Verify with: (a) RB3 clean TU5 boot past `config/band_keep.dta` parse,
  (b) DC3 boot-to-gameplay unchanged, (c) any x64-backend unit tests.

## Related context (outside this wiki)

- `../rb3-bringup-notes.md` — RB3 bring-up history on this fork
- `../rb3-same-instrument-verify.md` — the A/B harness this fault blocks
- rb3-xenon deliverables: `/home/free/code/milohax/rb3-xenon/_tu5probe/clean/`
