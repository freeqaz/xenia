# Rock Band 3 (Xbox 360, TU5) bring-up notes

Goal: boot retail RB3 (patched TU5 XEX + real ARK content) further toward
gameplay on this DC3-tuned Xenia fork, using **general, upstream-correct**
emulator fixes only (no RB3-specific hacks), without regressing DC3.

Build: `make -C build config=checked_linux xenia-headless`
(→ `build/bin/Linux/Checked/xenia-headless`).

RB3 repro:
```
timeout 120 ./build/bin/Linux/Checked/xenia-headless \
  --target=/tmp/rb3tu5boot/default.xex --headless_timeout_ms=60000 2>&1 | tee /tmp/rb3tu5.log
```
DC3 regression:
```
timeout 90 ./build/bin/Linux/Checked/xenia-headless \
  --target="/srv/torrents/games/arbys/Dance Central 3/default.xex" --headless_timeout_ms=45000
```

## Changes

### 1. Implement `XFileFsDeviceInformation` in `NtQueryVolumeInformationFile`
- `src/xenia/kernel/info/volume.h` — added `X_FILE_FS_DEVICE_INFORMATION`
  (`device_type` + `characteristics`, 8 bytes, big-endian) matching the NT
  `FILE_FS_DEVICE_INFORMATION` layout, plus `X_FILE_DEVICE_TYPE` and
  `X_FILE_DEVICE_CHARACTERISTICS` enums (standard winioctl/ntddk values).
- `src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc` (~line 341) — replaced the
  `assert_always()` fall-through with a real handler.

Why: RB3 opens `\Device\Harddisk0\partition0` and queries its FS device info.
This fork never implemented the class, so the `default: assert_always()` case
core-dumped. Fix reports `device_type = FILE_DEVICE_UNKNOWN` (matches
mainline/canary Xenia — a concrete DISK/CD_ROM type can push some titles down
HDD-cache or disc-swap branches the VFS can't honor). `characteristics` is
derived from `device->is_read_only()`: a read-only mount reports
`FILE_READ_ONLY_DEVICE | FILE_REMOVABLE_MEDIA` (disc/STFS media), a writable
mount reports 0 (fixed HDD). `NullDevice` (the HDD partition RB3 queries) has
`is_read_only()==false`, so it correctly reports as a writable fixed disk.

Reference: canary `xboxkrnl_io_info.cc` `XFileFsDeviceInformation` case
(FILE_DEVICE_UNKNOWN stub); we additionally fill characteristics from the
device's real writability, which is faithful 360 behavior.

### 2. Don't `assert_always()` when a title opens a read-only file for write
- `src/xenia/vfs/virtual_file_system.cc` (~line 246, `OpenFile`).

Why: RB3 retail opens dev-only debug files on the read-only `game:` partition
with write dispositions (`game:\dx_playlist.dta` kOverwriteIf, `log.dta`,
`dx_event_config.dta`). The code already handles this correctly by downgrading
the request to read-only access and continuing (upstream's long-standing data
path); the `assert_always()` immediately above it is only a debug flag for an
expected, already-handled condition and halts Checked builds on a benign case.
Softened to a `XELOGW` while preserving the exact access-downgrade behavior.
DC3 never reaches this path (0 hits in its boot), so no behavior change for DC3.

## Result

RB3: **no longer crashes.** Was core-dumping at the volume-info assert after
~11k GPU draws; now runs to the headless timeout in a live render loop —
~2.18M draws / 2.29M shader loads / ~3307 frame swaps in 60s (~55fps) with 1454
distinct back-buffer addresses (real double/triple-buffered, moving content). It
is past boot into the intro/attract/menu render loop (seen loading
`game:\main_background.bik` for the menu background).

DC3 regression: **still boots, no regression.** 0 asserts/aborts, reaches its
normal render loop (355 swaps, draw #43000 in 45s), and hits 0 of the new
write-downgrade path. Both fixes are additive/inert for DC3 by construction.

## Remaining non-fatal items (not blockers to the render loop)

- `undefined extern call to 82C4C2BC XeKeysConsolePrivateKeySign` (1x) and
  `82C4C6EC IoDismountVolumeByFileHandle` (2x) — unresolved kernel exports;
  currently log-and-continue, not fatal. Would matter for save/profile signing
  and volume unmount, not for reaching the menu.
- `NtCreateFile FAILED: game:\main_background.bik` — attract-video asset missing
  from the staged content dir (content issue, not emulator).
- `ResolvePath(gd:\dev_hdd0\game\blus30463\usrdir) failed` — a stray path lookup;
  non-fatal.

Next step toward gameplay would be driving input / verifying the menu renders
via a non-null GPU backend, and implementing the two unresolved externs above if
a later stage (profile/save) needs them.

---

# Session 2 (2026-07-07): XamUser fake local profile + real crash localization

## What was implemented (Xenia kernel)

1. **`xam_user.cc` — real multi-user fake local profile.** The XamUser sign-in
   APIs were already implemented for a single hard-coded user 0. Generalized them
   into a configurable fake local-profile model:
   - New cvar **`--local_user_count`** (default 1, clamped 1-4). Users beyond
     index 0 are *synthesized* (stable offline XUID `0xE000...+idx`, gamertag
     `Player{N}`) purely inside the sign-in query APIs, without touching the
     content/`UserProfile` plumbing — so single-user titles (and DC3) are byte-
     identical at the default, while local multiplayer titles observe the extra
     signed-in controllers.
   - `XamUserGetSigninState / GetXUID / GetName / GetGamerTag / GetSigninInfo`,
     `XamUserCheckPrivilege` (now **grants** for signed-in local users, was
     deny-all), `XamUserAreUsersFriends`, `XamUserContentRestriction*`,
     `XamUserGetMembershipTier` all honor `IsLocalUserSignedIn(index)`.
   - `XamUserGetXUID` now reports a local user as *offline-only* (mask bit 1),
     consistent with `signin_state == SignedInLocally`.
   - Runtime-verified: with `--local_user_count=2`, RB3 queries all four indices
     and sees `user 0 -> 1`, `user 1 -> 1`, `user 2/3 -> 0`.
2. **`user_profile.cc` — offline XUID.** User 0's placeholder XUID
   `0xB13EBABEBABEBABE` (invalid top nibble) → `0xE00000000000BABE` (offline
   form, top nibble 0xE), and name `User` → `Player1`. Mask check
   `0x00C0000000000000` stays clear. Consistent with a signed-in-locally profile.
3. **`xboxkrnl_io.cc` — `IoDismountVolumeByFileHandle`** minimal success stub
   (was an "undefined extern call"; RB3 calls it managing its cache volume).
4. **`xboxkrnl_crypt.cc` — `XeKeysConsolePrivateKeySign`** log-and-continue
   success stub (does NOT touch the output buffer — the true signature size is
   uncertain from the ABI and writing a wrong length could corrupt guest memory).

Both previously-unresolved externs now resolve cleanly (no `!!` / "undefined
extern call" in the boot log).

## DC3 regression: PASS (measured, not assumed)

DC3 boots identically with and without these changes. Its boot hits a pre-existing
crash at guest `0x82311A94` (fault `0x100000000`, single SIGSEGV) — **reproduced
byte-for-byte with the code reverted to HEAD**, so it is NOT caused by this work.
At the default `local_user_count=1`, only the user-0 identity changes apply.

## The `main_hub` crash: XamUser hypothesis REFUTED; real cause localized

The prior session hypothesized the `main_hub` heap crash was caused by
unimplemented XamUser sign-in APIs. **This is refuted:** with the full multi-user
implementation above, the patched RB3 **Deluxe** build crashes identically (heap-
allocator null-walk, fault `-8`, in the game's own MSL allocator at `0x8284xxxx`).

Two distinct facts were established:

- **The two "RB3" XEXes are different builds.** `/srv/torrents/games/arbys/rb3/`
  contains both `default.xex` (sha `6639ce25…`, **RB3 Deluxe** — contains the
  `rbdxcache` string and `songs/updates/deluxe/…`) and `default_vanilla.xex`
  (sha `cd472d07…`, **true retail RB3**, no `rbdxcache`). The prior session's
  "vanilla control" was actually the Deluxe build. The same-instrument **patch
  targets Deluxe TU5**.
- **Deluxe's crash is in the RB3-Deluxe `rbdxcache` path, not XamUser.** The
  crash is immediately preceded by
  `xeXamContentCreate root='rbdxcache' flags=0x14` →
  `NtCreateFile rbdxcache:\rbdxcache disp=1` → an (unlogged) `NtReadFile` on that
  handle returning `STATUS_ACCESS_VIOLATION` (`xeRtlNtStatusToDosError C0000005`)
  → RB3-Deluxe's cache-init then corrupts the MSL heap free-list → the next
  alloc/free walks a nulled node and faults. Nondeterministic crash *address*
  (heap state), deterministic *trigger* (loading `main_hub` after the splash).
- **True retail (`default_vanilla.xex`) boots much further** — past the logo,
  photosensitivity, and the shattered-glass splash to the **ESRB /
  "This game uses an autosave system" screen** (frame 700) — then hits a
  *deterministic* crash at `0x8226045C` (fault `0x100000000`), reproducible
  **with no input**, at the ESRB→main-menu transition. This is the same
  `0x100000000` fault family as DC3's pre-existing crash. (Retail is a third
  build — its VAs don't match the rb3-xenon `band.exe` disassembly base
  `4704202f`, so instruction-level disassembly of this XEX is not yet available.)

Net: reaching the RB3 overshell is blocked by **two independent, pre-existing
emulator/game crashes** — the Deluxe `rbdxcache` heap corruption (blocks the
*patched* build) and the retail/DC3 `0x100000000` fault (blocks the *unpatched*
retail build) — **neither related to the XamUser sign-in APIs.**

## Reproduce

```bash
# retail (boots furthest, to ESRB/autosave), no input, deterministic 0x8226045C:
mkdir -p /tmp/rb3retailboot && cd /tmp/rb3retailboot && \
  cp /srv/torrents/games/arbys/rb3/default_vanilla.xex default.xex && \
  for f in gen AvatarAwards nxeart charnames.zbm; do \
    ln -sf /srv/torrents/games/arbys/rb3/$f $f; done
build/bin/Linux/Checked/xenia-headless --target=/tmp/rb3retailboot/default.xex \
  --gpu=vulkan --local_user_count=2 \
  --dump_frames_path=rb3-verify/frames/retail --headless_timeout_ms=80000
```
