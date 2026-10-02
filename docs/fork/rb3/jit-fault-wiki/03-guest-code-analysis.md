# 03 — Guest Code Analysis

Disassembly of the faulting PPC sequences, the canary patch sites, and how
they relate. All addresses verified against **raw guest bytes** (not
speculative decompilation) via one of three sources, noted per block:

1. Xenia's own SIGSEGV crash-dump log (`Guest code near PC`, exact bytes as
   captured at fault time) — used for DC3.
2. `vf3`-disassembled `.s` files (`/home/free/tmp/vf3/build/45410914/asm/`)
   with resolved symbol names — used for RB3 retail. Cross-checked byte-exact
   against the flat `.xex` (see #3).
3. Direct extraction from the validated flat XEX2 images
   (`file_off = 0x3000 + (VA - 0x82000000)`, confirmed self-consistent across
   `/tmp/rb3cleanboot/default.xex`, `clean_tu5.xex`, `clean_tu5_nodd.xex`) fed
   through `capstone` (`CS_ARCH_PPC, CS_MODE_BIG_ENDIAN|CS_MODE_32`) — used
   for TU5. `band_clean_tu5.exe` was tried and **rejected**: it's a genuine
   PE32 (not flat) image and gives different bytes at the naive flat offset;
   avoid it unless someone adds real PE section translation.

## Headline finding: it's the same shared library routine in all three titles

RB3 retail (`fn_82260430`), RB3 TU5 (function at `0x822703D0`), and DC3
(function at `0x82311A68`) all disassemble to the **same 17-instruction
skeleton**, differing only in relocated addresses:

```
mflr r12
stw   r12, -8(r1)
stwu  r1, -0x60(r1)
lis   rA, <global_hi>            ; &g_prevArg
mr    r11, r3                    ; save incoming arg
lis   rB, <continuation_hi>
addi  r3, rB, <continuation_lo>  ; r3 = &<sibling function>  (install as new handler)
stw   r11, <global_lo>(rA)       ; g_prevArg = incoming arg
bl    <swap-helper>              ; atomically install r3 as chain head, return OLD head
lis   rC, <global2_hi>
li    r10, 1
stw   r10, 0(0)                  ; <-- FAULT: literal absolute guest EA 0x00000000
stw   r3, <global2_lo>(rC)       ; g_prevHead = old chain head (for later restore)
addi  r1, r1, 0x60
lwz   r12, -8(r1)
mtlr  r12
blr
```

| Title | Function start | Fault instruction | `bl` swap-helper target | Swap-helper body |
|---|---|---|---|---|
| RB3 retail | `fn_82260430` (`.text:0x82260430`, size `0x44`) | `0x8226045C: stw r10, 0(0)` | `0x82260450: bl fn_82815A10` | `fn_82815A10` — 5 instructions, `.text:0x82815A10` |
| RB3 TU5 | `0x822703D0` | `0x822703FC: stw r10, 0(0)` | `0x822703F0: bl 0x8283c6b0` | 5 instructions, byte-identical shape to `fn_82815A10` at relocated `0x8283c6b0` |
| DC3 | `0x82311A68` | `0x82311A94: stw r10, 0(0)` | `0x82311A88: bl 0x827fbc80` | same target reused at `0x82311A54` too (called twice in the caller) |

**The swap-helper** (`fn_82815A10`, verbatim from `vf3`):
```
lis  r10, &g_chainHead@ha
mr   r11, r3                 ; r11 = new head (arg)
lwz  r3,  g_chainHead@l(r10)  ; r3 = OLD head (return value)
stw  r11, g_chainHead@l(r10)  ; g_chainHead = new head
blr
```
This is the textbook idiom for **installing a new node at the head of a
singly-linked chain and returning the previous head so the caller can restore
it later** — i.e. exactly how Win32/Xbox-360 MSVC represents
`PrevFrame = ExceptionList; ExceptionList = NewFrame;` for structured
exception handling (SEH) frame push/pop. `fn_82260430`/`0x822703D0`/
`0x82311A68` are the **SEH-frame-install prologue** shared CRT/XDK routine,
statically linked into all three titles.

## Which register is NULL, and why the displacement is exactly 0

Per redirect-1's questions:

- **The faulting instruction is a store, not a load**, and it is architecturally
  special: `stw r10, 0(0)` encodes as `91400000` — primary opcode `stw`,
  `RA=0`. In PPC D-form addressing, **`RA=0` is a hardware special case**
  meaning "no base register — EA is the displacement literally," *not*
  "read register r0's value." Verified against Xenia's own (correct)
  `InstrEmit_stw` (`ppc_emit_memory.cc:493-511`): `if (i.D.RA == 0) { b = f.LoadZeroInt64(); }`.
  So **EA = 0 + 0 = guest address 0x00000000, unconditionally, every single
  time this code executes** — this is not a corrupted/wild pointer computed
  at runtime; it is a hardcoded constant baked into the compiled instruction.
  This is the retail/TU5/DC3 site.
- **The TU5 *observed* crash** (`0x8275026C: lbz r7, 0(r10)`) is a genuine
  register-relative load with **displacement 0**, and `last_fault=0x100000000`
  (= membase + guest 0) proves **`r10 == 0`** at the fault — a true NULL base
  register, not an overflowed/wrapped value. `r10` is fed by
  `0x82750264: lwz r10, 0x1c(r8)` — a string pointer field read out of a
  12-byte map/tree node (`r8 = node base`, stride `li r26,0xc` established
  earlier in the function) — then walked byte-by-byte
  (`lbz r7,0(r10); ...; addi r10,r10,1`) in what is structurally a `strcmp`
  loop over two symbol strings during `config/band_keep.dta` parsing. So here
  the NULL is **data** (a map node's string-pointer field reads back 0), not
  an instruction-encoded constant.

**These are two different mechanisms that produce the identical fault
signature** — important for deliverable (d), below.

## Redirect 2: the canary "Skip SEH usage" patch, decoded

### Retail: `0x8226045C` → `0x485B5DDC`

Original: `91400000` = `stw r10, 0(0)` (the fault instruction itself).
Patch value `485B5DDC` decodes (opcode 18, `AA=0`, `LK=0`) to:
```
0x8226045C: b 0x82816238
```
An **unconditional, non-linking branch that jumps clean over the store**,
landing at `0x82260460` + control flow redirected to `0x82816238` first (a
tail landing pad — needs no further chasing; the point is the store at
`0x8226045C` itself never executes). This is a **surgical single-instruction
patch at the exact crash site**: skip only the one write, fall through to the
rest of the function via the branch target, everything else (the SEH chain
push at `0x82260450`/`bl fn_82815A10`, and the deferred-restore store at
`0x82260460`) is untouched.

### TU5: `0x82272E90` → `0x4280D1F1`

Original at `0x82272E90`: `4bffd541` = `bl 0x822703d0` — a **call**, not the
fault instruction itself. `0x822703d0` is exactly the TU5 SEH-install
function from the table above (whose *internal* instruction at `0x822703FC`
is the literal-zero store).

Patch value `4280D1F1` decodes as `bc`-form (opcode 16), `BO=10100`
(`= 1z1zz` pattern → CTR ignored, condition ignored → **unconditional**),
`LK=1` (bit 31 set):
```
0x82272E90: bl 0x82270080     ; (encoded via bc-form "branch always + link",
                               ;  not opcode-18 bl — likely just the patch
                               ;  author's chosen encoding; semantically
                               ;  identical: unconditional call)
```
So the TU5 patch does **not** touch the crash instruction at all — it
**retargets the entire call** from the SEH-install wrapper (`0x822703d0`,
which unconditionally executes the literal-zero write internally at
`0x822703FC`) to a **different function**, `0x82270080`.

`0x82270080` disassembles as a large sequence of straight-line `bl` calls to
many distinct routines with no SEH-chain-install pattern anywhere in it — a
close structural match to retail's `fn_82260080` (called from retail's sibling
function `fn_82260408`, right next to the crash function `fn_82260430`), which
is a **static-initializer/global-constructor-list runner** (repeated
`lis/addi` to load `.data` pointers, `bl` into each one, loop). So the TU5
patch's effect is: **"run the static initializers directly, without going
through the SEH-wrapped entry point that would otherwise install a frame and
hit the literal-zero write."** This is a materially different patch strategy
from retail's (retarget-the-call vs. skip-one-store), but the same underlying
intent ("skip SEH usage").

**Corroboration from rb3-xenon's own symbol database** (`config/45410914/symbols.txt`,
title `45410914` = RB3 base/retail): the function containing the TU5 patch
site is *already named* by rb3-xenon's tooling, independent of anything in
this investigation:
```
except_data_82272E68   = .text:0x82272E60  ; type:object size:0x8  scope:global
except_record_82272E68 = .rdata:0x8200E5D8 ; type:object size:0x4  scope:global
```
i.e. rb3-xenon's own decompilation pipeline (presumably from linker-map or
compiler exception-table debris) already tagged the exact address range
containing `0x82272E90` as `except_*` — independent confirmation this is
genuine SEH/exception metadata, not a guess.

## Redirect 2, item (3): exception machinery near these sites

No `RtlRaiseException`/`RtlUnwind` kernel-thunk calls, no PCR/TEB-relative
loads at small fixed offsets, and no `mtmsr`/`sc` instructions appear in any
of the disassembled windows above (retail `0x82260408`–`0x82260474`, TU5
`0x82272E60`–`0x82272EB0`/`0x822703D0`–`0x82270410`, DC3
`0x82311A54`–`0x82311AA8`). The exception-handling character of this code is
**structural** (the chain-install idiom, and the independently-assigned
`except_*` symbol names) rather than call-visible — consistent with this
being the **compiler-generated SEH prologue itself** (frame push/pop), not a
call *into* a runtime exception-dispatch routine. The actual dispatch
machinery (`RtlDispatchException`/`__C_specific_handler`/scope-table walk)
would only run if a fault were *raised* through this installed frame — see
[06-root-cause.md](06-root-cause.md) for why that path is entirely stubbed on
this fork. **This static prologue write is unconditional and runs on every
call regardless of whether an exception is ever raised** — it is not itself
part of the "recovery" path, it's the frame bookkeeping that *would enable*
recovery later.

## Redirect 2, item (4): does `0x82272E90`'s function share a call chain with crash PC `0x8275026C`?

**No — not at the time of the observed crash.** Per
`docs/rb3-same-instrument-verify.md`'s traced call stack:
```
0x8283CEB0 → 0x82272E8C → 0x8227100C → 0x82741EC4/D94/974 → 0x82750188 (crash fn entry) → 0x8275026C (crash)
```
`0x82272E8C` is the **return address of the call at `0x82272E88`**
(`bl 0x82270e68`) — the *first* of two sequential calls inside the same small
function (`0x82272E60`–`0x82272EB0`). `0x82272E90` (`bl 0x822703d0`, the
canary-patched instruction) is the **second** call in that same function,
**textually immediately after** the first, but it has **not executed yet** at
the moment the actual TU5 crash fires — the crash happens deep inside the
first call's subtree (`0x82270e68 → ... → 0x8227100C → ... → 0x82750188 →
0x8275026C`), before control ever returns to fall through to the
`0x82272E90` instruction.

So the relationship is: **common caller, divergent callees.** The function at
`0x82272E60` looks like an init sequence of the shape
`ParseConfigDTA(...); InstallSEHFrame(...);` — two back-to-back steps, each
independently capable of producing the `0x100000000` fault signature via two
different mechanisms (data-dependent NULL map-node pointer vs.
instruction-encoded literal-zero write), but only the *first* one is ever
reached in the observed run. The second (the SEH-install call the canary
patch targets) is a **latent, not-yet-triggered instance of the same fault
family** downstream of it, not the cause of the currently observed crash.

## Task 4 / redirect 2 item 4: function identification in rb3-xenon

rb3-xenon's symbol table (`config/45410914/symbols.txt`, title `45410914`)
already names every retail-side function discussed here:
```
fn_82260080  = .text:0x82260080  size:0xF0   ; static-init runner
fn_82260408  = .text:0x82260408  size:0x24   ; SEH-wrapped init call
fn_82260430  = .text:0x82260430  size:0x44   ; retail crash function
fn_82815A10  = .text:0x82815A10  size:0x14   ; SEH chain swap-helper
except_data_82272E68   = .text:0x82272E60   size:0x8  ; TU5-address-coincident exception object
except_record_82272E68 = .rdata:0x8200E5D8  size:0x4
```
**Caveat:** rb3-xenon's decomp target for title `45410914` is confirmed
(`_tu5probe/FINDINGS.md`) to be **BASE/TU0, not TU5** — and that same doc
explicitly warns several same-instrument detour addresses do **not** match
between BASE and TU5 (byte drift from title updates). The retail-side lookups
above are safe (I independently verified byte-identical instructions between
`vf3`'s BASE-titled disassembly and the flat retail `.xex`). But looking up
the **TU5** address `0x8275026C` directly against this BASE-titled table is
*not* verified safe: the nearest-preceding function symbol
(`fn_82750248 = .text:0x82750248, size:0x88`) does not line up with my own
disassembly of the same bytes — my window shows a fresh function prologue
(`mflr r12` / `bl` / `stwu`) starting at `0x82750188`, with `0x82750248` sitting
mid-function (a `beq` branch target, not a prologue) — so this rb3-xenon split
boundary is very likely a **mis-split artifact**, not a real function
boundary, and should not be trusted for TU5 addresses without re-verification
against the actual TU5 binary. **Net: TU5's crash-site function (whatever its
real start address is, apparently `0x82750188` per my disassembly) is not
reliably named in rb3-xenon today** — this is a real, still-open gap for
Task 4.

## (d) What contradicts the pure "32-bit overflow" hypothesis

1. **Displacement/base-register analysis directly falsifies address-wrap.**
   Every fault site examined resolves to a base register or literal EA that
   is exactly `0`, with displacement `0` — not a large 33/34/35-bit garbage
   value that "wrapped" into `0x100000000` via missing truncation. If this
   were a truncation bug, the *pre-truncation* 64-bit value would need to be
   `≥ 2^32`; instead the values feeding these instructions are architecturally
   or data-dependently `0` from the start. (Independently corroborated by
   [02-address-translation.md](02-address-translation.md)'s source-level
   audit: the EA-truncation path is verified correct and byte-identical to
   upstream.)
2. **Two distinct mechanisms, same signature.** The retail/TU5-latent/DC3
   fault (compiler-emitted, unconditional, literal-address-zero SEH-frame
   write) and the TU5-observed fault (a data-dependent NULL string pointer
   read out of a DTA symbol-map node) are **not the same bug** even though
   both manifest as `last_fault=0x100000000`. A fix that only addresses one
   (e.g. neutering the SEH-install write, per the canary patch) will not by
   itself explain or fix the other — the DTA-map NULL-pointer path needs its
   own explanation (see open question below).
3. **The literal-zero SEH write is legitimate, executes unconditionally, and
   is likely fine on real hardware.** It runs on *every* call into this
   shared routine, success or failure, want-an-exception-or-not. Real Xbox
   360 hardware must back guest address `0x0` with a real (non-faulting) page
   for this code to work at all on real consoles — Xenia's `protect_zero`
   guard (`memory.cc:182-187`, default **on**) is a deliberate
   accident-catching NULL-guard that happens to also block this legitimate,
   load-bearing access. This reframes the bug from "JIT computes a bad
   address" to "Xenia's NULL-guard policy is stricter than real hardware's
   actual memory map for this one architecturally-special address."
4. **Open question, not yet resolved:** is the TU5 DTA-parser's NULL map-node
   string pointer itself a *downstream consequence* of the same
   unemulated-SEH gap (e.g. an exception that should have repaired/populated
   that node via a `__except` handler never ran, per
   [06-root-cause.md](06-root-cause.md) ranked-candidate #2), or is it an
   unrelated data/parser bug that coincidentally also lands on guest `0x0`?
   The call-chain analysis above shows the two fault sites are siblings under
   a common caller but were **not proven causally linked** — this is worth a
   follow-up (e.g. patching only the retail/DC3-style literal-zero write and
   checking whether the TU5 DTA-parse NULL persists or clears up).

## Semantics resolution (lane S1, 2026-07-08)

Independent re-disassembly of `clean_tu5_nodd.xex` via capstone
(`CS_ARCH_PPC, BIG_ENDIAN|32`, `file_off = 0x3000 + VA-0x82000000`) plus the
full crash register dump from `rb3-verify/logs/nodd_smoke.log`. Decoder:
`/tmp/jitfault-wf/decode_seh.py`.

### 1. The SEH-install store is a deliberate unconditional write, not a fault-probe

Full verified decode of the routine (`0x822703D0`):
```
822703d0  mflr r12
822703d4  stw  r12,-8(r1)
822703d8  stwu r1,-0x60(r1)
822703dc  lis  r10,0x82CC          ; r10 = 0x82CC0000
822703e0  mr   r11,r3              ; r11 = incoming arg
822703e4  lis  r9,0x8227
822703e8  addi r3,r9,0x3a8         ; r3 = 0x822703A8  (handler node to install)
822703ec  stw  r11,-0x3a00(r10)    ; g_prevArg @0x82CBC600 = incoming arg
822703f0  bl   0x8283c6b0          ; swap-helper: install r3 as chain head, return old
822703f4  lis  r11,0x82CC
822703f8  li   r10,1               ; r10 = 1  (constant)
822703fc  stw  r10,0(0)            ; <-- FAULT: write literal 1 to guest EA 0
82270400  stw  r3,-0x39f4(r11)     ; g_prevHead @0x82CBC60C = old chain head
82270404  addi r1,r1,0x60
82270408  lwz  r12,-8(r1)
8227040c  mtlr r12
82270410  blr
```
swap-helper `0x8283c6b0`: `lis r10,0x82E6; mr r11,r3; lwz r3,-0x5d58(r10);
stw r11,-0x5d58(r10); blr` → `g_chainHead @ 0x82E5A2A8` (install new head,
return old). Textbook `Prev = Head; Head = New;` SEH-frame **push**.

**Verdict: the store EXPECTS guest address 0 to be a writable, backed page.**
It is NOT a `__try` probe that anticipates a fault:
- The value written is the constant `1` (`li r10,1` immediately prior) — a flag
  store, not a read-back accessibility test.
- The instruction is straight-line with **no branch-around, no scope wrapping
  it**, and unconditionally **falls through** to `stw r3,-0x39f4(r11)` (save the
  old chain head for the later pop) and the epilogue. A probe expecting to fault
  would not unconditionally continue committing frame-bookkeeping state after
  the store.
- The only continuation path is the fall-through. The handler node it just
  installed (`0x822703A8`) governs the *caller's scope*, and would only be
  walked if a fault were **raised through** the frame later — dispatch is not
  reached from inside this routine.

So on real Xbox 360 hardware guest address 0 is a real (harmless) location and
this store succeeds every call; **only Xenia's `protect_zero` NULL-guard
(`memory.cc:182-187`, default on) turns it fatal.** This confirms candidate
(d)(3) above and reframes retail/DC3 as "NULL-guard stricter than hardware,"
not "JIT bug."

### 2. Open question RESOLVED — the TU5 observed crash is NOT caused by SEH divergence

Verified decode of the common caller `0x82272E60` — **three sequential calls,
each passing the same local scope object `r3 = r31+0x50`:**
```
82272e88  bl 0x82270e68   ; [1] FIRST  — body/DTA-parse (crash lives in this subtree)
82272e90  bl 0x822703d0   ; [2] SECOND — SEH-install (the canary-patched call)
82272e98  bl 0x82270000   ; [3] THIRD
82272e9c  li r3,0
```
Backtrace frame `[4] lr_bc8=0x82272E8C` = return address of the **first** call
(`0x82272E88`). The crash fires deep inside call [1]
(`0x82270e68 → … → 0x8227100C → 0x82741EC4/D94/974 → 0x82750188 → 0x8275026C`)
**before call [2] ever executes.** The SEH frame for this scope is therefore not
installed until after the crashing call would have returned — so the DTA NULL
**cannot** be a downstream consequence of SEH divergence in this scope.

Two prior candidate mechanisms are **refuted by log evidence** (`nodd_smoke.log`,
the actual crash run):
- **Silent-0 undefined-extern (06 candidate #1/mechanism): REFUTED.**
  `grep -c "undefined extern" nodd_smoke.log` = **0**. The only undefined
  externs anywhere in the corpus are `IoDismountVolumeByFileHandle` and
  `XeKeysConsolePrivateKeySign` — both **non-SEH** (filesystem / crypto) and
  present only in *other* logs, never the crash run.
- **`RtlLookupFunctionEntry`→0 dereference (06 candidate #1): REFUTED for TU5.**
  No `RtlRaiseException/RtlLookupFunctionEntry/RtlUnwind/RtlDispatchException`
  is *called* before the fault (they appear only in the IAT import listing).
  Dispatch runs only after a raised fault; this IS the first fault.

**Register dump at crash (`nodd_smoke.log:1211+`):** `r28=0` (the container/root
object), `r8=0`, `r10=0`, `r2=0`. The crashing `lbz r7,0(r10)` reads a string
pointer that is NULL because the **container object fed into the symbol/DTA
lookup is itself NULL** (`r28=0`), and that NULL propagates through the node/
field chain. Immediately-preceding log context is ARK/DTA asset load
(`main_xbox.hdr` / `main_xbox_*.ark`); the one file-not-found beforehand
(`update:\gen\patch_xbox.hdr → 0xC000000F`) is the expected "no TU update
mounted" path and falls back cleanly to `d:\gen\main_xbox.hdr` (which succeeds).

**Conclusion: the TU5 observed crash is an independent, data-dependent
NULL-container bug in DTA/ARK symbol-map lookup that shares only the guest-0
fault *signature* with the SEH store — a different mechanism, not a causal
consequence.** (The SEH store remains a real but *latent* fault in TU5, sitting
in call [2], never reached in the observed run.)

### 3. What canary's TU5 patch achieves — and why it does NOT move our crash

Canary TU5 patch (`0x82272E90`: `4bffd541 bl 0x822703d0` → `4280d1f1 bl
0x82270080`) confirmed byte-exact in the sibling-built
`_tu5probe/clean/clean_tu5_nodd_sehskip.xex` (diff = exactly this one word;
`/tmp/jitfault-wf/cmp_patch.py`). Semantically it **retargets call [2]** from the
SEH-wrapped entry (`0x822703d0`, whose internal `stw r10,0(0)` is the latent
fault) to `0x82270080` — a static-initializer runner (`mflr; bl 0x82829230;`
then a long `lis/addi`+`bl`-into-each-ctor block, structural twin of retail
`fn_82260080`). Effect: "run the static initializers directly, without
installing the SEH frame that would hit the guest-0 write."

**It patches call [2]. Our crash is in call [1], which runs first. Therefore the
canary TU5 patch is a no-op for the observed `0x8275026C` crash — a
canary-patched clean-TU5 XEX will still fault at `0x8275026C`.** (Static-analysis
certain; no booted log of `clean_tu5_nodd_sehskip.xex` exists yet —
empirical confirmation is a cheap follow-up.) Note: the `/tmp/rb3cleanpatchedboot`
logs are NOT relevant here — that XEX has both SEH sites at *original* bytes
(`0x82272E90=4bffd541`, `0x822703fc=91400000`) and wedged at 5 threads on the
content wall, a different failure.

**What WOULD fix the observed TU5 crash:**
- **(preferred, general, hardware-faithful) back guest page 0 / disable
  `protect_zero`.** This is the single change that neutralizes BOTH mechanisms:
  the SEH store (write 1 to 0 succeeds, as on real HW → fixes retail/DC3) AND the
  DTA NULL read (`lbz r7,0(0)` returns 0 → `strcmp` mismatch → "symbol not
  found" → returns instead of dying). **Caveat: for the DTA path this MASKS the
  NULL container** — the symbol the game sought won't be found, so downstream
  correctness needs runtime validation; it converts a hard crash into a
  possibly-wrong-but-survivable lookup. Still the right first move (it is what
  real hardware does) and it unblocks all three titles at once.
- **(per-title, root) fix the input that makes `r28` NULL** at call [1] — trace
  why the DataArray/symbol-table container is NULL during `main_xbox.hdr`/ARK
  load. Not resolvable from static disasm; needs a live boot *with page 0 mapped*
  to see how far it gets and whether the missing symbol matters.
- **(does NOT help) a retail-style branch-around the store inside `0x822703D0`,
  or the canary call-retarget** — both only defuse the latent call-[2] store,
  never the call-[1] crash.
