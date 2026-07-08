# 01 — Symptom and Evidence

Raw boot-log evidence for the `0x100000000` fault family across all three
titles. See [02-address-translation.md](02-address-translation.md) for why
`last_fault=0x100000000` decodes to guest NULL, and
[03-guest-code-analysis.md](03-guest-code-analysis.md) for the disassembly.

## Boot artifact directories

| Dir | `default.xex` size | Identity | Result |
|---|---|---|---|
| `/tmp/rb3vanillaboot/` | 13,971,456 B | true retail RB3 (sha1 `c5a17091…`, cross-checked clean retail via RB3Enhanced port-address match, no `rbdxcache` string) | deterministic fault, `crash_guest=0x8226045C` |
| `/tmp/rb3tu5boot/` | 13,971,456 B | **distinct binary** (sha1 `a9fa9a91…` — same size class as vanilla but NOT byte-identical to it, and NOT any of the `_tu5probe/clean/*.xex` variants either; earliest-timestamped boot dir, likely a stale pre-Session-4 artifact) | see `/tmp/rb3tu5.log` below — **not** the NULL-deref family |
| `/tmp/rb3cleanboot/` | 15,675,392 B | TU5 dirty-disc-bypassed build (`clean_tu5_nodd.xex`, sha1 `8014b7db…`, RB3Enhanced same-instrument bypass applied per `f1a51ec07`) | boots past the content wall, then deterministic fault, `crash_guest=0x8275026C` |
| `rb3-verify/logs/nodd_smoke.log` (+ `nodd_si_smoke.log`, `nodd_nopatch.log`) | — | 3 controlled A/B boots of the TU5 build | **byte-identical fault** regardless of same-instrument patch presence/content — emulator-side, not content |
| `/tmp/dc3boot_default.xex` + `/tmp/dc3*.log` | 14,827,520 B | Dance Central 3 (`373307D9`) | deterministic fault, `crash_guest=0x82311A94` |

## Exact fault lines, per title

**RB3 retail** (`/tmp/rb3retail_final.log`):
```
=== Thread Status Report (48046ms) === 24 threads, SIGSEGV=1 last_fault=0x100000000 last_rip=0xA07C3D6A crash_guest=0x8226045C []
```

**RB3 clean TU5** (`rb3-verify/logs/nodd_smoke.log`, repeats every ~3s from 3001ms to at least 54055ms):
```
=== Thread Status Report (3001ms) === 15 threads, SIGSEGV=7 last_fault=0x100000000 last_rip=0xA00B8451 crash_guest=0x8275026C []
```
Note `SIGSEGV=7` here (vs. `=1` for retail/DC3) — the count reflects retries/
re-entry into the same faulting instruction across worker threads before the
harness gives up; the address and PC are stable across every repetition. 15
threads is itself evidence the dirty-disc wall is defeated (Session 3's
pre-bypass wedge topped out at 5 threads with `LR=0`).

**DC3** (`/tmp/dc3.log`, `/tmp/dc3reg2.log`, `/tmp/dc3_final.log` — all agree):
```
=== Thread Status Report (12010ms) === 22 threads, SIGSEGV=1 last_fault=0x100000000 last_rip=0xA0766EEA crash_guest=0x82311A94 []
```
`/tmp/dc3.log` additionally captures Xenia's own crash-dump disassembly
window (guest code near PC, LR, and full register state) — this is the
highest-fidelity evidence of the three and is what pins the DC3 instruction
bytes without needing a flat-file offset guess (see 03 for why that matters).

## `/tmp/rb3tu5boot/` is a red herring — different binary, different fault

`/tmp/rb3tu5.log` (the log associated with `/tmp/rb3tu5boot/`) does **not**
show the `0x100000000` family at all:
```
=== Thread Status Report (54047ms) === 26 threads, SIGSEGV=14 last_fault=0x17FEA1A80 last_rip=0xA0A2757F crash_guest=0x82BCEFE4 []
```
This run reaches 26 threads and a growing, non-`0x100000000` fault address
deep in gameplay rendering territory (`0x82BCEFE4`) — a different, likely
recoverable issue (`SIGSEGV` count keeps climbing across reports rather than
being static like the other three, i.e. the process is surviving and
re-faulting repeatedly, not dying once). Do not conflate this with the TU5
NULL-deref fault; the correct TU5 evidence for this fault family is
`/tmp/rb3cleanboot/` + `rb3-verify/logs/nodd_smoke.log`, above.

## Evidence gap, closed

An earlier pass of this investigation found **no log artifact** containing the
literal string `8275026C` anywhere under `/tmp` or `rb3-xenon`, which looked
like a contradiction of the task's TU5 premise. That gap is now closed: the
matching evidence lives in `rb3-verify/logs/nodd_smoke.log` (git-tracked
working-tree output, not `/tmp`), produced by the Session 4 dirty-disc-bypass
work (`f1a51ec07`) that landed *after* the initial `/tmp` sweep. The TU5 crash
is real, reproducible, and independent of the same-instrument patch (see
`docs/rb3-same-instrument-verify.md` "Verdict" table — 3 boots, byte-identical
fault with/without the patch ARK).

## No failed file-open / XAM-stub evidence directly precedes the fault

Per the redirect-1 ask: none of the captured logs show a failed
`XamContentCreateEx`/file-open or an `undefined extern call` warning in the
handful of lines immediately before any of the three crashes — the fault is
not preceded by an obvious I/O failure. This is consistent with the
mechanism identified in 03/06: the fault is a **guest-code-intrinsic**
absolute-address-zero write baked into shared XDK/CRT SEH-install code
that runs unconditionally on this path, not a data-dependent NULL produced by
a failed load.

## Experiment E1: `protect_zero` — disabling the zero-page guard eliminates the fault

**Hypothesis (from [03](03-guest-code-analysis.md) item (d)#3 + [06](06-root-cause.md)):**
the `0x100000000` fault is Xenia's NULL-guard being stricter than real
Xbox-360 hardware, which backs guest `0x0` with a real page. If so, disabling
`protect_zero` should make the load-bearing zero-page access succeed and the
fault vanish.

**Cvar** (`src/xenia/memory.cc:29`): `DEFINE_bool(protect_zero, true, ...)`,
group `"Memory"`, default **on**, currently `true` in
`~/.local/share/Xenia/xenia.config.toml:256`. It guards **both reads and
writes** of the first 64 KiB (`0x0`–`0x10000`): `memory.cc:183-187` `AllocFixed`s
that range as `kMemoryProtectNoAccess` when on, or `kMemoryProtectRead |
kMemoryProtectWrite` when off. So flipping it off makes guest `0x0` both
readable (returns 0) and writable — covering both the retail/DC3 literal-zero
*store* and the TU5-observed NULL-pointer *load*.

**Method:** identical harness to the baseline (`clean_tu5_nodd.xex`,
md5 `42f5798101bd…`, staged at `/tmp/rb3_nodd/default.xex`), adding
`--protect_zero=false`:
```
./build/bin/Linux/Checked/xenia-headless --target=/tmp/rb3_nodd/default.xex \
  --gpu=vulkan --local_user_count=2 --protect_zero=false --headless_timeout_ms=60000
```
Existing `build/bin/Linux/Checked/xenia-headless` (no rebuild). Baseline cited
from `rb3-verify/logs/nodd_smoke.log`; experiment log
`/tmp/jitfault-wf/e1-noprotect.log`.

**Result — the fault DISAPPEARS (does not move to a new guest site):**

| Metric | Baseline (`protect_zero=true`) | Experiment (`protect_zero=false`) |
|---|---|---|
| `crash_guest` | `0x8275026C` (18 reports, stable) | none — `0` occurrences of `8275026C` |
| `last_fault` | `0x100000000` | `0x0` (all 6 reports) |
| `SIGSEGV` count | `7` | `0` |
| Threads | 15 | 25 |
| `XE_SWAP` frames presented | 12 | **728** |

The TU5 DTA-parser NULL string-pointer load at `0x8275026C` no longer faults
(the zero page now reads back `0`), and boot advances **out of DTA parsing and
into the render loop** — 728 presented frames (`VdSwap`/`XE_SWAP`), 25 live
threads, worker pool parked idle on a wait at guest `LR=0x82845144`. No
`XamShowDirtyDiscErrorUI` / `XamLoaderLaunchTitle` bail fired.

**New terminal failure (different family, host-side, not `0x100000000`):**
after the render loop the process aborts with
`terminate called without an active exception` → **SIGABRT (exit 134)**, i.e. a
C++ `std::terminate` during shutdown/teardown — **no `crash_guest`, no
`last_fault` (stays `0x0`)**. There is therefore **no new guest fault site to
disassemble** (task step 4 N/A); this is a host-side teardown abort reached
after the timeout, in the same title-teardown territory as Session 3's
now-fixed assert chain (see [../rb3-bringup-notes.md](../rb3-bringup-notes.md)
§Session 3), not a relocated instance of the NULL-deref fault.

**Interpretation:** E1 **confirms** the [03](03-guest-code-analysis.md) item
(d)#3 reframing — the `0x100000000` fault is not a JIT/EA-truncation bug but
Xenia's `protect_zero` NULL-guard blocking a legitimate, load-bearing guest
access to address `0`. It also **resolves the [03](03-guest-code-analysis.md)
open question #4 in favor of "one guard, one root cause"**: a single knob
(zero-page accessibility) clears *both* the retail/DC3 literal-zero SEH store
*and* the TU5 data-dependent NULL map-node load in one shot, so for the purpose
of getting past the fault they behave as one family regardless of the two
distinct mechanisms that produce the identical signature. Caveat: `protect_zero=false`
is a blunt global relaxation (loses genuine NULL-deref detection everywhere); a
production fix should scope zero-page backing to what hardware actually
guarantees rather than disabling the guard wholesale — see
[07-fix-and-verification.md](07-fix-and-verification.md). DC3-safety of any such
change still needs the regression check per [INDEX.md](INDEX.md) ground rules.

Logs: baseline `rb3-verify/logs/nodd_smoke.log`; experiment
`/tmp/jitfault-wf/e1-noprotect.log`; checkpoint `/tmp/jitfault-wf/e1-protectzero.json`.

## Experiment E2: canary SEH-skip — the guest patch does NOT unblock our boot

**Hypothesis:** if the TU5 crash is the SEH-frame-install literal-zero write
(the fault family canary neuters), then applying canary's TU5 "Skip SEH usage"
guest patch should make the crash disappear or move.

**Method — two minted XEXes, same isolated content dir** (`/tmp/rb3_e2boot/`,
same `AvatarAwards`/`charnames.zbm`/`gen`/`nxeart` symlinks as the
`clean_tu5_nodd` runs), same harness (`xenia-headless --gpu=vulkan
--local_user_count=2 --headless_timeout_ms=120000`):

1. **`clean_tu5_nodd_sehskip.xex`** (sha1 `0c02e99b…`, from `clean_tu5_nodd.xex`
   sha1 `8014b7db…`): the exact canary TU5 patch — flat offset `0x275E90`
   (VA `0x82272E90`), `0x4BFFD541` (`bl 0x822703d0`) → `0x4280D1F1` (canary's
   verbatim value; capstone decodes it `bdnzl 0x82270080` — a decode nuance vs
   [03](03-guest-code-analysis.md)'s "unconditional" reading, but the bytes are
   canary's, applied as-is).
2. **`clean_tu5_nodd_sehskip2.xex`** (sha1 `f62c238c…`): both edits — the canary
   retarget above **plus** a retail-style store-skip inside the SEH-install
   routine `0x822703D0`. Disassembled from the validated `.xex`
   (`band_clean_tu5.exe` re-confirmed a genuine PE32 — its naive-flat word at
   `0x272E90` is `0x484D9B19`, disagreeing with the `.xex`'s `0x4BFFD541`; do
   not use it for byte lookups). The literal-zero store is at `0x822703FC`
   (`0x91400000` = `stw r10, 0(0)`); replaced with `0x48000004` (`b 0x82270400`,
   opcode 18, disp `+4`) to branch cleanly over it to the next instruction.

**Original-bytes cross-check (recorded per task):** `clean_tu5_nodd.xex` and
`clean_tu5.xex` both read `0x4BFFD541` at `0x275E90` — matching [03](03-guest-code-analysis.md)'s
expected `bl 0x822703d0`. `band_clean_tu5.exe` disagrees (PE32, see above).

**Result — UNCHANGED, byte-identical to baseline in all three runs:**

| Metric | Baseline (`nodd_smoke.log`) | `sehskip` | `sehskip2` |
|---|---|---|---|
| First crash | 3001ms | 3001ms | 3001ms |
| `crash_guest` | `0x8275026C` (only value) | `0x8275026C` (only value) | `0x8275026C` (only value) |
| `last_fault` | `0x100000000` | `0x100000000` | `0x100000000` |
| `last_rip` | `0xA00B8451` | `0xA00B8451` | `0xA00B8451` |
| `SIGSEGV` | 7 | 7 | 7 |
| Threads | 15 | 15 | 15 |

The crash does not move, clear, or change signature — not even with the SEH
literal-zero store additionally neutered.

**Interpretation — canary's patch is off the observed crash path.** The
observed TU5 crash (`0x8275026C`) is the **data-dependent NULL string-pointer
READ** in the DTA symbol-map `strcmp` loop, reached via the **first** sibling
call at `0x82272E88` during `config/band_keep.dta` parse. Canary patches the
**second** call at `0x82272E90` (the SEH-install wrapper) and the retail store
at `0x822703FC` — both **downstream of, and never reached before**, the first
call's crash (see [03](03-guest-code-analysis.md) §"does `0x82272E90`'s function
share a call chain with crash PC `0x8275026C`?" — common caller, divergent
callees). So neutering the SEH-install path **cannot** affect this crash, and
empirically does not. This **confirms [03](03-guest-code-analysis.md)'s
prediction exactly** and **corroborates E1**: the crash clears only when the
zero **page** is made accessible (E1's `protect_zero=false`), because it is a
NULL *read* the page satisfies — a guest patch on the SEH *write* site elsewhere
is irrelevant to it. **A per-title canary-style guest patch does not unblock
TU5 on this fork.**

Logs: `sehskip` `/tmp/jitfault-wf/e2-sehskip.log`; `sehskip2`
`/tmp/jitfault-wf/e2-sehskip2.log`; checkpoint
`/tmp/jitfault-wf/e2-canarypatch.json`. See implications for the fix strategy
in [04-upstream-and-canary.md](04-upstream-and-canary.md) §"E2: canary's guest
patch does not transfer".

## V1 post-fix verification — `0x8275026C` ELIMINATED; new frontier is a host-side teardown SIGABRT

Deep A/B re-verification of the landed fix (commit `fb864e3e`, the cvar-gated
hardware-faithful zero-page backing + Linux null-guard mmap) on the
same-instrument pair, both with `--protect_zero=false --local_user_count=2`.
Logs: `/tmp/jitfault-wf/v1-A-clean.log` (`clean_tu5_nodd.xex`, md5 `42f57981…`),
`/tmp/jitfault-wf/v1-B-siPATCH.log` (`clean_tu5_nodd_siPATCH.xex`, md5
`e53c8316…`), `/tmp/jitfault-wf/v1-A-input.log` (+ scripted input & frame dump).
Checkpoint `/tmp/jitfault-wf/v1-rb3.json`.

**The target fault is gone.** In all three runs `crash_guest=0x8275026C` is
**absent**, `SIGSEGV=0` and `last_fault=0x0` at every 3s Thread Status Report,
no `CRASH DUMP`. The precheck logs fire once at startup: `Mapped zero page
0x0-0x10000 as R/W (all zeros); protect_zero=false` and `Mapped null-deref guard
page at 0xffff0000 (64KB below virtual_membase 0x100000000)`.

**A/B contract HELD — the siPATCH build introduces no divergence.** Both builds
progress byte-for-byte through boot: 16 threads → **25 threads at 9006ms**,
stable through 18s; MainThread sits in the game main loop `MainThread →
DxRnd::Present → Game::PostUpdate` at *every* report 3s→18s (both: 6×
`Game::PostUpdate`, 6× `DxRnd::Present`); ~751 vs 753 `VdSwap` frames (timing
jitter). Normalized log diff (timestamps + handle-IDs + VdSwap stripped) = **88
trivial lines out of ~3300**, entirely target-path, auto thread-names, one
timing-dependent stack-walk dump. Frame dump confirms real UI renders cleanly —
`frames-A/frame_0100` is the ESRB "Online Interactions… Not Rated by the ESRB" +
autosave-symbol legal screen. Earlier reports show the MainThread in
`UIManager::GotoFirstScreen → Splash::Splash(ctor)/PrepareNext/BeginSplasher`,
i.e. the splash-screen load path, then the steady `DxRnd::Present` game loop.
This is far past the DTA-parse crash site (baseline wedged at 15 threads on
`0x8275026C`).

**New frontier — host-side C++ teardown SIGABRT (NOT a guest fault).** All three
runs end deterministically at **~18s** (not the 120s `headless_timeout`) with
`terminate called without an active exception` → SIGABRT (exit 134). The final
~100 log lines are a burst of MainThread `Removed handle:…` kernel-object
destruction (28 of 36 total removals), i.e. the emulator kernel-state teardown.
The sibling manifestation seen during impl was `xobject.cc:52: Assertion
'handles_.empty()' failed` in `XObject::~XObject` (`assert_true(handles_.empty())`
→ abort in Checked builds). No `crash_guest`, `last_fault` stays `0x0`,
`SIGSEGV=0` — there is **no guest fault and no `0x100000000` fault**; this is the
E1/Session-3 **downstream teardown-assert** territory the fix spec explicitly
scoped **out** of the `0x100000000` work. It reproduces identically on clean and
siPATCH builds and is unaffected by scripted input (13 keystrokes injected on
pad 0; run still ends at the same ~18s), so it is not the same-instrument patch
and not input-driven.

**Interactive-UI reach — honest status.** The build reaches a *stable game
main loop* (`DxRnd::Present`/`Game::PostUpdate` every report) rendering real
boot UI, but does **not** reach the interactive `main_hub`/overshell menu before
the ~18s teardown abort — so the scripted two-guitar same-instrument proof
(`rb3-verify/scripts/two_guitar_p1p2.txt`) remains **unreachable**, now blocked
by this teardown SIGABRT rather than the old NULL fault. Whether the ~18s exit is
a guest title self-exit (nodd boot has no full mounted disc content) or the
emulator ending emulation is undetermined; no explicit `XamLoaderTerminateTitle`
/ content / DVD-error import is logged in the final region. Triaging this
teardown assert is the next step toward interactive UI, tracked as out of scope
for the JIT-fault lane.
