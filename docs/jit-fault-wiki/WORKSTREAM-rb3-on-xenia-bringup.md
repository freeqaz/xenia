# Work stream — bringing Rock Band 3 up on the Xenia fork (headless, Linux)

**Scope.** A multi-day debugging campaign to make Rock Band 3 boot far enough on
the Xenia Xbox-360 emulator fork (`/home/free/code/milohax/xenia`, branch
`headless-vulkan-linux`) to verify the *same-instrument* mod (multiple players on
one instrument) with a headless two-controller A/B. The mod itself was already
built and byte-verified; this work stream is about the **emulator + boot path**,
not the patch.

**Status at time of writing (2026-07-08).** Three distinct, load-bearing bugs
found and fixed; RB3 now boots **deterministically** from cold start through the
title/splash. One blocker remains — the game sits on `splash_screen` and never
reaches `main_hub`. Root cause of that last gate is still open (the leading
hypothesis was found and *refuted* with direct instrumentation; see §4.D).

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
| D | Splash never advances to `main_hub` | `splash_screen` (idle) | **Open.** Leading hypothesis (NetSession online-join stall) **refuted for this build**; real gate is *upstream* of the join, in the splash `Confirm → StartOvershell` step | **OPEN** — `--rb3dx_offline_join` built but inactive |

Each fix peeled off one layer and revealed the next: **boot → title → splash**.

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

**Current understanding.** The gate is **upstream of the join**, in the splash's
own `Entered → Confirm → ActivateSaveLoad → StartOvershell → AddUser` chain. A/START
input demonstrably reaches the guest (`XamInputGetState` shows user-0 `0x1000`/
`0x0010`) but the splash sub-state does not advance. Leading candidates: (a) RB3DX
confirm needs an *active profile sign-in* (`dialog_need_signin_screen` /
`XamShowSigninUI`), not just `SigninState==1`; (b) a `saveload_mgr is_idle` poll
that never completes headless; (c) the confirm landing on a different pad/user-slot
than the overshell watches. Full detail: [BRIEF-main-hub-load-stall.md](BRIEF-main-hub-load-stall.md).

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
| `0c0f291b4` | `--rb3dx_alloc_probe` diagnostics, default-off, DC3-inert |
| `5e118d415` | Crash-report doc |
| (uncommitted) | `--rb3dx_offline_join` + `--rb3dx_ui_probe` in `emulator.cc` — correct but inactive for the current gate; held for review |

All emulator behavior changes are cvar-gated and/or title-gated (`0x45410914`) and
default-off or default-inert; **DC3 non-regression verified** at each step.

---

## 8. Open frontier + next step

**UPDATE 2026-07-09 — the splash gate is ROOT-CAUSED (investigation #3).** The
pre-join splash step was traced: the gate is the splash's `{saveload_mgr
is_idle}` poll (candidate (b)), and the mechanism is a **fork regression**:
commit `a224a6846` (DC3 headless work) made `xeXamEnumerate` convert
`X_ERROR_NO_MORE_FILES` → `SUCCESS`+0 items on BOTH completion paths; RB3's
save-container search `MemcardXbox::FindValidUnit` (100% byte-verified) does a
**synchronous** `while (XEnumerate(...) == 0)` loop that therefore never exits →
`SaveLoadManager` never idles → `attempt_to_add_user`/`AddUser` never run.
Sign-in (a) and pad/user-slot (c) are refuted (no `XamShowSigninUI` call ever;
real bytes accept the nop pad as `kJoypadAnalog`; a **no-input control boot**
stalls identically, and the intro-movie skip proves input consumption). Fix +
task list + DC3-safety matrix: **[PLAN-splash-confirm-gate.md](PLAN-splash-confirm-gate.md)**;
full evidence: [BRIEF-main-hub-load-stall.md](BRIEF-main-hub-load-stall.md) §#3.
The same-instrument two-controller A/B remains staged and ready the moment
`main_hub` → instrument-select is reachable; the patch's hook VAs are
byte-verified for RB3DX and delivered as a title-gated runtime memory patch (no
XEX repack).

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
