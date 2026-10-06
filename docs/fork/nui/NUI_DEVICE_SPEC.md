# Kinect (NUI) HLE device: phase-0 spec, read from the SDK

Phase 0 of `NUI_HLE_DESIGN.md`. Every row below is read from the split listing of DC3's
`debug.xex` (dc3-decomp `build/373307D9/asm/xdk/nuiapi/`, dc3-decomp `main` at `6dcf3957e`),
which statically links NUI 2.0.21173, unless the row says *measured*. Line numbers are of
`nuiskeleton.s` (`sk:`) and `nuiruntime.s` (`rt:`).

`NuipRuntimeState` is the SDK's runtime block (`R` below); `R+0x874` is "skeleton tracking
enabled", `R+0x878` the internal KEVENT, `R+0x888` the referenced title event, `R+0x88C` the
tracking flags, `R+0x7C` "device connected", `R+0x1F10` "title sets tracked skeletons",
`R+0xE8B4/0xE8B8` the two title-chosen tracking ids.

## The skeleton frame

| field | SDK source | facade |
|---|---|---|
| size | `NuiSkeletonGetNextFrame` copies `0xab0` bytes from the ring entry + 0x20 (`sk:876-879`) | `kFrameSize = 0xAB0` |
| stride | `NuipConvertSTSkeletons` steps `r10` by `0x1c0` over 6 bodies (`sk:1039`) and clears `0xa80` (`sk:995-998`) | `0x1C0`; 0x30 + 6 x 0x1C0 = 0xAB0 |
| `liTimeStamp`, `dwFrameNumber` | `NuipPostProcessSkeletonFrame`: `ld r10, 0x50(frame_info)`, `std r10, 0(frame)`; `lwz r8, 0x48(frame_info)`, `stw r8, 8(frame)` (`rt:5346-5351`): the depth frame's own stamp and number. The title takes the delta as ms (`SkeletonUpdate.cpp:228-234`) | guest ms since the frame clock started; frame number counts produced frames |
| detected body | state 1 (POSITION_ONLY), `Position` = the ST body centre, `dwTrackingID` = ST id + `R+0xE8AC` (0 stays 0) (`sk:1009-1035`) | state 1, id, hip as `Position`, joints and joint states zero |
| tracked body | state 2; `0x140` bytes of joints from ST +0, `0x50` bytes of joint states from ST +0x148, `dwQualityFlags` = ST +0x144 (`sk:1055-1064`); seated mode zeroes the ten leg joints (`sk:1065-1130`) | state 2, 20 joints with `w = 1`, joint states from the source, quality flags from the source |
| `dwEnrollmentIndex`, `dwUserIndex` | never written after the `0xa80` clear (`sk:995-998`): **0** | 0 |
| joint `w` | copied from ST; not determinable from this unit | 1.0 |

## Tracking policy

- `NuiSkeletonTrackingEnable` sets `R+0x1F10 = 1` when `flags & 2` (`sk:752-758`) and clears
  both title ids (`sk:778-781`, `stw r25(=0), -0x174c(r11)` = `R+0xE8B4`).
- `NuipPostProcessSkeletonFrame` runs a tracking policy only when `R+0x1F10 == 0` and no system
  UI is up (`rt:5210-5224`): `ST_UpdatePlayerTrackingIDsWithSkeletons_TwoPlayer` or
  `NuipNuiHandsDefaultTrackingPolicy`.
- `NuipNuiHandsDefaultTrackingPolicy` (`rt:4555-4619`) picks the two bodies with the smallest
  `|Position|`, minus 0.3 (`__real@3e99999a`) for a body already in state 2.

**So the design's guess ("before any selection, the first two persons are TRACKED") is wrong
for a title that sets tracked skeletons.** With flag 2 nobody is TRACKED until the title calls
`NuiSkeletonSetTrackedSkeletons`; every body is POSITION_ONLY with a tracking id. DC3 passes
flag 2 (`kinect.dta (title_tracked_skeletons TRUE)`, measured: the facade logs
`NuiSkeletonTrackingEnable(event F8000110, flags 2)`), and its `SkeletonChooser` round-robins
the POSITION_ONLY ids and then calls `SetTrackedSkeletons` (measured: `(-1, -1)`, then
`(5, -1)` for the constant pose's id 5).

## HRESULTs

| function | condition | HRESULT | where |
|---|---|---|---|
| `NuiInitialize` | flag `0x200` | `0x80070057` E_INVALIDARG | `rt:9753-9762` |
| `NuiInitialize` | **sensor absent** | **`0x8301000D`** | *measured* (probe below) |
| `NuiSkeletonTrackingEnable` | skeleton not initialised (`R+0` flags) | `0x83010005` | `sk:696-706` |
| | `flags & ~7` | E_INVALIDARG | `sk:708-715` |
| | already enabled | `0x800704DF` | `sk:767-768` |
| | event handle not an event | E_INVALIDARG | `sk:733-742` |
| | success | S_OK; both events set unless `flags & 1` (`NuipSetSkeletonFrameEvents(0)`, `sk:326-370`) | `sk:743-765` |
| `NuiSkeletonTrackingDisable` | always | S_OK; when enabled, sets both events and dereferences the title event | `sk:120-167` |
| `NuiSkeletonSetTrackedSkeletons` | `ids == NULL` | E_INVALIDARG | `sk:235-240` |
| | tracking not enabled | `0x83010002` | `sk:243-250` |
| | title did not ask to choose (`R+0x1F10 == 0`) | `0x83010005` | `sk:252-258` |
| | system UI up (`R+0x84`) | `0x8301000B` | `sk:263-266, 296-302` |
| | success | S_OK, ids stored (id - base, 0 stays 0) | `sk:267-292` |
| `NuiSkeletonGetNextFrame` | `frame == NULL` | E_INVALIDARG | `sk:811-817` |
| | tracking not enabled | `0x83010002` | `sk:827-829, 941-942` |
| | device not connected (`R+0x7C == 0`) | `0x8007048F` | `sk:830-832, 937-938` |
| | wait on `R+0x878` times out (ms; `-1` or > 8000 capped to 8000, 60000 by a runtime flag, unless `flags & 1`) | `0x8000000A` E_PENDING, events **not** reset | `sk:833-866, 934-935` |
| | ring entry status 1 | copy 0xAB0, entry status 0, S_OK | `sk:867-895` |
| | entry status 2 | `0x8301000B` | `sk:907-914` |
| | else, `flags & 1` | `0x8007048F` | `sk:916-925` |
| | else | `0x83010001` | `sk:926-932` |
| | every result except E_PENDING | `KeResetEvent` title event and `R+0x878` | `sk:946-957` |

The other 50 entries keep their legacy return values in phase 1 (design §5); their SDK
semantics are phase 3.

## Kernel/XAM surface

- `XamXStudioRequest` (`rt:941-968` `NuipXStudioPsCamDeviceRequest`, `rt:4036-4063`
  `NuipXStudioAttach`; also `sk:197-227` `NuipSkeletonCaptureConsumption`, opcode `0x1402`): a
  result `>= 0` means "Kinect Studio handled it" and the answer is read back from the request
  block. Xenia returned 0. **Fixed** (`375448c31`): `0x8007048F`, so the SDK falls back to
  `PsCamDeviceRequest`.
- `XamNuiGetDeviceStatus` "present": only `+0xC status = 1` is read by `XNuiGetHardwareStatus`
  (`xapilibi/xnui.s:137-149`); the other fields are not read anywhere in DC3's image, so zero
  is as good as any value. No change.

## Phase-0 probe: the real SDK, sensor absent

`--stub_nui_functions=false --nui_device_present=false`, binary `375448c31` (XStudio fixed, no
facade), S1 command, one run (`/home/free/tmp/nui-hle-runs/p0-probe-absent/`):

- the SDK's own `NuiInitialize` returns **`0x8301000D`** within 1.5 s of boot;
- the game MILO_FAILs on the main thread: tripwire `TAINTED: main-thread Debug::Fail
  mFailThreadMsg=00000000 'NuiInitialize failed (0x8301000d)'`, `mFailing` latched at 3.5 s;
- the run never reaches the title screen (boot_hang_or_no_title).

That is the honest "Kinect unplugged" answer for DC3: the title requires the sensor. The facade
returns the same code when `--nui_device_present=false` (`kE_NUI_NO_SENSOR_AT_INIT`), so an
absent sensor looks the same with and without the HLE.
