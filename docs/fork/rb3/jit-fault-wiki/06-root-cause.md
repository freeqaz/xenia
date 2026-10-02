# 06 — Root Cause: the Guest-SEH Gap

Status: **SETTLED (2026-07-08).** Mechanism inventory COMPLETE; final synthesis
below (§"FINAL synthesized root cause") is confirmed by lanes S1/E1/E2 and the
shipped fix (`fb864e3e`, [07-fix-and-verification.md](07-fix-and-verification.md)).
Background: [02-address-translation.md](02-address-translation.md) (why the
fault = guest NULL), [04-upstream-and-canary.md](04-upstream-and-canary.md)
(canary's "Skip SEH usage" patch proving the family).

## One-paragraph statement

RB3's DTA parser (and likely the DC3 site) exercises Xbox-360 guest **SEH**
(`__try/__except`, table-based unwind). On this tree, every piece of that
machinery is a stub, a silent-0 return, or absent entirely — so the exception
round-trip never completes, a NULL propagates into guest pointer
dereference-and-write, and the process dies on the guard page at guest 0x0
(host `0x100000000`). It is **not** a JIT address-computation bug (that path
is verified correct and identical to upstream/canary).

## Inventory: what Xenia implements for guest exceptions (this tree)

| Export / mechanism | Status | Where | Behavior |
|---|---|---|---|
| `RtlRaiseException` | stub | `xboxkrnl_debug.cc:126-142` | Only SetThreadName (0x406D1388) + MSVC C++ throw (0xE06D7363) special-cased; anything else → `debugging::Break()` ("TODO: unwinding. This is going to suck."). No scope-table walk; `__except` never runs. |
| `RtlCaptureContext` | stub | `xboxkrnl_rtl.cc:795-800` | `memset(context, 0, 0x200)` — no real capture (added by our `6394d2a7f`). |
| `RtlUnwind` | stub | `xboxkrnl_rtl.cc:802-807` | Empty body, XELOGW only. |
| `__C_specific_handler` | stub | `xboxkrnl_rtl.cc:809-817` | Unconditionally returns 1 (`ExceptionContinueSearch`); filters/handlers never evaluated. |
| `RtlLookupFunctionEntry` | **table-only** | `xboxkrnl_table.inc:319` | No implementation → resolves to null handler → **silently returns 0 to the guest**. |
| `RtlRaiseStatus`, `RtlUnwind2`, `RtlVirtualUnwind`, `RtlRip` | table-only | `xboxkrnl_table.inc:325,342,345,326` | Same silent-0 fate. |
| `RtlDispatchException`, `KeRaiseUserException`, `KiUserExceptionDispatcher` analog | **missing entirely** | — | No trap→guest dispatch of any kind. |
| `.pdata` parsing | **absent** | `xex_module.cc` (0 hits for pdata) | Even a real `RtlLookupFunctionEntry` would have no table to search. |
| `X_KPCR`/`X_KTHREAD` exception fields | unmodeled, zero-filled | `xthread.h:73-146`, alloc `xthread.cc:339` → `memory.cc:540` | Guest SEH state slots read back 0. |
| Host-fault→guest dispatch | **does not exist** | `x64_backend.cc:395-413` (only illegal-instruction), `mmio_handler.cc:399+` (only MMIO ranges), `emulator.cc:2234-2242` + `processor.cc:804-809` (bail without debugger) | Any guest AV = bare host SIGSEGV. |

## The silent-0 mechanism (how NULL enters the guest)

`XexModule::SetupImportThunk` (`xex_module.cc:1309-1382`): a table-only export
has `trampoline`/`shim` both null → `GuestFunction::SetupExtern(nullptr, …)`.
At call time `X64Emitter::CallExtern` (`x64_emitter.cc:833-873`) routes to
`UndefinedCallExtern` (`x64_emitter.cc:822-832`), and because
`ignore_undefined_externs` **defaults to true** (`x64_emitter.cc:45`), the call
is logged and **guest r3 = 0**. A boot log therefore should contain
`undefined extern call` lines naming exactly which Rtl* the game invoked —
check [01-symptom-and-evidence.md](01-symptom-and-evidence.md).

## Ranked NULL-write candidates (pre-disassembly)

1. **`RtlLookupFunctionEntry` → 0**, caller dereferences/writes the "found"
   function-table entry (strongest: the one routine .pdata-driven dispatch
   cannot do without, and it's wired to silently return 0).
2. **`__C_specific_handler` = ContinueSearch + `RtlRaiseException` = Break()**:
   the `__except` recovery never runs, original path continues with a bad
   pointer → NULL write downstream in ordinary code.
3. **Zero-filled KPCR/KTHREAD exception-chain slot** read by SEH prolog,
   treated as a valid frame to link into → write through 0.

The disassembly at TU5 `0x82272E90`/crash PC `0x8275026C` (and retail
`0x8226045C`) discriminates among these — see
[03-guest-code-analysis.md](03-guest-code-analysis.md).

### Re-ranking after lane-S1 disassembly + log audit (2026-07-08)

The candidates above were pre-disassembly guesses. Post-evidence
(see [03 §"Semantics resolution"](03-guest-code-analysis.md#semantics-resolution-lane-s1-2026-07-08)),
they must be split **by title**, because RB3-TU5's *observed* crash and
retail/DC3's crash are two different mechanisms with one shared signature:

- **Retail (`0x8226045C`) and DC3 (`0x82311A94`): root cause = the guest-SEH
  gap, mechanism 3 below — CONFIRMED.** The crash IS the unconditional
  `stw …,0(0)` SEH-frame-push store; real hardware backs guest 0, Xenia's
  `protect_zero` does not. Not a probe, not data-dependent (see 03 §1).
- **RB3-TU5 (`0x8275026C`): NOT the SEH gap.** Candidates #1 and #2 are
  **REFUTED** for this crash: `nodd_smoke.log` has **0** `undefined extern`
  lines and **0** pre-fault `Rtl{RaiseException,LookupFunctionEntry,Unwind,
  DispatchException}` *calls*; the crash is in the caller's **first** call
  (`bl 0x82270e68`), which runs *before* the SEH-install (`bl 0x822703d0`,
  call [2]) is ever reached. Register dump shows `r28=0` (NULL container) →
  NULL string-pointer read during ARK/DTA symbol lookup. **This is an
  independent data-dependent NULL-container bug**, sharing only the guest-0
  signature. The SEH store is a real but **latent** (never-reached) fault in
  TU5's run.

**Consequence for the fix:** the general **`protect_zero`-disable / back guest
page 0** change is the single lever that resolves *both* mechanisms
(SEH store succeeds; TU5's NULL read becomes a benign 0-read → `strcmp`
mismatch → returns), and it is hardware-faithful. It does, however, **mask**
TU5's NULL container (the sought symbol won't be found) — flag for downstream
validation. Canary's TU5 call-retarget patch is a **no-op for the observed TU5
crash** (patches call [2], crash is in call [1]).

## Patch-loader status (for the pragmatic option)

**No canary-style `.patch.toml` loader exists in-tree.** The only guest-patch
mechanism is the DC3-specific NUI resolver
(`src/xenia/dc3_nui_patch_resolver.{h,cc}`, manifest/symbol/signature-based).
Adopting canary's RB3 patch means either porting canary's patcher subsystem,
generalizing the DC3 resolver, or (zero-emulator-change option) baking the
same instruction edits into our XEX directly — we already own that tooling in
rb3-xenon (`_tu5probe/clean/` writes.json pipeline).

## Fix directions (to be decided in [07-fix-and-verification.md](07-fix-and-verification.md))

- **General (improve the emulator):** implement enough guest SEH for the
  RB3/DC3 family — candidates: real `RtlLookupFunctionEntry` (+ parse
  `.pdata` from the XEX), honest failure semantics for unresolvable Rtl*
  (loud, or correct sentinel), possibly host-AV→guest-exception dispatch for
  guarded probes. Must be DC3-safe.
- **Pragmatic (per-title):** apply canary's "Skip SEH usage" instruction edits
  (retail `0x8226045C`→`0x485B5DDC`, TU5 `0x82272E90`→`0x4280D1F1`) via our
  XEX pipeline; verify DC3's `0x82311A94` site for an analogous edit.
- Likely best: pragmatic unblock first (validates everything downstream),
  general fix second with the pragmatic patch as the oracle.

## FINAL synthesized root cause (decision synthesis, 2026-07-08)

After lanes S1 (guest semantics), E1 (`protect_zero` experiment), and E2
(canary-patch experiment), the root cause is settled. See the DECIDED fix in
[07-fix-and-verification.md](07-fix-and-verification.md).

**Statement.** The `last_fault=0x100000000` (= guest EA 0) fault is a
**two-mechanism, one-signature** family, and it is fundamentally a
**NULL-guard-policy mismatch**, not a JIT/EA-truncation or address-computation
bug:

- **M1 — retail (`0x8226045C`) / DC3 (`0x82311A94`): the guest-SEH-install
  literal-zero store.** A compiler-emitted, **unconditional** `stw r10,0(0)`
  (`RA=0` D-form special case) that pushes an SEH frame flag. Real Xbox 360
  backs guest page 0 with real memory, so this store succeeds on console;
  Xenia's `protect_zero` NULL-guard (stricter than the console memory map for
  this architecturally-special address) makes it fatal. CONFIRMED. Not a probe,
  not data-dependent.
- **M2 — RB3-TU5 *observed* (`0x8275026C`): an independent data-dependent NULL
  READ.** `lbz r7,0(r10)` with `r10==0`, a DTA symbol-map node's string-pointer
  field reading back 0 during `config/band_keep.dta` / ARK load. It is reached
  in the caller's **first** call (`bl 0x82270e68`), which runs **before** the
  SEH-install second call — so it is NOT the SEH store and shares only the
  guest-0 signature. Register dump: `r28=0` (NULL container) → NULL string read.
  Candidates #1/#2 (RtlLookupFunctionEntry→0, undefined-extern→0) are REFUTED
  for M2 (0 such lines in `nodd_smoke.log`; no Rtl* SEH dispatch called
  pre-fault).

**The single proven lever.** Making **guest page 0 accessible** resolves BOTH
mechanisms and is hardware-faithful (E1: `--protect_zero=false` eliminates the
fault, does not relocate it, and advances boot from 12 → 728 presented frames
into the render loop). The M1 store then succeeds; the M2 read returns a benign
0 → `strcmp` mismatch → lookup returns instead of dying.

**Refuted alternative.** Canary's per-title SEH guest patch does **not** unblock
RB3-TU5 (E2: byte-identical crash at `0x8275026C` with the patch applied) — it
edits the SECOND call / SEH store, but the crash is a NULL *read* in the FIRST
call. A per-title guest patch is therefore the WRONG fix for the observed crash.

**Caveats carried forward.** Opening page 0 MASKS M2's NULL container (the
sought DTA symbol won't be found), and E1 exposed a downstream host-side
`terminate called without an active exception` SIGABRT during teardown *after*
the render loop — the next blocker, out of scope for this fault.
