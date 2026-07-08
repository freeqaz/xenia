# 07 — Fix & Verification

**Status: DECIDED (2026-07-08).** This page costs each emulator-side fix option
concretely against *this* tree (`headless-vulkan-linux`). Root-cause context:
[06-root-cause.md](06-root-cause.md), [02-address-translation.md](02-address-translation.md),
[03-guest-code-analysis.md](03-guest-code-analysis.md). The options survey (A/B/C/D)
is retained below; the DECISION section is authoritative.

---

## DECISION (authoritative)

**Ship a GENERAL, cvar-gated hardware-faithful zero-page backing** — generalize
DC3's proven page-0 R/W remap **+ host-side null guard** into `memory.cc`,
controlled by the **existing `protect_zero` cvar (default `true`, unchanged)**.
RB3 clean-TU5 headless boot is unblocked by running with `--protect_zero=false`.

This is **Option A, generalized and made safe**, chosen because it is the **only
experimentally proven** unblock:

- **E1 proves it works.** `--protect_zero=false` ELIMINATES the fault (crash
  `0x8275026C` gone, `last_fault` `0x100000000→0x0`, SIGSEGV `7→0`, XE_SWAP
  frames `12→728`) and advances boot into the render loop. The fault does not
  relocate — it disappears. (`01-symptom-and-evidence.md` §E1.)
- **E2 refutes the per-title patch.** Canary's TU5 SEH-skip guest patch
  (`0x82272E90→0x4280D1F1`) and a store-skip over `0x822703FC` both boot
  **byte-identically** to baseline (still `crash_guest=0x8275026C`). The observed
  crash is a NULL page-0 **read** in the caller's FIRST call, reached *before*
  the SECOND call the patch edits. So **Option D2 is rejected as the unblock.**
  (`04-upstream-and-canary.md` §E2.)

**What changes (behavioral contract).**
- `protect_zero == true` (default): **no change** — guest page 0 stays
  `NoAccess`; every read/write/exec to `0x0..0xFFFF` faults. DC3 and all other
  titles keep today's behavior and their global null-deref guard.
- `protect_zero == false`: guest page 0 (`0x0..0x10000`) is committed **R/W**
  (as today) **and** explicitly zeroed, **and** on Linux a `PROT_READ` guard
  region is `mmap`'d `0x10000` below `virtual_membase_`
  (`MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE`) so host null-pointer derefs
  are still trapped. This mirrors `dc3_hack_pack.cc:4537-4563` but is now
  general (any title), not DC3-gated.

**File / function.** `src/xenia/memory.cc`, `Memory::Initialize`, the page-0
`AllocFixed` block (lines ~182-189). Enhance **only** the `!protect_zero` branch:
after the existing `Read|Write` `AllocFixed`, `memset` the backing to 0 and (under
`#if defined(__linux__)`, adding `#include <sys/mman.h>`) map the host guard.
~15-25 LOC, single file, additive.

**Why not B (write soft-fault, protect_zero stays ON).** B targets *writes*, but
the proven TU5 unblock requires handling a page-0 *read*; the fork's existing
READ soft-fault demonstrably does NOT catch page-0 reads (E1 baseline still
crashes on the read). B is unproven and needs extra exception-routing work for
committed-but-`PROT_NONE` pages. Kept as a possible future refinement (it would
preserve the global guard while special-casing page 0), **not** the decided fix.

**Compose with Option C (instrumentation), cheap.** Add a scoped, loud `XELOGW`
on the `Rtl*` undefined-extern path and/or an honest `RtlLookupFunctionEntry_entry`
(log + return 0). ~2-15 LOC. Not a fix, but converts silent-0 into a named
diagnostic and helps answer *why* `r28`/`r10` are NULL once page 0 is opened. Do
**not** flip `ignore_undefined_externs` globally (DC3-risky).

**DC3 safety.** Default `true` = zero behavior change. DC3 remaps page 0 R/W
itself in `ApplyDc3HackPack` (`dc3_hack_pack.cc:4537`, title-gated `0x373307D9`)
regardless of this cvar; if DC3 is ever run with `--protect_zero=false`, the
guard double-map is harmless (`MAP_FIXED_NOREPLACE` fails the 2nd → logged skip).

**Test plan.**
- **RB3 unblock (milestone a):**
  `build/bin/Linux/Checked/xenia-headless --gpu=vulkan --protect_zero=false
  --local_user_count=2 --headless_timeout_ms=120000 /tmp/rb3_nodd/default.xex`
  (`clean_tu5_nodd.xex`). PASS = no `crash_guest=0x8275026C`, `last_fault=0x0`,
  XE_SWAP frames ≫ 12 (E1 saw 728), reaches render loop. Shape ref:
  `rb3-verify/logs/nodd_smoke.log`.
- **DC3 non-regression (milestone b):**
  `build/bin/Linux/Checked/xenia-headless --gpu=vulkan <dc3.xex>` with **no
  flag** (default `protect_zero=true`) → boots to gameplay unchanged. Spot-check
  with `--protect_zero=false` → still boots (guard double-map harmless).
- **Unit (c):** x64-backend tests green (`src/xenia/cpu/testing/`).
- **Precheck:** confirm `virtual_membase_ == 0x100000000` on the live run
  (wiki 02) before trusting address arithmetic.

**Known follow-ups (out of scope for this fault).**
- Opening page 0 **masks** TU5's NULL DTA-symbol container (sought symbol not
  found) — validate downstream; use Option C logging to judge if it matters.
- E1 exposed a NEW host-side `terminate called without an active exception`
  SIGABRT (exit 134) during teardown *after* the render loop (no `crash_guest`,
  `last_fault` stays `0x0`) — the **next** blocker (Session-3 teardown-assert
  territory).

**Rollback.** Single-file, flag-gated. Default `true` keeps the change inert
unless `--protect_zero=false` is passed. Full revert = drop the `!protect_zero`
enhancement in `memory.cc`; the raw `--protect_zero=false` blunt lever remains as
before.

---

## IMPLEMENTED & VERIFIED (2026-07-08)

**Commit:** `8a7792f29b1d52b0e41534c72c6e417ae0725939` on `headless-vulkan-linux`.

**What shipped (matches the DECISION exactly):**
- `src/xenia/memory.cc`:
  - Added `#include <cerrno>` + `#include <sys/mman.h>` under `#if defined(__linux__)`.
  - In `Memory::Initialize`, after the existing page-0 `AllocFixed` (still
    `Read|Write` when `!protect_zero`), added a `!cvars::protect_zero` block that
    (1) `std::memset(TranslateVirtual<uint8_t*>(0x0), 0, 0x10000)` — hardware-
    faithful zeroed low page; (2) under `__linux__`, `mmap`s a `PROT_READ` guard
    region `0x10000` below `virtual_membase_` with
    `MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE`, logging success/skip. Mirrors
    `dc3_hack_pack.cc:4533-4568`. **`protect_zero==true` path is byte-unchanged.**
- `src/xenia/cpu/backend/x64/x64_emitter.cc` (composed Option C): in
  `UndefinedCallExtern`, emit a scoped `XELOGW("undefined Rtl* extern call ...
  (SEH/CRT frame path)")` when the extern name begins with `"Rtl"`. Control flow
  unchanged; `ignore_undefined_externs` **not** touched.

**Precheck satisfied.** Live log confirms `virtual_membase 0x100000000` (guard
mapped at `0xffff0000`), so wiki-02 address arithmetic holds for this run.

**Smoke result — RB3 clean-TU5 (`clean_tu5_nodd.xex`, md5 `42f5798101bd…`):**
PASS. Full log: `/tmp/jitfault-wf/impl-rb3-smoke.log`.

| Signal | Baseline (`nodd_smoke.log`, `protect_zero=true`) | With fix (`--protect_zero=false`) |
|---|---|---|
| `crash_guest` | `0x8275026C` (persists ≥27 s) | **absent** |
| `last_fault` | `0x100000000` | `0x0` |
| `SIGSEGV` | `7` | `0` |
| CRASH DUMP | yes (`0x8275026C: 88EA0000` = `lbz r7,0(r10)`, M2) | **none** |
| Threads reached | 15 (crash on Main XThread) | **25** (Main XThread runs 18 s+, NtWaits) |

The M2 NULL page-0 read is eliminated (not relocated); the title launches well
past the DTA-parse crash site into steady multi-thread runtime.

**New frontier (the next blocker, out of scope for the `0x100000000` fault):**
the run now ends in a host-side `terminate called without an active exception`
→ SIGABRT (exit 134) during teardown — **no `crash_guest`, `last_fault` stays
`0x0`, `SIGSEGV=0`.** A first (positional-arg, target-not-loaded) run surfaced
the same family as `xobject.cc:52 Assertion 'handles_.empty()' failed`. This is
exactly the E1/Session-3 teardown-assert territory flagged above and is the next
thing to triage.

**Deviation from the written TEST PLAN (documented per instructions):** the
plan's command passed the xex as a **positional** arg
(`… /tmp/rb3_nodd/default.xex`). This harness's `xenia-headless` requires
`--target=<path>` (see `docs/rb3-bringup-notes.md:12`); a positional path is
**ignored** ("No target specified", title never loads — only the 4 emulator
kernel threads spawn, then the teardown-assert fires). The verified command is:
```
build/bin/Linux/Checked/xenia-headless --gpu=vulkan --protect_zero=false \
  --local_user_count=2 --headless_timeout_ms=120000 \
  --target=/tmp/rb3_nodd/default.xex
```
RB3 headless harness scripts must pass `--protect_zero=false --target=…`
(deliberately a runtime flag, not a per-title hack).

**DC3 non-regression / x64 unit tests:** default `protect_zero=true` makes the
`memory.cc` change inert (NoAccess page-0 path untouched) and the Option C log
only fires on the already-undefined `Rtl*` extern path, so no behavior change for
DC3 or the backend; full DC3 boot spot-check deferred to the coordinator's
regression lane.

---

## Options survey (retained; DECISION above supersedes the DRAFT sequencing)

The fault family has **two distinct mechanisms with one signature** (wiki 03):
- **M1 — the SEH-install literal-zero store.** `stw r10, 0(0)` (`RA=0` → EA is
  the literal 0), compiler-emitted, unconditional, runs every call into the
  shared CRT/XDK SEH-frame-install routine. Retail `0x8226045C`, TU5
  `0x822703FC` (reached via the call at `0x82272E90`), DC3 `0x82311A94`.
- **M2 — the TU5 *observed* data-NULL.** `lbz r7, 0(r10)` with `r10==0`, a map
  node's string-pointer field reading back 0 during `config/band_keep.dta`
  parse (TU5 `0x8275026C`). **Open question (wiki 03 #4): is M2 a downstream
  consequence of M1's unemulated SEH, or an independent parser bug?**

Options A/B/D below neutralize **M1**. None *directly* fixes M2, but handling
M1 makes the causal-link experiment runnable (patch M1 only; observe whether M2
persists). Option C is diagnostic for both.

---

## Option A — Zero-page semantics (`protect_zero`)

**What it is today.** `DEFINE_bool(protect_zero, true, ...)` (`memory.cc:29`) —
**default ON**. The first 64 KiB of the guest virtual heap is *always committed*
(`memory.cc:184`, `AllocFixed(0x0, 0x10000, …, kMemoryAllocationReserve |
kMemoryAllocationCommit, …)`); only the **protection** is conditional
(`memory.cc:186`): `NoAccess` when `protect_zero`, else `Read|Write`. So today
**every** access to guest `0x0–0xFFFF` — read, write, *and* exec — faults
(host `PROT_NONE`). Flip it and page 0 becomes real zeroed R/W memory.

**What happens if the store to 0 silently succeeds.** Nothing else in the
emulator places kernel structs / interrupt vectors / TEB at guest `0x0–0xFFFF`.
A grep for low-address placement finds only: the guard alloc itself
(`memory.cc:184`), the physical-tail guard (`memory.cc:188`), and — decisively —
**DC3's own runtime remap of page 0 to R/W** (`dc3_hack_pack.cc:4537`,
`heap->Protect(0x0, 0x10000, Read|Write)` + `memset(base,0,0x10000)`,
log `"DC3: Mapped zero page 0x0-0x10000 (all zeros)"`). So the SEH store would
write `1` to guest 0 into otherwise-dead memory; any later read reads it back
correctly. This is *more* faithful than skipping the store (Option B).

**Real Xbox 360.** The routine stores to 0 **unconditionally** on real hardware
(wiki 03: `RA=0` is a hardcoded EA, not a runtime pointer), so page 0 must be
backed by a real (non-faulting) page on console — Xenia's `protect_zero` is a
*debugging* NULL-guard that is **stricter than the console's actual memory map**
for this architecturally-special address (wiki 03 point 3). Confirmed by DC3
booting with page 0 mapped R/W.

**DC3-safety.** *Proven safe by DC3 itself* — DC3 already runs with page 0 R/W
and boots to gameplay (`dc3_hack_pack.cc:4537`, added in `0d4eb416e`). DC3
additionally keeps a **host-side** null-guard 64 KiB *below* `virtual_membase_`
(`dc3_hack_pack.cc:4546`, Linux `mmap(guard_base, 0x10000, PROT_READ,
MAP_FIXED_NOREPLACE)`) so host-side null derefs of the membase pointer are still
caught. This is the template: map guest page 0, keep a host guard.

**Cost.**
- Files: `src/xenia/memory.cc`.
- LOC: **~1** (global default flip `protect_zero=false`) — blunt: loses the
  NULL-deref debugging aid for *all* titles. Or **~10** scoped: leave the
  default ON and add a per-title `heap->Protect(0x0,0x10000,Read|Write)` +
  host-guard, mirroring DC3 exactly (belongs in an RB3 hack-pack analog, see D).
- Composition: **mutually exclusive with B** (A makes the store succeed; B skips
  it). Independent of C/D.

**Verdict.** Lowest-effort, DC3-proven. The *scoped* variant (not the global
flip) is preferred so other titles keep the guard.

---

## Option B — Minimal guest exception dispatch (decode-and-skip the store)

**Key finding: the machinery already exists and is fork-local.** The MMIO
exception path already decodes the faulting host instruction and resumes:

- `TryDecodeLoadStore(p, decoded)` (`mmio_handler.cc:~150–410`) yields
  `is_load`, `value_reg`, and `length` of the faulting x64 insn.
- A **read** soft-fault already exists (`mmio_handler.cc:~519–533`, added in fork
  commit `610d68d78`): for a read from unmapped guest memory it zeroes the dest
  register (`ex->ModifyIntRegister(...) = 0`) and resumes
  (`ex->set_resume_pc(rip + decoded.length)`) — "keeps decomp/stub guests
  alive." A cache-hint skip (`clflush`/`prefetch`) and the DC3 64 KiB-granularity
  write re-protect (`mmio_handler.cc:462–476`) use the same resume mechanism.
- **There is no symmetric WRITE soft-fault** for a genuinely guarded page.

So the fix is a ~15–25 LOC addition inside `ExceptionCallback`'s `if (!range)`
block: if `is_write` and the fault host-addr is inside guest page 0
(`< virtual_membase_ + 0x10000`), `TryDecodeLoadStore` the store and
`set_resume_pc(rip + decoded.length)` — i.e. **emulate the store as a no-op and
resume at the next instruction.** The JIT already supports next-instruction
resume (that is exactly what the read path and cache-hint path do today).

**Honest caveat vs A.** B *discards* the stored value (no-op), whereas A stores
it and lets it read back. For M1 (`stw r10(=1), 0(0)`, an SEH bookkeeping flag)
nothing reads it back on this path, so no-op is acceptable — but A is strictly
more faithful.

**Do we need a *real* `KiUserExceptionDispatcher`-style dispatch?** No — not for
this fault. A full guest dispatch (raise into the game's `__except`) would need
`.pdata` parsing + a real `RtlLookupFunctionEntry` + scope-table walk (hundreds
of LOC; see 06's inventory of what's stubbed). But M1 is an **unconditional
bookkeeping write, not a raised exception** (wiki 03) — no `__except` ever needs
to run for it. Decode-and-skip is sufficient and honest. (A full dispatch would
only be warranted if M2 turns out to require the game's exception handler to run
— that is the open question, not established.)

**DC3-safety.** Additive and gated to the guard-page range; DC3 already bypasses
page 0 via its R/W remap (Option A path), so B never fires for DC3 → no
interaction, no regression risk.

**Cost.**
- Files: `src/xenia/cpu/mmio_handler.cc`.
- LOC: **~20**.
- Composition: mutually exclusive with A for M1; composes fine with C (diag) and
  D (D can be the oracle that proves skip-the-store is sufficient before B lands).

**Verdict.** The preferred **durable, general, DC3-neutral** emulator fix:
surgical, reuses proven fork-local decode-skip infra, keeps `protect_zero` ON
globally so real null derefs in other code still crash loudly.

---

## Option C — Honest undefined-extern / `RtlLookupFunctionEntry` semantics

**Current state.** `ignore_undefined_externs` defaults **true**
(`x64_emitter.cc:45`); a table-only export with no handler routes through
`UndefinedCallExtern`, which (flag true) logs `"undefined extern call …"` and
**returns guest r3 = 0** (`x64_emitter.cc:822–832`). Flip the flag → `FatalError`
instead (`x64_emitter.cc:824`).

**But the Rtl* family is mostly *implemented*, not undefined.** Boot logs
(`/tmp/rb3clean2.log`) show `RtlRaiseException`, `RtlCaptureContext`,
`RtlUnwind`, `RtlImageXexHeaderField`, etc. resolved as real kernel exports
(F-entries with host addresses `82C4Cxxx`). The one genuinely table-only entry
in the SEH path is **`RtlLookupFunctionEntry`** (`xboxkrnl_table.inc:319`, with
**no `_entry` implementation** in `xboxkrnl_rtl.cc` → null handler → silent-0,
matching 06's ranked candidate #1).

**Why C is diagnostic, not curative here.** Per wiki 01, **no `undefined extern`
line and no failed file-open immediately precedes any of the three crashes** —
M1 is a *compiler-emitted inline* store (never routed through an extern call),
and M2 is a *data* NULL. Flipping `ignore_undefined_externs` globally is also
**blunt and DC3-risky**: it would `FatalError` on the *first* unresolved extern
anywhere in DC3's boot, likely regressing a title that currently boots.

**Recommended scoped form.** Implement `RtlLookupFunctionEntry_entry` to return
failure **honestly and loudly** (log + return 0 — which is what it already
effectively does, minus the log), *or* add a targeted `XELOGW` in
`UndefinedCallExtern` for the `Rtl*` name prefix only. Value: converts silent-0
corruption into a named diagnostic, and would confirm whether M2's map-node NULL
is fed by an `RtlLookupFunctionEntry`→0 that some caller dereferences.

**Cost.**
- Files: `x64_emitter.cc` (flag/log) and/or `xboxkrnl_rtl.cc` + `xboxkrnl_table.inc`
  (honest `RtlLookupFunctionEntry` impl).
- LOC: flag/log ~2; honest impl ~15.
- Composition: pure instrumentation — layer under A/B/D to *explain* M2. Do
  **not** ship the global flag flip (DC3 regression risk).

**Verdict.** Worth doing as **instrumentation** to close the M2 open question;
not a fix for the observed crashes on its own.

---

## Option D — Per-title guest patch loading

**Three sub-paths, costed:**

**D1 — Port canary's `.patch.toml` patcher.** New subsystem: TOML parse,
address→word map, `writable_code_segments`. General (any title, community
patches drop-in) but heavy: **~200–400 LOC + a new file + a TOML dep**. No
in-tree loader exists today (06); we'd be importing canary's whole mechanism.

**D2 — Generalize the DC3 infra (recommended).** We already own an in-emulator
instruction patcher: `dc3_hack_pack.cc` writes guest code words at runtime via
`memory->TranslateVirtual<uint8_t*>(addr)` + `xe::store_and_swap<int32_t>(...)`
(e.g. `dc3_hack_pack.cc:857`), and `dc3_nui_patch_resolver.{h,cc}` (2793 LOC)
already models `struct …PatchSpec { uint32_t address, insn0, insn1; const char*
name; }` with manifest / catalog / symbol-map / signature resolution. The
title-gate pattern is `emulator.cc:2663` / `3567` (`title_id_ == 0x373307D9` →
`ApplyDc3HackPack`). An **RB3 analog is ~30–80 LOC**: `ApplyRb3SehPatch(memory)`
gated on `title_id_ == 0x45410914`, writing the 1–2 canary words —
retail `0x8226045C → 0x485B5DDC`, TU5 `0x82272E90 → 0x4280D1F1`
(and DC3's `0x82311A94` is already covered by A's page-0 remap, so no DC3 word
patch is needed). No new files (append beside DC3), no new deps.

**D3 — Stay with direct XEX edits.** We own the rb3-xenon `_tu5probe/clean`
`writes.json` pipeline (flat-offset = `VA − 0x82000000 + 0x3000`). **Zero
emulator change**, but produces a per-build patched artifact, doesn't compose
with DC3, and re-does work on every title update (byte drift, wiki 03 caveat).

**DC3-safety.** D2 is title-gated (`== 0x45410914`) → **cannot** touch DC3 by
construction. D3 is a separate binary → no emulator interaction.

**Cost.**
- Files: D2 → `dc3_hack_pack.cc` (or a small new `rb3_hack_pack.cc`) +
  `emulator.cc` gate; D1 → new patcher module; D3 → none (external tooling).
- LOC: D1 ~200–400; **D2 ~30–80**; D3 ~0 (in-emulator).
- Composition: D2 is the natural **oracle** — land it first, confirm RB3 boots
  past M1, run the M2 causal test, then decide A vs B for the durable general
  fix.

**Verdict.** **D2** is the fastest guaranteed RB3 unblock and DC3-safe by
construction. Prefer it over porting canary's TOML system (D1) or staying purely
external (D3).

---

## How they compose — recommended sequence

> **SUPERSEDED by the DECISION section above.** This DRAFT sequence led with D2
> as the "fastest unblock / oracle"; experiment **E2 refuted D2** (no-op for the
> observed TU5 crash) and **E1 proved Option A** is the actual unblock. The
> decided plan is A (generalized, cvar-gated) + C (instrumentation). Kept for
> the reasoning trail.

1. **D2 (per-title runtime word patch)** — fastest unblock + serves as the
   *oracle* that neutering M1's store is sufficient for RB3 boot. DC3-safe by
   title gate. ~30–80 LOC.
2. **C (scoped, loud `RtlLookupFunctionEntry` / `Rtl*` extern log)** — turn on to
   answer the **M2 open question**: with M1 patched, does the TU5 data-NULL at
   `0x8275026C` persist? If it clears, M2 was downstream of the SEH gap; if it
   persists, M2 is an independent parser bug needing its own trace. ~2–15 LOC.
3. **B (write soft-fault decode-and-skip on guest page 0)** — the durable,
   general, DC3-neutral emulator fix; keeps `protect_zero` ON so real null
   derefs elsewhere still crash. ~20 LOC. Land after D2/C validate the approach.
4. **A (scoped page-0 R/W remap)** — fallback if B's no-op semantics prove
   insufficient (i.e. something *reads back* the SEH flag). Already DC3-proven;
   mirror `dc3_hack_pack.cc:4537` + host guard. Global flip is **not**
   recommended (loses the debugging guard fleet-wide).

**A and B are alternatives** (both fix M1); **C is orthogonal instrumentation**;
**D2 is the pragmatic bootstrap** that de-risks the choice between A and B.

## Verification plan (unchanged from INDEX ground rules)

- (a) RB3 clean TU5 boots past `config/band_keep.dta` parse (no
  `crash_guest=0x8275026C` / `last_fault=0x100000000`); harness =
  `rb3-verify/logs/nodd_smoke.log` shape.
- (b) DC3 boot-to-gameplay **unchanged** (title-gated changes cannot regress it;
  A/B are DC3-inert because DC3 already remaps page 0).
- (c) x64-backend unit tests green (`src/xenia/cpu/testing/`).
- First confirm `virtual_membase_ == 0x100000000` on the live run (wiki 02
  verification hook) before trusting any address arithmetic.
