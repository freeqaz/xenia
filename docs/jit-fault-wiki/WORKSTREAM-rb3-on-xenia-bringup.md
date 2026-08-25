# Work stream — bringing Rock Band 3 up on the Xenia fork (headless, Linux)

**Scope.** A multi-day debugging campaign to make Rock Band 3 boot far enough on
the Xenia Xbox-360 emulator fork (`/home/free/code/milohax/xenia`, branch
`headless-vulkan-linux`) to verify the *same-instrument* mod (multiple players on
one instrument) with a headless two-controller A/B. The mod itself was already
built and byte-verified; this work stream is about the **emulator + boot path**,
not the patch.

**Status at time of writing (2026-07-09).** Four distinct, load-bearing bugs
found and fixed; RB3 boots **deterministically** from cold start through the
title/splash and now **leaves `splash_screen`** into the first-boot flow. The
splash gate (bug D) is FIXED (`9096dd4d0`): the `XamEnumerate` sync-path
regression is corrected, DC3 non-regression proven. `main_hub`/instrument-select
still not reached — the residual blocker moved one flow downstream to the
first-boot `first_time_calibration → cal_audio_screen` interactive A/V-latency
calibration, which cannot complete headless with null audio + fixed-time
scripted input (see §4.D + §8).

**How to read this.** §1 is the arc in one table. §2 is the goal and why the
emulator matters. §3 is the methodology (the agent pattern + the diagnostic
techniques — the reusable part). §4 is the detective story per layer, with the
deep details. §5 is the graveyard of refuted hypotheses (as valuable as the
fixes). §6 is the general emulator-correctness lessons. §7 is commits. §8 is the
open frontier. §9 links everything.

---

## 1. The arc at a glance

| # | Blocker | Where it stopped | Root cause | Outcome |
|---|---|---|---|---|
| A | `0x100000000` "JIT fault" | ~15 threads into init | **Guest SEH gap**: unimplemented `__try/__except` path stores to guest page 0; Xenia's `protect_zero` guard makes it fatal (real 360 backs page 0) | **FIXED** `fb864e3e` (+ circuit-breaker `b803faab1`) |
| B | Which executable to run | n/a | Content on disk is **RB3 Deluxe (RB3DX)**; its `patch_xbox` ark uses `LOLZ` encryption that vanilla TU5 can't read → clean_tu5 self-exits at 18 s | **RESOLVED**: use RB3DX `default.xex` |
| C | Title-screen wedge (~1-in-2 race) | animated Deluxe title | **Host-side uninitialized memory**: POSIX `GetInfo()` never set `FileInfo::total_size` → host-ASLR stack garbage became the guest file size of `rbdxcache` → multi-hundred-MB `MemHeap::Alloc` → OOM → store to `0xFFFFFFFC` | **FIXED** `e248d624c` (8/8 broken → 13/13 clean) |
| D | Splash never advances past `splash_screen` | `splash_screen` (idle) | Fork regression `a224a6846`: `xeXamEnumerate` converted `NO_MORE_FILES → SUCCESS/0` on the **synchronous** path too, so RB3's byte-verified `MemcardXbox::FindValidUnit` `while (XEnumerate(...)==0)` loop never exits → `SaveLoadManager` never idles → the splash `{saveload_mgr is_idle}` poll never passes. (NetSession online-join hypothesis was refuted first.) | **FIXED** `9096dd4d0` (conversion restricted to overlapped path; DC3-safe, verified) |
| E | `main_hub`/instrument-select not reached | `first_time_calibration → cal_audio_screen` | First-boot interactive A/V-latency calibration (`cal_audio_panel`) can't complete headless (null audio + fixed-time scripted `A`) | **OPEN** — next gate; downstream of D |

Each fix peeled off one layer and revealed the next: **boot → title → splash →
first-boot calibration**.

---

## 2. Goal and why the emulator is on the critical path

The deliverable is a same-instrument runtime patch for RB3 (RB3Enhanced-style),
already built and **byte-verified** (its hook addresses are identical on RB3DX and
clean TU5; it applies unchanged). The emulator is the **verification vehicle**: a
headless two-controller A/B (stock vs patched) is the cleanest proof the feature
works before it ships to hardware. That A/B needs RB3 to reach the instrument-
select screen, which needs `main_hub`, which needs a clean boot. So "make RB3
boot on Xenia" became the gating task — and turned into a stack of independent
emulator bugs, each worth fixing on its own merits ("improve the emulator
overall," per project direction).

DC3 (Dance Central 3) also runs on this fork and boots to gameplay; it is the
**non-regression bar** for every change here. Every fix is either cvar-gated,
title-gated to RB3 (`0x45410914`), or a general correctness fix proven inert for
DC3 (`0x373307D9`).

---

## 3. Methodology (the reusable part)

### 3.1 The report → investigate → implement agent pattern

The productive loop, once the problem got deep, was a three-stage hand-off, each
stage a separate subagent with a fresh context:

1. **Opus compiles a self-contained report/brief** — synthesizes all prior
   evidence into one doc, cites every claim to a log line or `file:line`, and —
   crucially — ends with an explicit *"known unknowns / contradictions"* section.
   This forces the real open questions to the surface and gives the next agent a
   ground truth instead of a guess.
2. **Fable investigates** against that brief — runs the emulator, adds
   instrumentation, disassembles, and *resolves the flagged unknowns*. It is told
   to investigate, not ship.
3. **Opus implements** the recommended fix, builds, boot-tests, and reports
   honestly (including refutations).

This worked where earlier monolithic multi-agent workflows had circled a bug for
four passes without pinning it: the OOM (§4.C) had been "circled" repeatedly and
was only nailed once the Opus report isolated the two genuine unknowns and the
Fable pass chased exactly those. The pattern's value is the **honest brief** —
it prevents each new agent from re-deriving refuted theories.

### 3.2 Diagnostic techniques (Xenia-specific, reusable)

- **`__savegprlr_23` guest-entry override.** To read guest registers at a
  function's *entry* (before the JIT caches them mid-block into host regs and they
  go stale), hook via the fork's existing guest-function-override mechanism
  (`RegisterGuestFunctionOverride`, first used by `--rb3dx_alloc_probe`). **Do
  not** inject synthetic PPC code caves into `.data` "zero bytes" — those can be
  live statics.
- **`ctx->r[]` is unreliable at fault time.** The x64 JIT keeps guest GPRs in host
  registers within a basic block, so the `PPCContext` copy is stale mid-block. The
  reliable channel is the **guest stack** — e.g. reading the failure `String` Milo
  builds at `[SP+0x60]` recovered the exact `Allocation failure, heap "main", want
  N bytes` message when register reads returned garbage.
- **Signature-scan, don't hardcode addresses.** rb3-xenon's decomp (`band.exe`) is
  a *different build* than the running `default.xex` (retail + TU5 + RB3DX); code
  is relocated, so decomp addresses are wrong for the live image (e.g. `IsHost`
  decomp `0x823CECE0` vs runtime `0x823E1700`). Trace to a function via a stable
  caller, then **signature-scan its relocation-independent prologue/body** to find
  it in the live image, and verify (prologue match + caller count).
- **The circuit-breaker as a diagnosis enabler.** `--fault_spin_limit` converts a
  silent 11,640/s resume-without-progress livelock into a clean, fast,
  *diagnosable* crash with the guest EA printed. This both stopped wasted wall-
  clock and made the OOM legible.
- **Read-only guest-state probes.** `--rb3dx_ui_probe` reads `TheBandUI`
  (`0x82DFD2B0`) → `ObjectDir::sMainDir` (`0x82E054B8`) → the current screen name
  and transition state each frame — proving the game was stuck on `splash_screen`
  (not `main_hub`) with `mTransitionState = kTransitionNone`. Cheap, read-only,
  title-gated, and it reframed the entire "main_hub load stall" (§4.D).
- **The RB3 native port as an oracle.** `/home/free/code/milohax/rb3` renders
  `main_hub` correctly and documents the console-vs-host divergences it had to
  shim (NetSession, movies, save/load). When a headless Xenia boot stalls,
  checking *what the native port does at the same step* tells you what *should*
  happen — it turned the NetSession join-gate from a guess into a named mechanism.
- **Race-beating retry loops.** Before §4.C was fixed, the OOM was a ~1-in-5 good-
  boot race; a shell retry loop (boot → classify by exit code + captured-frame
  count → keep the good one) made the intermittent state reachable.

---

## 4. The detective story, layer by layer

### 4.A The `0x100000000` fault — a guest-SEH gap, not a JIT overflow

**Symptom.** RB3 (retail `0x8226045C`, clean TU5 `0x8275026C`) and DC3
(`0x82311A94`) all died with a host fault at `membase + 0x100000000` (exactly
4 GiB). The inherited assumption was a JIT bug — a guest 32-bit address
overflowing host 64-bit pointer arithmetic.

**The turn.** Xenia's Linux host mapping base is `1<<32` = `0x100000000` exactly,
and the headless harness prints `last_fault` **raw**. So `last_fault=0x100000000`
decodes to **guest address 0x0** — a NULL access, not an address wrap. The JIT's
effective-address masking was verified byte-identical to upstream and canary
(§[05](05-fork-divergence.md)); it was never the bug.

**Root cause.** All three titles hit the same compiled CRT/XDK **SEH-frame-install**
routine that unconditionally executes `stw …, 0(0)` — a store to guest page 0. On
real Xbox 360 page 0 is backed by memory; Xenia's `protect_zero` guard makes it
fatal. Guest SEH is entirely stubbed/absent in this tree (`RtlRaiseException`,
`RtlUnwind`, `__C_specific_handler` are no-ops; `RtlLookupFunctionEntry` and
friends silently return 0), so the round-trip never completes. Xenia-canary
independently ships a per-title RB3 patch named *"Skip SEH usage to prevent
nullptr write"* targeting our exact crash PCs — corroboration.

**Fix** (`fb864e3e`). A general, cvar-gated, hardware-faithful zero-page backing
in `memory.cc`: when `--protect_zero=false`, back guest page 0 R/W (+ a Linux
host-side guard below membase so host-null derefs still trap). Default
(`protect_zero=true`) is unchanged, so DC3 and every other title are byte-
identical. RB3 boots past the fault with `--protect_zero=false`. Full analysis in
[06](06-root-cause.md)/[07](07-fix-and-verification.md).

**Companion** (`b803faab1`). `--fault_spin_limit` circuit-breaker (see §3.2) — and
a real bug fixed along the way: `xe::FatalError` **from inside a signal handler
hangs** (it runs destructors from a guest-thread signal frame); reworked to park
the faulting thread and have the main thread `std::_Exit`.

### 4.B Vehicle selection — RB3DX, not clean TU5

Clean (vanilla) TU5 boots past the SEH fault but self-exits cleanly at ~18 s
after a warm-up scene, `main_hub` never rendering. The content on disk
(`/srv/torrents/games/arbys/rb3/`) is **RB3 Deluxe**: `default.xex` carries
`rbdxcache`, and `gen/patch_xbox.hdr` uses `LOLZ` magic — RB3 Deluxe's own patch-
content encryption. Vanilla TU5 can't read the `LOLZ` patch, which is why it bails.
The correct vehicle is **RB3DX's own `default.xex`**, which understands `LOLZ`;
Xenia decrypts/decompresses the compressed retail XEX natively at load (its old
`rbdxcache` crash *was* the `0x100000000` fault, now fixed). Boot dir
`/tmp/rb3dxboot` = RB3DX `default.xex` + symlinks to `/srv` for
`gen`/`charnames.zbm`/`AvatarAwards`/`nxeart`. Detail in
[08](08-boot-to-menu.md) (clean-TU5 phase) and [09](09-rb3dx-title-to-menu.md).

### 4.C The title-screen OOM — host-side uninitialized memory (the deep one)

**Symptom.** RB3DX renders the ESRB + photosensitivity splashes + the animated
Deluxe title, then wedges ~half the time: an ~11,640/s recovered-fault spin at
`crash_guest=0x827BCBD8`, `last_fault=0x1FFFFFFFC` (= guest EA `0xFFFFFFFC`).

**Mechanical chain.** Milo `MemHeap::Alloc` (@`0x827bca78`, heap **"main"**) is
called with a corrupted size `0xGG001524` — low ~5.4 KB correct, **top byte
garbage, varying per run**. The huge size exhausts the heap; retail `MILO_FAIL` is
a no-op, so execution falls through the inlined free-block-split with sentinel
`FreeBlockInfo {mBlock=NULL, mPadWords=0x7FFFFFFF}` → `stwux r27,r30,r10`
@`0x827bcbd8` stores at `0 + (0x7FFFFFFF<<2)` = guest `0xFFFFFFFC`. Xenia's
recovered-fault path resumes **without advancing the guest PC** → the spin.

**The chase.** The "stable low word 0x1524" was residue coincidence; the real
value was a **stale host pointer that varied with host ASLR**. Tracing the size
backward: guest `GetFileSize` for `rbdxcache` → `SaveLoadManager::mSaveSize` →
`_MemAllocTemp(mSaveSize)`. The size came from
`NtQueryInformationFile(FileNetworkOpenInformation)`'s `end_of_file`, which came
from `HostPathEntry::size_`, which came from **`xe::filesystem::GetInfo()` in
`filesystem_posix.cc` — which never assigned `FileInfo::total_size`** (the Windows
build zero-fills and sets it; the POSIX build left it as an uninitialized host
stack slot). That is the garbage. It explained every prior contradiction at once:
force-zeroing guest commits was inert (the garbage is *host* stack, not guest
memory), the per-run variance was ASLR, the "race" was which top byte you drew,
and Windows/console are immune (their kernels return true sizes).

**Fix** (`e248d624c`). Initialize `total_size` (+`name`/`path`) in POSIX
`GetInfo()`. **Validated: 8/8 boots corrupt pre-fix → 13/13 clean post-fix**, both
`XamContentCreate('rbdxcache')` calls complete, `main_hub` bring-up proceeds; DC3
A/B identical. It's an **upstream-inherited bug** — worth a PR back to Xenia. Full
report: [CRASH-REPORT-main-hub-oom.md](CRASH-REPORT-main-hub-oom.md).

**Also resolved:** the fault is the `stwux` **write**, "recovered" by a Linux-only
*watch-cleared abort* branch in `MMIOHandler::ExceptionCallback` — POSIX
`QueryProtect` reads the memfd-backed arena tail as ReadWrite while the page still
faults, so it retries without advancing. (Windows `VirtualQuery` would hard-crash,
which is console-faithful.) Optional hardening noted but not shipped: gate that
branch for heap-hole EAs (`LookupHeap==nullptr`).

### 4.D The splash join-gate — hypothesis found, then refuted

With the OOM fixed, boots became deterministic (18,553 VdSwaps, zero wedges) and
the game advanced **past the title into a black/blue loading-wipe** — then stuck
there. Two hypotheses were investigated and **refuted**, and a third was
identified and then also **refuted for this build**:

- **Bink menu-background video — refuted.** `main_background.bik` is a decorative
  `TexMovie`, not a load-gate; `main_hub`'s load chain never polls a movie.
- **Missing Deluxe loose files — refuted (red herrings).** `dx_*.dta` and
  `main_background.bik` fail to open (`0xC000000F`) and are absent from the dump,
  but extracting the `LOLZ` `patch_xbox` ark (arkhelper, 6,136 files) showed the
  Deluxe payload (`dx_settings.dtb`, the `dx/` tree) is **packed in the ark**
  (loaded via ArkFile, invisible to `NtCreateFile`); the loose opens are harmless
  fallback probes, `main_background.bik` is `{file_exists}`-guarded, and the
  `dx_playlist/values` are runtime user-write files that return null gracefully.
- **NetSession online-join stall — hypothesized, then refuted for this build.** A
  read-only probe showed the screen is `splash_screen` *forever* at
  `kSplashScreen_WaitOvershell`, which advances only on an
  `overshell_allowing_input(TRUE)` message fired when a local user joins via
  `OvershellSlot::AddUser → SessionMgr::AddLocalUserImpl → NetSession::AddLocalUser`
  (an online request/response join). The RB3 native port fixes exactly this gate
  (`rb3/native/src/rb3_netsession_native.cpp`). A title-gated, default-off
  `--rb3dx_offline_join` override was built (forcing the offline host branch via a
  self-locating `IsHost` signature-scan → runtime `0x823E1700`). **But direct
  instrumentation refuted it for this build:** `session.mState == 0` (idle) and
  `mQNet == 0` (null) the entire boot, and `NetSession::IsHost()` is called **0
  times despite 20 real callers** — so `AddLocalUser` is never reached and the
  join is never attempted (and with `mQNet==0` the real `IsHost()` already returns
  true, so there was no online stall to fix).

**Root cause found and FIXED (investigation #3, 2026-07-09, `9096dd4d0`).** The
gate is **upstream of the join** as suspected, and it is candidate (b): the
splash's `{saveload_mgr is_idle}` poll. `SaveLoadManager`'s auto-load parks a
Memcard worker in an **infinite guest loop** — `MemcardXbox::FindValidUnit`
(100% byte-verified vs retail) does a **synchronous** `while (XEnumerate(...) ==
0)`, and fork commit `a224a6846` (DC3 headless work) made `xeXamEnumerate`
convert `X_ERROR_NO_MORE_FILES → SUCCESS`+0-items on **both** completion paths,
so an exhausted synchronous enumerator returned `0` forever. Candidates (a)
sign-in and (c) pad/user-slot are REFUTED (no `XamShowSigninUI` call ever; real
`ReadSingleXinputJoypad` bytes accept the nop pad as `kJoypadAnalog`; and a
**no-input control boot** stalls identically — the flow is input-independent).

**The fix** (`src/xenia/kernel/xam/xam_enum.cc`, `9096dd4d0`): restrict the
`NO_MORE_FILES → SUCCESS/0` conversion to the **overlapped** path (a
`run_overlapped` wrapper); the synchronous path returns `WriteItems`' result
verbatim, restoring stock upstream Xenia semantics. DC3-safe by construction
(its enumerate consumers use the overlapped path, preserved bit-for-bit) AND
verified empirically (identical DC3 milestones/SIGSEGV at `--fault_spin_limit`
4096/0). **Verified on RB3DX:** the `flags=4096` `FindValidUnit` enumerator now
fires once and is Removed (no spin), 16,595 presents continue, and scripted
START advances `splash_screen → first_time_calibration`.

**New frontier (bug E, §8):** the boot now stalls one flow downstream on the
first-boot `first_time_calibration → cal_audio_screen` interactive A/V-latency
calibration — unreachable headless with null audio + fixed-time scripted input.
Full detail: [PLAN-splash-confirm-gate.md](PLAN-splash-confirm-gate.md) "Results
(2026-07-09)" + [BRIEF-main-hub-load-stall.md](BRIEF-main-hub-load-stall.md) §#3.

---

## 5. Refuted hypotheses (the graveyard)

Recording these so they are not re-derived:

- **JIT 32-bit→64-bit address overflow** (§4.A) — EA masking verified byte-
  identical to upstream; `last_fault` was guest NULL, not a wrap.
- **`--rb3dx_force_zero_commit`** (§4.C) — force-zeroing `NtAllocateVirtualMemory`
  + `MmAllocatePhysicalMemoryEx` did not fix the OOM; the garbage was host stack,
  not guest memory.
- **Bink main-menu video** (§4.D) — decorative `TexMovie`, not a gate.
- **Missing `dx_*.dta` / `main_background.bik`** (§4.D) — packed in the `LOLZ` ark
  / graceful-null; not the gate.
- **Nav/input timing** (§4.D) — input reaches the guest; START-spam on good boots
  does not leave the title/splash.
- **NetSession online-join stall** (§4.D) — `AddLocalUser` never reached
  (`mState==0`, `IsHost` 0 calls); the join isn't the active gate for this build.
- **Splash needs active profile sign-in** (§4.D cand. (a)) — `XamShowSigninUI`
  never invoked at runtime; no signin predicate in `OvershellSlot::AddUser`; the
  real gate was the `is_idle` enumerate loop.
- **Confirm lands on wrong pad/user-slot** (§4.D cand. (c)) — real
  `ReadSingleXinputJoypad` bytes accept the nop pad as `kJoypadAnalog`; a
  no-input control boot stalls identically (flow is input-independent).

---

## 6. General emulator-correctness lessons

Bugs found here are not RB3-specific; several are upstream-relevant:

1. **Uninitialized `FileInfo::total_size` in POSIX `GetInfo()`** (`e248d624c`) —
   host stack garbage leaking into guest-visible file sizes. Upstream-inherited;
   any title that reads a file size and allocates by it is exposed. PR-worthy.
2. **Guest SEH unimplemented** (`Rtl*` stubs / silent-0 returns) — benign until a
   title's `__try/__except` path runs, then it stores through NULL. The zero-page
   backing (`fb864e3e`) papers over the store faithfully; a real fix would
   implement guest exception dispatch.
3. **`FatalError` from a signal handler hangs** (fixed in `b803faab1`) — running
   destructors / `std::exit` from a guest-thread signal frame deadlocks; use
   `_Exit` from a non-signal thread.
4. **Linux watch-cleared-abort livelock** (§4.C) — POSIX `QueryProtect` on the
   memfd-backed arena tail reports ReadWrite while the page faults, so a heap-hole
   fault retries forever instead of crashing (Windows would crash, which is
   console-faithful). `--fault_spin_limit` is the backstop; gating the branch for
   `LookupHeap==nullptr` EAs is the real fix.

---

## 7. Commits (branch `headless-vulkan-linux`)

| Commit | What |
|---|---|
| `fb864e3e` | Zero-page backing + `Rtl*` extern logging (the SEH-gap fix) |
| `b803faab1` | `--fault_spin_limit` fault-livelock circuit-breaker + OOM diagnostics + signal-handler-hang fix |
| `292ea0c18` | Clean-TU5 phase diag cvars (`--rb3_trace_shutdown`, `--rb3_mount_update`), default-off |
| `e248d624c` | **POSIX `GetInfo()` `total_size` init — the title-screen OOM fix** |
| `9096dd4d0` | **`xeXamEnumerate` sync-path `NO_MORE_FILES` restore — the splash-gate fix** (regresses `a224a6846`; DC3-safe, verified) |
| `0c0f291b4` | `--rb3dx_alloc_probe` diagnostics, default-off, DC3-inert |
| `5e118d415` | Crash-report doc |
| (uncommitted) | `--rb3dx_offline_join` + `--rb3dx_ui_probe` in `emulator.cc` — kept as default-off, title-gated diagnostics (the `ui_probe` sampler was the tool that verified this fix); held for review, not folded into the fix commit |

All emulator behavior changes are cvar-gated and/or title-gated (`0x45410914`) and
default-off or default-inert; **DC3 non-regression verified** at each step.

---

## 8. Open frontier + next step

**UPDATE 2026-07-09 — the splash gate is FIXED (`9096dd4d0`); next gate =
first-boot calibration.** Investigation #3 traced the pre-join splash step to the
`{saveload_mgr is_idle}` poll (candidate (b)) and named a **fork regression**:
commit `a224a6846` made `xeXamEnumerate` convert `X_ERROR_NO_MORE_FILES` →
`SUCCESS`+0 items on BOTH paths, so RB3's byte-verified synchronous
`MemcardXbox::FindValidUnit` `while (XEnumerate(...)==0)` loop never exited.
Sign-in (a) and pad/user-slot (c) refuted. **The fix landed** (`9096dd4d0`):
restrict the conversion to the overlapped path; the synchronous path returns
`WriteItems`' result verbatim. Verified: the `flags=4096` enumerator fires once
and is Removed (no spin), 16,595 presents continue, scripted START advances
`splash_screen → first_time_calibration`. **DC3 non-regression PROVEN** (identical
milestones/SIGSEGV at `--fault_spin_limit` 4096/0; DC3 never exercises the sync
path). Fix + verification matrix:
**[PLAN-splash-confirm-gate.md](PLAN-splash-confirm-gate.md)** "Results
(2026-07-09)"; evidence: [BRIEF-main-hub-load-stall.md](BRIEF-main-hub-load-stall.md)
§#3 + "Fix landed".

**The next gate (bug E) — first-boot A/V-latency calibration.** With the splash
cleared, the boot advances `splash_screen → first_time_calibration →
cal_welcome_screen → cal_audio_screen`, then stalls (~74 s to timeout). Input is
proven still live on `cal_audio_screen` (`XamInputGetState` user-0 `0x1000`
during the stall), but `cal_audio_panel` is an **interactive** output-vs-input
latency calibration — headless with null audio and fixed-time scripted `A`
presses it cannot complete, so the screen never advances.
`main_hub_screen`/`instrument_screen` appear only in the maindir screen-registry,
never as `curScreen`. An opt2-skip attempt (`DOWN`+`A` on the
`first_time_calibration` dialog) regressed to staying on
`first_time_calibration` — not trivially scriptable. Next-pass leads (cheapest
first): (i) prime `get_has_seen_first_time_calibration` true so first boot skips
calibration → `main_hub`; (ii) drive the exact `cal_audio` skip-button focus
sequence; (iii) feed a headless calibration-complete signal. All are downstream
of, and independent of, the now-fixed enumerate contract.

The same-instrument two-controller A/B remains staged and ready the moment
`main_hub` → instrument-select is reachable; the patch's hook VAs are
byte-verified for RB3DX and delivered as a title-gated runtime memory patch (no
XEX repack).

**UPDATE 2026-07-09 (wave 3) — calibration bypass landed; a NEW gate (doc-09
main_hub-load abort) blocks instrument-select; the SI verdict was obtained
WITHOUT reaching it, via a direct guest-function self-test.** Three new
default-off, title-gated (`0x45410914`) cvars were added to `emulator.cc`
(uncommitted, additive-only; harness files `command_processor.cc`,
`nop_input_driver.{cc,h}`, `xam_input.cc` untouched by this lane):

- **`--rb3dx_skip_calibration`** — lead (ii) from the "next-pass leads" above
  (poke, not the getter-override in lead (i): `ProfileMgr::Handle` inlines the
  `mHasSeenFirstTimeCalibration` read at `-inline noauto`, so overriding the
  standalone leaf `GetHasSeenFirstTimeCalibration@0x82794838` fires 0 times —
  a trap for this title). A detached host thread resolves `profile_mgr` via
  `ObjectDir::sMainDir` and writes `1` to the flag byte every 150 ms
  (`obj_vbase - 0x74`, i.e. `this + 0x54`; the dir stores the `Hmx::Object`
  vbase for `MsgSource`-derived classes, so derived fields sit at *negative*
  offsets from the dir pointer — not `+0x54` from the dir pointer as a naive
  header-offset read would suggest). Result: `splash_screen` routes straight to
  `main_hub_screen`; `first_time_calibration` never becomes `curScreen` (0/N
  across all runs, 3/3 reproduced). This retires lead (i) and confirms (ii);
  lead (iii) (exact `cal_audio` skip-button sequence) is now moot for our goal.
- **`--si_probe`** — read-only host thread logging the same-instrument flag
  word (`0x82C8AAA0`), the `IsActive` detour word (`0x826684C0`), and one-time
  hex dumps of the cave stub / original body / trampoline. Confirmed the flag
  is a correct, stable `1` at runtime (never zeroed) and the detour word is
  live in guest RAM — both via passive reads, no execution.
- **`--si_selftest`** — fires once (title-gated, mid-boot) inside the existing
  `__savegprlr_23` MemAlloc override: snapshots the live guest-thread PPC
  context, calls `processor->Execute()` directly against the detour site
  (`0x826684C0`) and the cave entry (`0x82C8A080`) with a crafted empty `this`,
  logs `r3`, then restores registers so the intercepted call continues
  undisturbed. This is what produced the SI verdict below — **it does not
  require reaching instrument-select**, sidestepping the new doc-09 blocker
  entirely.
- A fourth cvar, **`--rb3dx_clamp_alloc`**, was added as an attempted mitigation
  for the doc-09 main_hub-load abort but never fired (that crash path emits
  `XamAlloc: unk=268435456` — a different signature than the garbage-top-byte
  size this cvar clamps — and "proceeds anyway"). The doc-09 gate is unsolved;
  see `09-rb3dx-title-to-menu.md` and the wave-3 verdict doc referenced below.

**SI verdict (headline): the static `.data` code cave is INERT under Xenia too**
— `Execute()` on the `.text→.data` detour branch returns unchanged (branch not
followed), and `Execute()` on the cave entry itself fails to resolve
(`Processor::ResolveFunction`: "failed to find function", `0xDEADBABE` sentinel)
because Xenia's function analyzer only translates addresses a module claims as
code. A `.text`-resident positive control (`IsActive → li r3,1; blr`) executes
correctly (`r3=1`), isolating the `.data` location as the sole cause — flag- and
logic-independent (`default_tu5_forceall.xex`, all 4 cave flag-loads forced,
gives an identical inert result). Full evidence, artifact hashes, and the console implication:
`/tmp/si-hw-fix/WAVE3-XENIA-VERDICT.md` (scratch path, not checked into this
repo) and `/tmp/si-hw-fix/wave3-xenia/` (`recon.json`, `calskip.json`,
`twopad.json`, `drive.json`, `logs/`).

---

## 8b. The `0x82BCEFE4` "hub-load crash" — REFUTED, it is a recovered XMA-MMIO red herring (2026-07-13)

**Framing that was handed off.** The 2026-07-12 reference-capture pass reported
that with `--rb3dx_skip_calibration` the boot routes `splash_screen →
main_hub_screen` (curScreen 2/2) and then "the hub 3D scene load **crashes the
guest**: SIGSEGV at guest PC `0x82BCEFE4`, fault EA `0x7FEA1A80`, identical on
both boots; SIGSEGV count climbs 2→8 then host `terminate` at ~30 s." This was
taken to be *the wall* to in-game reference capture.

**Verdict: that crash is a red herring.** `0x82BCEFE4` is a **guest XMA hardware
MMIO register write that Xenia catches and recovers correctly** — expected,
by-design MMIO emulation, not a fault that stops the guest. Evidence and
mechanism below.

**Disassembly (real RB3DX bytes, `xex1tool -b` image `/tmp/rb3dx-wf/rb3dx_decomp.bin`,
base `0x82000000`).** The faulting function is at `0x82BCEF04` (an XDK/libXMA
audio-context routine statically linked into `band.exe`; not in the partial
rb3-xenon decomp, and not Deluxe-injected). It manages an array of 0x60-stride
audio contexts and pokes the Xbox 360 XMA decoder registers directly:

```
; helper @0x82BCEEA8 — read XMA ContextArrayAddress (reg 0x1800) and cache it
0x82bceea8  lis   r11,0x7fea ; ori r11,r11,0x1800   ; r11 = 0x7FEA1800
0x82bceeb4  lwbrx r11,0,r11                          ; MMIO READ (little-endian)
0x82bceeb8  stw   r11,-0x3098(r10)                   ; cache @0x82E4CF68

; main loop @0x82BCEF04 — per context, compute a value and kick a register
0x82bcefe0  slwi   r10,r9,2                          ; r10 = 0x7FEA1A80
0x82bcefe4  stwbrx r11,0,r10                         ; MMIO WRITE  <<< FAULT
0x82bcefe8  eieio                                    ; enforce in-order I/O
```

`stwbrx`+`eieio` (store-word-byte-reversed then enforce-in-order-IO) is the
textbook PPC idiom for a memory-mapped **device register write**. The EA is the
constant `0x7FEA1A80` (= host `0x17FEA1A80` = the reported `last_fault`), computed
purely from an `addis 0x1ffb / addi -0x7960 / slwi 2` sequence — no register
mistranslation involved.

**Why it faults and why that is fine.** `0x7FEA0000` is the **XMA decoder MMIO
range**, registered by `apu/xma_decoder.cc:114`
(`AddVirtualMappedRange(0x7FEA0000, 0xFFFF0000, 0x0000FFFF, …)`). The guest page
isn't backed by RAM, so the host store SIGSEGVs; `MMIOHandler::ExceptionCallback`
(`cpu/mmio_handler.cc`) matches `0x7FEA1A80 & 0xFFFF0000 == 0x7FEA0000`, decodes
the JIT's `MOVBE` store, and dispatches to `XmaDecoder::WriteRegister` (reg
`0x1A80/4 = 0x6A0`; the `0x1800` read → reg `0x600 = ContextArrayAddress`). **The
guest PC is advanced and execution resumes.** This is exactly how *every* XMA
register access is emulated on this fork.

**Direct proof it is not the wall:**
- `run1.log` (95 s timeout): after the fault, VdSwaps keep climbing to **#5000+**
  while **SIGSEGV plateaus at 18** — the game renders to timeout, no livelock (cf.
  the doc-09 OOM's ~11,640/s resume-without-progress spin, which this is NOT).
- Fresh 2026-07-13 run (`/tmp/xhc/sw2.log`, swiftshader): the fault fires, **SIGSEGV
  plateaus at 8 and is stable from 36 s → 90 s** (a *one-time* XMA-init burst, not
  continuous), the process stays alive, and the `splash → main_hub` transition
  proceeds throughout.
- "`terminate called without an active exception`" at the run's end is the
  **known-benign teardown** (`xobject.cc:52` `handles_.empty()` assert on process
  exit) — it fires identically on a clean timeout *and* on an external kill, and
  is unrelated to the guest. The "2→8 then terminate at ~30 s" was simply the
  30 s `--headless_timeout_ms` teardown; the SIGSEGV counter merely tallies the
  recovered MMIO faults.

**Fix classification: (c) — nothing to fix here.** The XMA MMIO recovery is
working as designed; there is no host uninitialized-leak (not class a) and no
guest/emu-gap workaround is warranted (not class b). *No code change made.* (A
possible future *quality* improvement — teach the JIT to route known MMIO ranges
without a fault/recover round-trip, and/or stop the headless harness counting
recovered MMIO faults as "SIGSEGV" so they stop masquerading as crashes — is
optional and out of scope.)

**The ACTUAL wall (unchanged from the doc-09 gate): the `main_hub_panel` async
load never completes.** With `--rb3dx_skip_calibration`, the boot advances to the
`splash → main_hub_screen` transition (confirmed: `run3.log` reaches
`transState=2 curScreen=main_hub_screen`; the 2026-07-13 swiftshader run reaches
`transState=1 … transScreen=main_hub_screen`). But `--rb3dx_ui_probe` shows the
hub panel wedged mid-load:

```
T/C:panel[..] 'main_hub_panel' active=1 refLoaded=1 mState=2 mLoaded=0 mLoader=0x00000000 mLoadRefs=1
```

i.e. a load is *requested* (`mLoadRefs=1`) but `mLoaded=0` and **no Loader object
exists** (`mLoader=NULL`) — the scene load stalls. Companion signature during the
transition: repeated `XamAlloc: unk=268435456 (expected 0); proceeding anyway`
(flags `0x10000000` ignored by `xam_info.cc:328`; `SystemHeapAlloc(size)` still
succeeds, so this is a symptom, not proven-causal). This is the same
"doc-09 main_hub-load abort" gate flagged in §8 wave 3 — still open.

**Environment caveat for capture.** In-game reference capture (task 3) could NOT
be completed this pass: the host **NVIDIA Vulkan driver is version-mismatched**
(kernel NVRM `610.43.02` vs userspace `libGLX_nvidia.so 610.43.03`, updated
2026-07-09/12 — needs a reboot), so hardware Vulkan returns
`ERROR_INCOMPATIBLE_DRIVER`. Software Vulkan (chromium swiftshader,
`VK_ICD_FILENAMES=/usr/lib/chromium/vk_swiftshader_icd.json`) boots and renders
but (a) is far too slow to confirm hub-load completion and (b) reproduces the L3
tiled-readback scramble worse than hardware, so captured frames are unusable as
references (`/tmp/rb3dx-wf/hub_transition_stuck_swiftshader.png`).

**Next cheapest levers for the real wall (main_hub_panel load):**
1. Add a read-only `--rb3dx_ui_probe` extension that also dumps the panel's
   `Loader`/`FileStream` state and the file/ark the loader is blocked on (mirror
   the native port's `main_hub` `.milo` load list) — the panel shows `mLoader=NULL`,
   so first establish *whether the Loader is never created* vs *created-and-freed
   without setting `mLoaded`*.
2. Attribute the `XamAlloc unk=0x10000000` calls: log caller VA + returned ptr +
   whether the guest treats the (flags-ignored, virtual not physical) allocation
   as physical/contiguous — a wrong heap type there could fault the loader
   downstream. This is the one concrete emulation-gap lead.
3. Restore hardware Vulkan (reboot to sync the NVIDIA kernel/userspace versions)
   before any capture attempt — the swiftshader path cannot yield usable refs.

Artifacts: disasm scripts `/tmp/rb3dx-wf/disasm*.py` (image regenerated via
`xex1tool -b`, `reverse-compiler-refs/idaxex/xex1tool/build/xex1tool`); logs
`/tmp/xhc/sw2.log`, `/tmp/xenia-refcap/run{1,3}.log`; evidence frame
`/tmp/rb3dx-wf/hub_transition_stuck_swiftshader.png`. No commits; no cvar changes
(the crash needed neither).

---

## 8c. 2026-08-24 — GAMEPLAY REACHED; the "main_hub_panel load stall" was two host crashes in disguise

Resuming after the August alloc-trace detour, a fresh reproduction under the
2026-08-24 binary (same branch, now carrying the July cvars as committed code)
showed the boot **aborting** ~30 s in at the splash → main_hub transition —
not stalling. Two independent Checked-build host crashes, peeled in order:

1. **Thread-suicide terminate** (`25c506505`). A fire-and-forget guest worker
   whose handle the guest had already closed reached `XThread::Exit`; its own
   `ReleaseHandle()` dropped the LAST reference, so `~XThread` ran **on the
   exiting thread** and destroyed its own `xe::threading::Thread`.
   `~PosixCondition<Thread>` then did `pthread_cancel(self)` +
   `pthread_join(self)` — and since `ThreadStartRoutine` sets ASYNCHRONOUS
   cancel type, the self-cancel force-unwound through the noexcept destructor
   → `std::terminate` ("terminate called without an active exception", core
   Thread-1 stack: `~PosixCondition ← ~PosixThread ← ~XThread ←
   ObjectTable::RemoveHandle ← XThread::Exit`). Fix: self-destruction detaches
   instead of cancel/join and clears the TLS `current_thread_` so
   `Thread::Exit` falls through to plain `pthread_exit` instead of calling
   `Terminate()` on the freed object. Same teardown family as `ef5025af9`,
   one link deeper; Windows never sees it (`~Thread` there just closes a
   handle).
2. **XConfig Checked assert** (`6fff0996a`). RB3DX queries user-category
   XConfig setting `0x000F` once; `assert_unhandled_case` aborted the Checked
   build where Release would return `X_STATUS_INVALID_PARAMETER_2` (which the
   guest handles fine). All three unhandled arms now XELOGW + return.

**Result: RB3DX boots headless into song GAMEPLAY.** With
`--protect_zero=false --rb3dx_skip_calibration --rb3dx_ui_probe` and a plain
timed A-press script, the flow runs `intro_movie → splash → dx_welcome_screen
→ dx_settings_error_screen → main_hub_screen → song_select_screen →
part_difficulty_screen → preloading_screen → tv3_d_screen → game_screen`, and
`game_screen` holds for the rest of a 360 s run: ~59 fps (21,177 VdSwaps),
recovered-fault count flat at 63 (all the §8b benign XMA MMIO writes), XMA
audio contexts streaming, zero aborts (probe: `/tmp/rb3-probe4`). `dx_welcome`
and `dx_settings_error` each dismiss with A (the settings error is the known
graceful dx_playlist/values absence).

**The §8b "main_hub_panel load stall" is REFUTED as a gate.** The probe still
reports `mLoader=NULL mLoaded=0` for `main_hub_panel`, yet navigation
proceeds through it into gameplay — the stalled-looking panel state was the
*aftermath of the crashed loader worker*, and in the July binaries the same
underlying thread crash (or its shallower ancestors fixed in `ef5025af9`)
killed the process before the flow could advance. The `XamAlloc
unk=268435456` spam remains, harmless as suspected.

DC3 non-regression for both commits: verified 2026-08-24 (identical boot
frontier to the session-52 baseline — CRT + XapiInitProcess complete, main()
runs, AsyncFile::Read stall unchanged, no new traps/aborts).

Still open for the original same-instrument goal: two-pad join + instrument
select scripting (the P2 join presses in the old script fire during
dx_welcome and are wasted), and video-out capture for pixel evidence (runs
above are NullGPU).

## 8d. 2026-08-25 — Vulkan pixel-proof + TWO-PLAYER gameplay; the one-gamepad slot model

**Vulkan works.** The July NVIDIA kernel/userspace mismatch is gone (610.43.3
both sides); `--gpu=vulkan --dump_frames_path=<dir>` renders and captures
correctly on the RTX 3090. Pixel evidence (all fully legible): the CHOOSE
INSTRUMENT part-select card, CHOOSE DIFFICULTY with per-instrument icons, the
main_hub 3D venue with the RB3 logo and menu stack, and in-song gameplay —
vocals HUD for "Du Hast" (Rammstein), guitar fret highway under the RB3
DELUXE logo, and finally a TWO-TRACK drums+second-player stage. Probes:
`/tmp/rb3-vk1`, `/tmp/rb3-2pad2`, `/tmp/rb3-2pad9`.

**Why a second scripted pad could never join (five probe passes):** the input
arm was healthy end-to-end — pad-1 keystrokes delivered per-user, guest
`gJoypadData[1]` conn=1 with a bound LocalUser, held buttons visible in guest
memory. The gate was RB3's slot model: `OvershellPanel::RefreshJoinableUsers`
assigns each candidate to a slot accepting its ConnectedControllerType, and
only ONE overshell slot accepts plain gamepads. Probe evidence: pre-join both
users listed in slot2 (npot=2, both kMetaJoinOK); after P1's gamepad takes
that slot, every join list is empty forever. Vanilla one-instrument-per-slot
design, not an emulator bug. Two prerequisites are real, though:
`--local_user_count=2` (else the join dies at XamShowSigninUI), and keeping
the presses out of dialogs (`HasActiveDialogEvent` eats overshell buttons
while the dx settings-error dialog is up).

**Lever (b831e2e23): `--scripted_pad_subtypes=1,8`** — the nop HID driver now
reports a per-pad XINPUT_DEVSUBTYPE; subtype 8 routes pad 1 through
`SetupHXDrums` (guest type 9, standard drums), it lands in the DRUMS slot's
join list, and START@1 joins it: the slot's state object advances, the user
leaves the join list, and gameplay renders TWO track highways (drum lanes +
kick bar on the left). New probe instrumentation under `--rb3dx_ui_probe`:
per-sample `gJoypadData[0..3]` (base 0x82CCB2C8 stride 0xD4, decoded from
`JoypadGetPadData` @0x82524998 = RB3E PORT_JOYPADGETPADDATA) and per-slot
overshell join lists (mSlots auto-located; offsets from rb3-xenon
OvershellSlot.h).

**New frontier — song playback never starts.** Every headless in-song run
(vocals, guitar, two-player) holds game_screen at position 0:00: score 0,
static gems, XMA contexts resetting. Suspect the offline
`NetSession::StartGame → Poll → EnterInGameState → SyncStartGameMsg →
Game::Go → MasterAudio::Play` chain — exactly the chain the RB3 native port
had to shim offline (`rb3/native/src/rb3_netsession_native.cpp` V3 notes: the
sync_audio_net_panel waits on mState 4→5). Next lever: probe
`SyncGameStartPanel`/`NetSession::mGameState` at song start under xenia.

**Same-instrument A/B status:** the SI feature was meanwhile proven ON REAL
HARDWARE (RB3Enhanced `docs/plans/same-instrument-live-diagnosis.md`: live
song, two watchers on track 2, second claimant got a private cloned DB), so
the Xenia A/B is corroboration now, not the primary proof. The in-tree
`--si_load_dll` harness carries hook VAs from the older Phase-2 DLL layout;
the current from-source DLL moved them (map addendum in that doc) — refresh
before any Xenia SI run.

---

## 9. Related docs

Inside this wiki (`docs/jit-fault-wiki/`):

- [INDEX.md](INDEX.md) — wiki overview + per-phase status banners
- [00-source-map.md](00-source-map.md) … [05-fork-divergence.md](05-fork-divergence.md) — the `0x100000000` investigation (source map, symptoms, address translation, guest disassembly, upstream/canary, fork divergence)
- [06-root-cause.md](06-root-cause.md) / [07-fix-and-verification.md](07-fix-and-verification.md) — the guest-SEH gap root cause + the shipped zero-page fix (§4.A)
- [08-boot-to-menu.md](08-boot-to-menu.md) — the clean-TU5 boot-to-menu phase (App::Run flow-quit; §4.B context)
- [09-rb3dx-title-to-menu.md](09-rb3dx-title-to-menu.md) — the RB3DX title wedge / OOM (§4.C)
- [CRASH-REPORT-main-hub-oom.md](CRASH-REPORT-main-hub-oom.md) — self-contained OOM crash report + Fable root-cause (§4.C)
- [BRIEF-main-hub-load-stall.md](BRIEF-main-hub-load-stall.md) — the splash/main_hub stall brief + investigation + `--rb3dx_offline_join` implementation (§4.D)

Outside this wiki:

- `../rb3-bringup-notes.md` — RB3 bring-up history on this fork
- `../rb3-same-instrument-verify.md` — the two-controller A/B harness this work unblocks
- `/home/free/code/milohax/rb3/native/src/rb3_netsession_native.cpp` — the RB3 native-port NetSession shim (the §4.D oracle)
- `/home/free/code/milohax/rb3-xenon/` — RB3 decomp (symbols `config/45410914/symbols.txt`; Ghidra service `http://ghidra.local:8001/mcp`) used for address/signature resolution
- same-instrument patch + deliverables: `/home/free/code/milohax/rb3-xenon/_tu5probe/clean/`
