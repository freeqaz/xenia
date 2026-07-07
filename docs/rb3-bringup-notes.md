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
