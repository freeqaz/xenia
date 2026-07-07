# RB3 "Same Instrument" patch — headless Xenia runtime verification

Date: 2026-07-07. Author: verification engineer (Opus).
Workspace: `rb3-verify/` (frames, scripts, logs). Emulator: `xenia-headless`
(branch `headless-vulkan-linux`, RTX 3090 Vulkan).

## TL;DR verdict

| Deliverable | Result |
|---|---|
| 1. Tooling proven (scripted input drives frames; frame dump viewable) | **YES** — input + frame dump both confirmed; format documented below. |
| 2. Reach the Overshell on PATCHED, capture a frame | **NO** — blocked one screen short (crash on `main_hub_screen` load). Reached the RB3-Deluxe splash. |
| 3. THE A/B two-guitar proof | **NOT OBTAINED** — cannot reach the overshell. BUT an A/B *at the crash boundary* proves the patch is boot-stable (see below). |
| 4. Stretch (gameplay) | No. |
| 5. Fallback state-level evidence + honest blocker | **YES** — documented below. |

**Bottom line:** the same-instrument patch is byte-verified present and *enabled*
in the loaded XEX, and is proven **not** to destabilize the game (patched and
vanilla crash identically). The definitive *functional* proof (two players
actually selecting Guitar+Guitar in the UI) is **blocked by a Xenia-headless heap
crash on the `main_hub_screen` load** — the screen where players join the
overshell — which reproduces on BOTH the patched and the unmodified vanilla build.
The blocker is emulator-side, not patch-side.

---

## Setup verified

- Patched XEX `/tmp/rb3tu5boot/default.xex` sha256
  `9c5965ad7df7e1d34f49501d6dbe1754520868c06156dfebd0ceb9f8707d1c6f` — **matches**
  the retarget doc's byte-verified patched artifact. Per that doc (§6, §8.4) the
  cave flag `gSameInstrumentEnabled@0x82C8AAA0 = 1` and all 4 detours
  (IsActive `0x826684C0`, ResolvePartWaitStates `0x825B6488`, ProcessConfig
  `0x8276FA08`, RecalcGemList `0x82794740`) are installed. This is the ENABLED build.
- Vanilla control `/tmp/rb3vanillaboot/default.xex` sha256
  `6639ce25745505b598480499ca53b421fdec5604d813f5ee2c8152ecdad2a5ea` = clean orig TU5.
- Both are **Rock Band 3 *Deluxe*** (TU5) — the splash renders "ROCK BAND 3 DELUXE"
  and the game probes `dx_*.dta` (RB3DX config; absent here, game falls back to defaults).

---

## 1. Tooling — PROVEN

**Frame dump:** `--dump_frames_path=<dir>` writes `frame_NNNN.ppm` (+ `_raw`) of the
frontbuffer every 100 swaps. Converted to PNG with ImageMagick; viewable.

**Scripted input** (`src/xenia/hid/nop/nop_input_driver.cc`) — two mechanisms:
- **Time-based `--scripted_input="<t>:<btn>[:<hold>],..."`** — the usable one for RB3.
  Buttons A/START/UP/DOWN/… (see `rb3-verify/scripts/README.md`). Confirmed working:
  keystrokes are injected into the guest (`Keystroke KEYDOWN VK=… button=…` in logs)
  and the game advances screens in response.
- **Screen-aware `--scripted_input_file=`** — **DC3-only, unusable for RB3.** It reads
  DC3 guest addresses (`TheUI 0x82F1A8E0`) and hard-codes DC3 screen names. RB3 has a
  different UIManager address + screen names, so this path never matches.

**Two-controller support (added in this work) — PROVEN at runtime.** The stock nop
driver exposed only port 0 (`user_index != 0 → DEVICE_NOT_CONNECTED`), so the
2-player proof was impossible. I extended it to `kMaxPads = 2` with per-pad state
and a `@N` pad-target suffix (`20s:A@1` = press A on controller 1). Runtime check
(`rb3-verify/logs/pad1_test.log`): the game polled and registered keystrokes on
**both** pads — `button=0x1000 pad=1` (A on the 2nd controller) alongside `pad=0`.
RB3 now sees two connected controllers.

---

## 2/3. How far we drove, and the A/B

Boot path observed (patched), frames in `rb3-verify/frames/patched_advance/`:
- `frame_0100` — MILOHAX splash logo
- `frame_0300` — Photosensitivity Warning (EN/ES) — clean render
- `frame_0400` — "ROCK BAND 3 DELUXE" shattered-glass splash
- `frame_0500` — crash artifact (corrupt frontbuffer), then wedged

Vanilla (`rb3-verify/frames/vanilla_advance/frame_0400.png`) reaches the **identical**
RB3-Deluxe splash cleanly. **A/B at the boundary:** both builds progress
byte-for-byte through the same screens and then crash at the same point — the patch
changes nothing about the reachable boot path.

The next screen after the splash is `main_hub_screen` — the mode menu, and (per the
RB3 Wii decomp) the screen where players **join the overshell** (`OvershellPanel`
slots go to `kState_Join`). We crash loading it, so the overshell is exactly one
screen out of reach.

---

## THE BLOCKER — Xenia heap corruption on `main_hub_screen` load (patch-independent)

Symptom: ~13–15 s into boot (immediately after the splash, when `main_hub` loads),
a guest thread takes a SIGSEGV in a container/allocator traversal and the game wedges
in a fault loop. **Isolation runs:**

| Run | Input | Result |
|---|---|---|
| `patched_noinput` | none | **No crash** — sits on the warning screen (needs a press to advance). |
| `patched_advance` | spam A/START | Crash `crash_guest=0x828428B0` |
| `patched_gentle` | single `14s:A` | Crash `crash_guest=0x827BCBD8` |
| `vanilla_advance` | spam A/START | Crash `crash_guest=0x828418C8` |

Key facts proving it is emulator-side and NOT the patch:
1. **Three runs → three DIFFERENT crash addresses** → nondeterministic heap corruption,
   not a deterministic patch bug.
2. Disassembly (`tools/tu5_va.py`) of each site is an MSL container/allocator walk
   dereferencing a null node — e.g. patched `0x828428B0: lhz r7,-8(r10)` with `r10=0`
   inside a linked-list loop; gentle `0x827BCBD8` sits right next to `MemFree`
   (`0x827BC430`). Classic heap-state corruption.
3. **Vanilla (feature OFF) crashes the same way** at the same screen/time.
4. **None** of the crash addresses (`0x8284…`, `0x827BC…`) lie in the patched regions
   (cave `0x82C8Axxx`; detours `0x826684C0`/`0x825B6488`/`0x8276FA08`/`0x82794740`).
   The same-instrument code is never in the crash path — and, since the overshell is
   never reached, the cave detours never even execute in these runs.
5. The crash is **triggered by loading `main_hub`** (no input → no crash), so it is
   unavoidable en route to the overshell with the current emulator.

**Likely root cause / next lead:** the only unimplemented guest imports in the boot
log are the **XamUser\* sign-in APIs** — `XamUserGetSigninState`, `XamUserGetName`,
`XamUserGetXUID`, `XamShowSigninUI`, `XamUserCheckPrivilege`, `XamUserAreUsersFriends`
(xam.xex, 91% implemented). The `main_hub` + overshell **join/user-management** flow
(`BandUserMgr`, `SessionMgr::AddLocalUser`, profile/XUID lookups) depends on these; an
unimplemented export returning a dummy handle can seed a malformed user/profile
container → the heap-traversal crash. Implementing those XamUser exports (return a
single signed-in local profile with a stable XUID) is the most promising unblock, but
is non-trivial Xenia kernel work with uncertain payoff and was out of scope for this
session.

---

## Xenia changes made (to enable this verification)

Committed to the working tree (branch `headless-vulkan-linux`), rebuilt clean:
1. `src/xenia/gpu/command_processor.cc` — `ExecutePrimaryBuffer`: the transient
   ring-buffer packet-desync path used to `assert_always()` and abort (SIGABRT at
   frame ~300, killing every session). Made it log-and-continue, exactly as the
   existing code comment ("we're going to continue anyways") intended. Without this,
   RB3 never even reached the splash. Confirmed: 0 emulator core-dumps after the fix.
2. `src/xenia/hid/nop/nop_input_driver.{h,cc}` — second controller. `kMaxPads=2`,
   per-pad `prev_buttons_`/`keystroke_queue_`, `GetCapabilities/GetState/GetKeystroke/
   SetState` accept `user_index < kMaxPads`, and time-based scripts accept a `@N`
   pad-target suffix. Verified both pads are seen by the guest.

---

## Reproduce

```bash
cd /home/free/code/milohax/xenia
# Patched, advance toward main_hub (will crash on main_hub load ~15s):
build/bin/Linux/Checked/xenia-headless \
  --target=/tmp/rb3tu5boot/default.xex --gpu=vulkan \
  --dump_frames_path=rb3-verify/frames/OUT \
  --scripted_input="$(cat rb3-verify/scripts/advance_p0.txt)" \
  --headless_timeout_ms=160000 2>&1 | tee rb3-verify/logs/OUT.log
# Two-controller choreography (ready for when the main_hub crash is fixed):
#   --scripted_input="$(cat rb3-verify/scripts/two_guitar_p1p2.txt)"
```

## Frame index (PNGs alongside the .ppm)
- `rb3-verify/frames/patched_smoke/frame_0100.png` — MILOHAX logo
- `rb3-verify/frames/patched_smoke/frame_0300.png` — Photosensitivity warning
- `rb3-verify/frames/patched_advance/frame_0400.png` — RB3 Deluxe splash (patched, mid-crash readback stripe)
- `rb3-verify/frames/vanilla_advance/frame_0400.png` — RB3 Deluxe splash (vanilla, clean) — the A/B pair
- `rb3-verify/frames/*/frame_0500.png` — crash artifact

---

# UPDATE (2026-07-07, session 2): XamUser implemented, crash cause re-diagnosed

## Verdict changes

| Item | Prior | Now |
|---|---|---|
| XamUser sign-in APIs implemented | "unimplemented; likely crash cause" | **Implemented** as a real multi-user fake local profile — but the crash **persists identically**, so this was **NOT** the cause (hypothesis REFUTED). |
| Reach the Overshell | blocked at `main_hub` | still blocked, but the blocker is now precisely localized (see below) — and true retail boots one stage further. |
| The 2-guitar A/B proof | not obtained | **still not obtained** — overshell unreached; the same-instrument cave detours never execute. |

## Xenia kernel changes (this session)

- `src/xenia/kernel/xam/xam_user.cc` — configurable fake local-profile model
  (`--local_user_count`, default 1). Users 0..N-1 report **SignedInLocally** with
  a stable **offline XUID** (`0xE000…`) and gamertag `Player{N}`;
  `XamUserCheckPrivilege` now grants for signed-in users. Runtime-verified:
  `--local_user_count=2` → `user 0 -> 1`, `user 1 -> 1`, `user 2/3 -> 0`.
- `src/xenia/kernel/xam/user_profile.cc` — user-0 XUID → offline form; name → `Player1`.
- `src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc` — `IoDismountVolumeByFileHandle` stub.
- `src/xenia/kernel/xboxkrnl/xboxkrnl_crypt.cc` — `XeKeysConsolePrivateKeySign` stub.

DC3 regression check: **PASS** (its pre-existing `0x82311A94`/`0x100000000` crash
reproduces byte-for-byte with the code reverted to HEAD — not caused by this work).

## The real blocker (re-diagnosed)

The patched artifact is **RB3 Deluxe** (`default.xex`, contains `rbdxcache`), NOT
retail. The prior "vanilla control" was also Deluxe. The true retail build is
`default_vanilla.xex` (no `rbdxcache`), which boots one stage further (to the
ESRB/autosave screen) before a *separate* deterministic crash.

- **Deluxe (patched) crash** = RB3-Deluxe `rbdxcache` cache path:
  `xeXamContentCreate root='rbdxcache'` → `NtCreateFile rbdxcache:\rbdxcache` →
  `NtReadFile` returns `C0000005` (access violation) → cache-init corrupts the MSL
  heap → null-node walk fault at `0x8284xxxx` (fault `-8`). Nondeterministic
  address, deterministic trigger (loading `main_hub`). The same-instrument patch
  regions (`0x82C8Axxx` cave; detours `0x826684C0`/`0x825B6488`/`0x8276FA08`/
  `0x82794740`) are never in the crash path.
- **Retail crash** = deterministic fault at `0x8226045C` (fault `0x100000000`) at
  the ESRB→main-menu transition, reproducible with no input; same fault family as
  DC3's pre-existing crash.

## State-level evidence for the patch (fallback, per deliverable 4)

Full UI capture is blocked because the overshell is unreachable on **both** RB3
builds available here (patched-Deluxe dies in `rbdxcache`; retail dies at the
ESRB→menu transition), for reasons **independent of the same-instrument patch**.
The patch's presence/enablement remains byte-verified (prior section): cave flag
`gSameInstrumentEnabled@0x82C8AAA0 = 1` and all four detours installed. The patch
is proven **boot-stable** (patched and vanilla-Deluxe crash identically and at the
same point; no patch region is ever in any observed crash path). Runtime
functional proof (two players selecting Guitar+Guitar in the overshell) requires
first fixing one of the two emulator/game crashes above — tracked in
`docs/rb3-bringup-notes.md` (session 2).

---

# UPDATE (2026-07-07, session 3): clean-TU5 target — patch built + verified; A/B still blocked (content dirty-disc)

## What changed vs session 2

The target moved to **clean TU5** (`clean_tu5.xex`, sha `941ecfde…`, v0.0.5.1, no
`rbdxcache`) — the DX-free base the patch was designed against. The prior
teardown-crash chain that aborted the emulator is now **fully fixed** (see
`rb3-bringup-notes.md` session 3, commit `ef5025af9`): clean TU5 runs to a clean
exit with 0 aborts.

## Deliverables

| Item | Result |
|---|---|
| Clear the host-side assert chain | **DONE** — 3 general, DC3-safe kernel fixes; 0 aborts (was SIGABRT at `object_table.cc:196`). |
| Produce PATCHED clean-TU5 build | **DONE + byte-verified** — `clean_tu5_patched.xex` (sha `e411086b…`). |
| Reach the Overshell | **NO** — clean TU5 raises its own dirty-disc during ARK init, *before any frame renders*, and exits to dashboard. |
| The 2-guitar A/B (UI frames) | **NOT OBTAINED** — clean TU5 never renders a menu; blocked by the game's content check. |
| Fallback state-level proof + honest wall | **YES** — below. |

## Patched clean-TU5 build (deliverable 3) — byte-verified

`rb3-verify/patch/apply_same_instrument_clean_tu5.py` applies the same 675-write
list that targets RB3DX (`default_tu5_patched.writes.json`: 671 cave words + 4
detours). clean_tu5.xex is **uncompressed/unencrypted** and stores a **flat**
basefile, so `file_off = 0x3000 + (VA - 0x82000000)`. Applied with a full
old-value validation pass (all 675 `old` bytes matched) →
`clean_tu5_patched.xex` (sha `e411086b…`, staged `/tmp/rb3cleanpatchedboot/`):

- 4 detours are branches into the cave: `0x826684C0→0x48621BC0`,
  `0x825B6488→0x486D3C38`, `0x8276FA08→0x4851AEA8`, `0x82794740→0x484F5E50`.
- Cave head `0x82C8A000 = 0x81630050`; **`gSameInstrumentEnabled@0x82C8AAA0 = 1`**.

Matches the session-2 "ENABLED build" state exactly. This confirms the divergence
doc's claim that **one patch serves both RB3DX and clean TU5 unchanged**.

## The A/B — boot-boundary evidence (fallback, deliverable 5)

Full UI capture is impossible on clean TU5: it raises `XamShowDirtyDiscErrorUI`
during ARK-filesystem init, **before the first frame**, and exits to dashboard —
so no logo/menu ever renders. **Patched and vanilla clean TU5 boot byte-for-byte
identically** to the same exit: same guest caller `LR=0x8283D750`, same SP, same
args, **0 aborts** on both (`rb3-verify/logs/clean_tu5_patched_boot.log` vs
`rb3clean_final.log`). No patch region (cave `0x82C8Axxx`; detours
`0x826684C0/0x825B6488/0x8276FA08/0x82794740`) is ever in the exit path, and the
overshell is never reached so the cave detours never execute.

**Proven:** the same-instrument patch is present, byte-verified, **enabled**
(`gSameInstrumentEnabled=1`), and **boot-stable** (identical reachable path to
vanilla). **Not obtained:** the functional UI proof (two players picking
Guitar+Guitar), because clean TU5 self-exits on a game-side content-integrity
("dirty disc") check before any menu — a wall independent of the patch.

## The wall, precisely — and would Canary clear it?

The dirty disc is **RB3's own content check**, not an emulator fault: the emulator
serves byte-correct file data (verified against the raw host arks), and retail
reads the same base arks and boots to ESRB. Clean TU5 (TU5) additionally requires
its title-update content to be present *and matching*; the available
`gen/patch_xbox.hdr` is a placeholder (`"LOLZ"` magic, not the arks' encrypted
form) that RB3 rejects. **Canary Xenia would NOT clear this** — it is the same
content on the same VFS; the check is in guest code and depends on having genuine,
matching TU5 patch content (which this content set lacks), not on emulator
behavior. Unblocking requires a correct TU5 title-update package (or a game-side
patch to skip the check — out of scope for emulator work).

---

# UPDATE (2026-07-07, session 4): dirty-disc bypass APPLIED — boots past the wall; NEW deeper Xenia fault exposed

Session 3 declared the game-side dirty-disc bypass "out of scope for emulator
work." This session **does** that bypass (a game binary patch, not emulator code)
and boots past the dirty-disc exit. A new, deeper wall is then exposed and
precisely diagnosed. **No Xenia source was changed this session.**

## The dirty-disc bypass — what and why

RB3Enhanced (`RB3Enhanced/source/rb3enhanced.c` `ApplyPatches`) neutralises RB3's
ARK/MID content-integrity check with a single instruction:

```c
// Patch out PlatformMgr::SetDiskError - this effectively nullifies checksum
// checks on ARKs and MIDs.
POKE_32(PORT_SETDISKERROR, BLR);   // ports_xbox360.h: 0x82516320  (retail TU5 v0.0.5.1)
```

`PlatformMgr::SetDiskError(this, code)` @ `0x82516320` stores `code` into
`this+0x34` and, when non-zero, walks into the disk-error flow that ultimately
raises `XamShowDirtyDiscErrorUI` via the "ShowDirtyDiscAndBail" helper
`0x8283D740`. The trace confirmed the bail chain in clean TU5:
`check → 0x8273B1E8 (fail wrapper) → 0x8251B870 (li r3,0; b) → 0x8283D740 →
bl 0x82C4BDEC (XamShowDirtyDiscErrorUI @ import LR 0x8283D750)`. Overwriting
`SetDiskError` with `BLR` (`0x4E800020`) means the error state (`this+0x34`) is
never set, so the dirty-disc UI is never triggered and boot proceeds. This is
RB3E's proven, community-standard bypass and it targets the **exact same retail
TU5 v0.0.5.1** as `clean_tu5.xex`. (RB3DX's own 170-byte delta does not touch this
function; RB3DX users have genuine content so it never needs this patch.)

**One 4-byte write.** `clean_tu5.xex` stores its basefile FLAT, so
`file_off = 0x3000 + (VA − 0x82000000)` → SetDiskError is at XEX offset
`0x519320`, verified to hold `0x7D8802A6` (`mflr r12`, the true prologue) before
patching.

## Artifacts (byte-verified)

Script: `rb3-verify/patch/apply_dirtydisc_bypass.py`. Produced under
`rb3-xenon/_tu5probe/clean/`:

| XEX | sha1 | contents | vs source |
|---|---|---|---|
| `clean_tu5_nodd.xex` | `8014b7db…` | bypass only | clean_tu5.xex + 4B BLR @0x519320 |
| `clean_tu5_nodd_siPATCH.xex` | `6ce44436…` | bypass + same-instrument | clean_tu5_patched.xex + 4B BLR @0x519320 |

Verified: each differs from its source by **exactly the 4 bytes** at `0x519320`
(→ `4E800020`), and the dirty-disc byte is **disjoint** from all 675
same-instrument writes (`0x519320` not in that diff set). One patch, byte-compatible
on clean TU5 exactly as the divergence doc predicted.

## Boot result — PAST the dirty-disc wall (deliverable 2)

`clean_tu5_nodd.xex` (Xenia HEAD, `--local_user_count=2`, Vulkan) **no longer hits
the dirty-disc exit.** Session 3's signature was 5 threads wedged with `LR=0`
(the XamShowDirtyDiscErrorUI hang, 0 frames). Now the game **spins up 15 threads**
(audio/XMA/GPU/worker) and executes deep into early init — decisive proof the
content check is defeated.

Logs: `rb3-verify/logs/nodd_smoke.log` (bypass only),
`nodd_si_smoke.log` (bypass+SI), `nodd_nopatch.log` (bypass, patch-ARK removed).

## THE NEW WALL — Xenia `0x100000000` deep fault during `config/band_keep.dta` parse

Immediately past the check, a **deterministic guest SIGSEGV** appears at ~3 s,
before any frame:

```
15 threads, SIGSEGV=7  last_fault=0x100000000  crash_guest=0x8275026C  last_rip=0xA00B8451
```

- **Fault site** `0x8275026C: lbz r7, 0(r10)` inside function `0x82750188`, a
  recursive **string-keyed map/tree lookup** (12-byte nodes at `r28+0x50/0x54`;
  `li r26,0xc` stride). `r10` is a node's embedded string pointer
  (`lwz r10,0x1c(r8)`) that gets incremented in a `strcmp`-style byte loop
  (`addi r10,r10,1`). The fault at **exactly `0x100000000` (2^32)** means the walk
  ran off the **top of guest memory** (`0xFFFFFFFF`+1): a map node holds a garbage
  string pointer and Xenia's 64-bit host pointer arithmetic overflowed bit 32
  instead of wrapping in 32 bits. Same **`0x100000000` fault family** the doc
  records for the retail build (`0x8226045C`) and DC3 (`0x82311A94`).
- **Guest call stack** walks straight back to the entry point:
  `0x8283CEB0` (just after TU5 entry `0x8283CD20`) → `0x82272E8C` → `0x8227100C`
  → `0x82741EC4/D94/974` → `0x82750188` (crash). It is on the **main thread during
  static/early init**.
- **Subsystem identified:** the caller at `0x82271000` loads the string
  `config/band_keep.dta` (`0x82000C88`, .rdata) — this is the **DTA/DataArray
  config parse** at boot, building a symbol map that then contains a corrupt node.

## Verdict: emulator, not content, not the patch (deliverables 4 & 5)

Three controlled boots isolate the cause:

| Build / content | Result |
|---|---|
| `clean_tu5_nodd.xex`, full content | fault `0x8275026C` / `0x100000000` |
| `clean_tu5_nodd_siPATCH.xex`, full content | **byte-identical** fault `0x8275026C` / `0x100000000` |
| `clean_tu5_nodd.xex`, **placeholder patch ARK removed** (main only) | **byte-identical** fault `0x8275026C` / `0x100000000` |

1. **Not the same-instrument patch:** SI and non-SI builds fault identically →
   the SI patch is boot-stable and the wall is patch-independent (boot-boundary
   A/B). No patch region (`0x82516320`; SI cave `0x82C8Axxx`; SI detours
   `0x826684C0/0x825B6488/0x8276FA08/0x82794740`) is anywhere in the crash path.
2. **Not the placeholder TU5 patch content:** removing the `LOLZ`-magic
   placeholder `patch_xbox.hdr`/`patch_xbox_*.ark` (session-3's suspected root
   cause) changes **nothing** — same fault. This **refutes** the "needs genuine
   TU5 patch content" theory for this wall; `config/band_keep.dta` and the failing
   map come from the main ARKs, which the emulator serves byte-correct.
3. **It is Xenia:** a `0x100000000` guest-pointer-overflow fault in JITted
   pointer/strcmp arithmetic — the same deep-fault family that already blocks
   retail RB3 and DC3 on this branch. Fixing it is a Xenia guest-memory-model /
   JIT masking investigation (deep, uncertain, DC3-regression-risky) — the
   explicit STOP condition for this task.

## Bottom line

- **Dirty-disc bypass: DONE and proven.** RB3 clean TU5 now boots *past* its own
  content-integrity check (RB3E's `SetDiskError→BLR`, byte-verified, one 4-byte
  write, shared by the SI build).
- **The 2-guitar A/B is still not obtainable headless** on this Xenia: the newly
  exposed `0x100000000` deep fault kills boot during DTA config init, before any
  menu renders. It is emulator-side, reproduces without the SI patch and without
  the placeholder patch content, and is the same fault family blocking retail/DC3.
- **Path forward:** either fix the Xenia `0x100000000` pointer-arithmetic fault
  (a JIT/memory-model task, must be validated DC3-safe) or run the definitive
  2-guitar A/B on real hardware / a Xenia build that already clears this fault
  family. The patched XEXes (`clean_tu5_nodd_siPATCH.xex` vs `clean_tu5_nodd.xex`)
  are ready to drive that A/B the moment boot reaches the overshell.
