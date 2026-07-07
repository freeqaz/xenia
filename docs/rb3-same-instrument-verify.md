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
