# A title-agnostic Kinect (NUI) HLE device for Xenia: design

Status: phases 0-1 built by lane nui-hle (2026-10-02/03); phase 2 (retiring the calib.*,
pause and controller-mode hacks) is under measurement, not landed. §8 "As built" lists what
differs from this design and why. Phases 3+ are still design.
Written 2026-10-02 against:
- xenia `main` at `1f309687c`, read with `git show main:` and `git grep main`;
- dc3-decomp `main` at `a7cf32774`, including its split listings in `build/373307D9/asm/`.

Line numbers are as of those commits. Each `path:line` reference below is to the file on that
tree, unless the reference says otherwise.

---

## 0. Summary

1. **There is no structured data below the SDK.** DC3 statically links the whole Kinect stack:
   - `NUI`, the API and runtime;
   - `ST`, the skeletal tracker itself (Exemplar, running on the GPU and CPU);
   - `NUISP` (speech), `NUIAUD` (audio) and `FITNESS`, all at XDK 2.0.21173.

   The console below that is only:
   - a USB camera behind the kernel export `PsCamDeviceRequest`, carrying raw depth and colour
     transfers;
   - a mic array (`MicDeviceRequest`, `XamVoiceGetMicArray*`);
   - a tilt motor and accelerometer (`XamNuiCamera*`);
   - a small XAM "NUI app" (app `0xFE`, messages `0x2B001`-`0x2B004`).

   Skeletons exist only inside the title.

2. **The device is a hybrid.**
   - **Kernel/XAM layer.** A faithful emulation of the sensor's kernel and XAM surface. It is
     upstreamable, and it is the only layer whose "not connected" answer is honest.
   - **SDK facade.** A per-XDK-version HLE of the NUI SDK's public API, located by signature.
     Detection keys on the XEX static-library header (`NUI 2.0.21173`), never on a title ID or
     an address.
   - **Pose source.** Skeleton frames come from a pluggable source behind the facade: empty,
     constant, tape, or the native port's `DC3_POSE_SOCKET` protocol.

   This is the pattern Cxbx-Reloaded uses for statically linked XDK libraries (OOVPA signature
   tables per library version).

3. **Why not emulate the raw camera.** Feeding synthetic depth through `PsCamDeviceRequest`
   would run the real `ST` tracker. That is XL work, it depends on GPU async-command-buffer
   fidelity, and it would *change the skeleton*. That last point defeats requirement (b): the
   same skeleton sequence must reach native and Xenia. The structured seam is therefore the
   SDK's own public skeleton API.

4. **Today's frames are malformed.** This is the most likely root cause of the calibration hacks.
   `Dc3NuiSequencerExtern`:
   - leaves all 20 per-joint tracking states at 0 (NOT_TRACKED);
   - stamps time in units 1000× too large;
   - uses a 0x1B4 skeleton stride where the real stride is 0x1C0.

   `SkeletonQualityFilter` counts only TRACKED joints. A skeleton with every joint NOT_TRACKED is
   never "confident", so in the shell it is never valid, so `SkeletonChooser` never binds a
   player. That is a code-grounded hypothesis; phase 2 tests it (§5).

5. **What the device retires.** On the original layout:
   - the 56 NUI SDK table entries, `nui.get_next_frame` among them;
   - `skel.wait_33ms` and `skel.is_override_nop`;
   - `speech.grammar_unload`;
   - the 6 `calib.*` ids;
   - `game.pause_for_skeleton_loss` and `seq.controller_mode`;
   - the transitional cvars `stub_nui_functions` and `fake_kinect_data`.

   With the decomp-layout signature variants it also retires the 85 decomp NUI table entries and
   the 13 decomp-pack NUI stubs.

   It does **not** retire the 3 `CXbcImpl` entries, which are SmartGlass/XLRC, not Kinect.

6. **Nobody upstream has done this.** Neither upstream xenia nor xenia-canary has any NUI
   emulation. Upstream reports the sensor as not connected; canary lists `PsCamDeviceRequest`
   as an unimplemented kernel export for four titles. Sources are in §2.7.

---

## 1. Requirements recap

| # | Requirement | Where it is met |
|---|---|---|
| R1 | Emulate the Kinect at the kernel/XAM/NUI-library boundary Xenia already partly models | §3.2 (kernel/XAM layer), §3.3 (SDK facade) |
| R2 | Title-agnostic | detection by XEX static-library version (§3.3.1); no title ID or address in `src/xenia/kernel/nui/`; RB3 has no NUI (§2.6) |
| R3 | Pose source (a): recorded or synthetic stream | `EmptyPoseSource`, `ConstantPoseSource`, `TapePoseSource` (§3.4) |
| R4 | Pose source (b): the native port's DC3_20 protocol, so one skeleton sequence drives both | `SocketPoseSource` speaking the exact v2 wire format (§2.5, §3.4.3) |
| R5 | Fix Xenia rather than mask it | faithful return codes and event semantics, copied from the SDK's own code (§2.3); real kernel handles and events; no guest writes to title globals |
| R6 | S1 still reaches song end with no mFailing latch | definition of done of every phase (§5) |

---

## 2. Findings, with evidence

### 2.1 How DC3 talks to the Kinect

#### 2.1.1 Imports (kernel and XAM)

The import thunks are in `config/373307D9/symbols.txt`:

| import | IAT slot | role |
|---|---|---|
| `XamNuiGetDeviceStatus` | `.rdata:0x820007B8` (symbols.txt:111) | sensor present/absent; `XNuiGetHardwareStatus` reads `+0xC` of the 24-byte struct (`asm/xdk/xapilibi/xnui.s:137-149`) |
| `XamNuiCamera{TiltGetStatus,TiltReportStatus,ElevationStopMovement,ElevationGetAngle,ElevationSetAngle,RememberFloor,TiltSetCallback}` | `0x82000730`-`0x82000748` | tilt motor and floor memory; called only by the SDK's `nuidetroit` unit (`asm/xdk/nuiapi/nuidetroit.s`) |
| `XamNuiIdentityGetSessionId`, `XamUserNuiGetUserIndex`, `XamUserNuiGetEnrollmentIndex`, `XamUserNuiEnableBiometric`, `XamNuiGetDeviceSerialNumber`, `XamRead/WriteBiometricData` | `0x82000758`-`0x820007A8` | identity and biometrics |
| `XamVoiceGetMicArray{AudioEx,UnderrunStatus,Status}` | `0x820007AC`-`0x820007B4` | 4-mic array, used by NUISP and NUIAUD |
| `XamShowNui*UI` (9) and `XamShowNuiTroubleshooterUI` | `0x82000604`-`0x82000624`, `0x8200074C` | system UI; thunked by `asm/xdk/xapilibi/nuiapithunk.s` |
| `XamXStudioRequest` | `0x82000728` (symbols.txt:75) | "Kinect Studio" record/playback hook (§2.2) |
| `PsCamDeviceRequest` | `0x82000974` (symbols.txt:222) | **the camera driver**: depth and colour transfers, control requests, VBlank sync |
| `MicDeviceRequest`, `RmcDeviceRequest` | `0x82000B84`, `0x82000B80` | microphone and remote-control device requests |
| `XMsgInProcessCall` / `XMsgSystemProcessCall` to app `0xFE`, msgs `0x2B001`-`0x2B004` | (via `xapilibi/xnui.s:43-127`) | `XNuiDelayUI`, `XNuiInterpretFrame`, `XNuiTitleInitialize`, `XNuiTitleShutdown` |

#### 2.1.2 Statically linked SDK

The XEX static-library header, parsed from `orig-assets/debug.xex` optional header `0x200FF`,
lists:

```
NUI 2.0.21173   NUIAUD 2.0.21173   NUISP 2.0.21173   ST 2.0.21173
FITNESS 2.0.21173   XMIC 2.0.21173   XBC 2.0.21173   XLRC 2.0.21173 …
```

`ST` is the skeletal tracker. The split confirms it: `config/373307D9/splits.txt` has
- 108 `xdk/nuiapi/*` units,
- 17 `xdk/ST/*` units (`exemplar`, `pipeline`, `preprocessingstage`, `headdetect` …),
- 291 `xdk/nuispeech/*` units,
- 21 `xdk/nuiaudio/*` units.

#### 2.1.3 Game-side call sites

Every NUI entry point the game itself calls (dc3-decomp `src/`, excluding `src/xdk`):

| caller | NUI calls | failure handling (what an HLE must honour) |
|---|---|---|
| `LiveCameraInput` ctor, `src/system/gesture/LiveCameraInput.cpp:629-701` | `NuiInitialize(0x4049 or 0x40004049, -1)`, `NuiAudioCreate`/`RegisterCallbacks`, `NuiSkeletonTrackingEnable(SkeletonUpdate::NewSkeletonEvent(), title_tracked ? 2 : 0)`, `NuiImageStreamOpen` ×2 (COLOR_YUV 640×480; DEPTH_AND_PLAYER_INDEX_IN_COLOR_SPACE 320×240), `NuiCameraSetProperty` | `MILO_ASSERT_FMT(SUCCEEDED…)` on Initialize (641), TrackingEnable (653-656) and both stream opens (658-683). Audio is optional (`if SUCCEEDED`, 646) |
| `LiveCameraInput` dtor, `:704-721` | `CloseHandle(mStreams[i].mHandle)` (712-714), `NuiAudioUnregister/Release`, `NuiShutdown` | **stream handles must be real kernel handles** |
| `LiveCameraInput::PollNewStream`, `:893-927` | `NuiImageStreamGetNextFrame(h, 0, …)`, `NuiImageStreamReleaseFrame` | `E_NUI_DEVICE_NOT_CONNECTED` sets `mConnected=false`; `0x83010001` (no frame data) is counted; success sets `mConnected=true` |
| `LiveCameraInput::SetTrackedSkeletons`, `:1205-1210` (via `GestureMgr::UpdateTrackedSkeletons`, `GestureMgr.cpp:466-480`, from `SkeletonChooser.cpp:509,555,586,588,612`) | `NuiSkeletonSetTrackedSkeletons(DWORD[2])` | -1 means none |
| `SkeletonUpdateThread` / `SkeletonUpdate::Update`, `SkeletonUpdate.cpp:116-133, 227-268` | waits INFINITE on `sNewSkeletonEvent` (manual-reset, `:593`), then `NuiSkeletonGetNextFrame(0, frame)` | non-zero return with `mIsCameraConnected` false: the game synthesises its own stub frame (`StubCameraInput::StubSkeletonFrame`, `:241-262`) |
| `KinectGuideThread`, `src/App.cpp:1395-1430` | `NuiSkeletonTrackingDisable` / `Enable(0,0)` / `Enable(event,2)`; listens for XNotify `0x6001A` and calls `XShowNuiGuideUI` | `MILO_FAIL` on any failure |
| `SpeechMgr`, `src/system/gesture/SpeechMgr.cpp` | `NuiSpeechEnable` (188-222), `Create/Load/Unload/CommitGrammar`, `SetGrammarState/RuleState`, `CreateRule/State/AddWordTransition`, `Start/StopRecognition` (318-339), `SetEventInterest`, `GetEvents` (626-634), `DestroyEvent`, `EmulateRecognition` (118, the debug handler) | `NuiSpeechEnable` failing with anything other than INVALIDARG/DB codes only logs "(no Kinect?)" and leaves `mEnabled=false` (210). `CreateGrammar` asserts success **regardless of `mEnabled`** (145-147; called by `VoiceControlPanel.cpp:216`) |
| `Skeleton::RequestIdentity/Enroll`, `Skeleton.cpp:519-570`; `SkeletonIdentifier::UpdateEnrolledPlayers`, `SkeletonIdentifier.cpp:238-245`; `GestureMgr::SetIdentificationEnabled`, `:407-414` | `NuiIdentityIdentify/Enroll` (callback-bearing), `NuiIdentityGetEnrollmentInformation`, `NuiIdentityAbort` | failure returns `false` (not an assert), except that `E_INVALIDARG` asserts. `GetEnrollmentInformation`'s output is read unconditionally |
| `CameraTilt`, `src/system/gesture/CameraTilt.cpp:83-93, 150-160, 213-223, 276-286, 344, 415` | `NuiCameraAdjustTilt(…, XOVERLAPPED*)`, `NuiCameraElevationSet/GetAngle` | `ERROR_IO_PENDING` / `ERROR_SUCCESS` / `E_INVALIDARG` branches |
| `FitnessFilter.cpp` | `NuiFitness{Start,Pause,Resume,Stop}Tracking`, `GetCurrentFitnessData` | MILO_NOTIFY only |
| `WaveToTurnOnLight.cpp` | `NuiWaveSetEnabled`, `NuiWaveGetGestureOwnerProgress` | — |
| `JointUtl.cpp` | `NuiTransformSkeletonToDepthImage` | inline, pure math (`src/xdk/nui/nuiskeleton.h:78-117`): no HLE needed |
| `Skeleton.cpp` | `NuiTransformMatrixLevel` | pure math: no HLE needed |
| `MoveDir.cpp:1497` | `XNuiDelayUI` (XMsg `0x2B001`) | result ignored |

Today's xenia handles these as follows:
- a 59-entry table at `src/xenia/titles/dc3/dc3_title.cc:272-416`: 56 NUI functions plus 3
  `CXbcImpl` functions, 12 of them returning -1;
- registered as guest-function overrides at `dc3_title.cc:727-837`;
- `NuiSkeletonGetNextFrame` is routed to `Dc3NuiSequencerExtern` when `--fake_kinect_data` is set
  (`:729-734`, hack id `nui.get_next_frame`, `:818-821`).

### 2.2 Where the true hardware boundary is

Reading `build/373307D9/asm/xdk/nuiapi/nuiruntime.s` gives the real pipeline:

```
NuiInitialize ─▶ NuipInitialize (nuiruntime.s:9050)
                  ├─ ExCreateThread(NuipThreadRoutine)                 (:9354, worker at :8466)
                  ├─ NuipAllocateSTTextures / GPU command buffers      (ST runs on Xenos)
                  └─ XamXStudioRequest (Kinect Studio attach)          (:9730)

camera I/O:  NuipXStudioPsCamDeviceRequest(_NUICAM_REQUEST*)          (:941-961)
                 r = XamXStudioRequest(0x1003, &req)   ── if r >= 0, XStudio played it back
                 else PsCamDeviceRequest(req)           ── the kernel camera driver
             NuipQueueDepth/ColorTransferRequest, NuipSendControlRequest{A,}Synchronous,
             NuipGetVBlankInfo / NuipSynchronizeCameraToVBlank        (raw USB transfers)
             NuipDepthTransferComplete / NuipColorTransferComplete    (_NUICAM_TRANSFER_REQUEST)

per frame:   NuipThreadRoutine → NuipPreProcessFrame → NuipSubmitGPUWork
             (D3D__RunAsyncCommandBuffer ×4, :7624-7767) → NuipGPUFinishSkeletonCallback
             → NuipPostProcessSkeletonFrame (:5106; writes the ring at RuntimeState+0x894)
             → NuipConvertSTSkeletons (_ST_SKELETON_DATA → _NUI_SKELETON_DATA, nuiskeleton.s)
             → NuipSetSkeletonFrameEvents (KeSetEvent internal +0x878 and title +0x888)
             → NuipSendFrameToXam → XNuiInterpretFrame (XMsg 0xFE/0x2B002; nuixam.s:115-138)

title:       NuiSkeletonGetNextFrame copies the ring slot              (nuiskeleton.s:807-1000)
```

Candidate HLE boundaries, lowest first:

| boundary | data there | cost | verdict |
|---|---|---|---|
| **B0: USB/`PsCamDeviceRequest`** | Raw depth (320×240) and colour transfers, control and VBlank requests, in an undocumented `_NUICAM_*` protocol | XL. Needs the protocol reversed from the SDK's call sites, synthetic depth rendered from a pose, the real `ST` Exemplar GPU pipeline running on Xenia's GPU emulation (async command buffers, GPU callbacks via `NuipPatchGPUCallback`), and the `database.xmplr` tracker database. The skeleton that comes out is the tracker's *estimate* of the synthetic body, not the source pose | **No, as the main path.** It breaks R4: the same skeleton sequence cannot reach both sides. Keep as research (phase 6) |
| **B0′: `XamXStudioRequest(0x1003)`** | Microsoft's own record/playback seam, in front of every camera request | Same raw-depth problem as B0 | Not the skeleton seam. But Xenia's stub returns **0**, which tells the SDK "XStudio handled it" and leaves the result uninitialised (§2.2.1) |
| **B1: SDK producer seam** (`NuipPostProcessSkeletonFrame` / `NuipConvertSTSkeletons`) | `_NUI_SKELETON_FRAME` written into the SDK ring | Requires the real SDK running, so B0 has to work first, and binds to `NUIP_RUNTIME_STATE` field offsets | No: it inherits B0's cost and adds nothing over B2 |
| **B2: SDK public API** (`NuiSkeletonGetNextFrame` and its ~55 siblings) | the documented `NUI_SKELETON_FRAME`, handles and events | M-L. One signature table per XDK NUI version | **Yes**, with return codes and event semantics copied from the SDK's own code (§2.3) |
| **B3: kernel/XAM sensor surface** | device status, tilt, mic array, XAM NUI app, `PsCamDeviceRequest` "not connected" | S-M | **Yes, always on.** Faithful whether or not B2 is active |

So the device is B3 plus B2: kernel/XAM faithful, and the SDK API emulated with skeleton
injection where the SDK would hand frames to the title.

The difference from today's table is not "SDK-level vs kernel-level". Today's overrides are:
- placed by title addresses (`dc3_title.cc:274-416`);
- shaped `li r3,0|-1; blr`, with no handles, no events and no state;
- fed constant data.

The facade replaces each of those with, respectively:
- resolution by SDK version;
- real kernel objects and SDK-identical state machines;
- a pose source.

#### 2.2.1 Latent kernel/XAM defects the device fixes

These matter whenever any SDK code runs unpatched.

- `XamXStudioRequest` returns 0 (`src/xenia/kernel/xam/xam_nui.cc:226-230`). The SDK takes `>= 0`
  as "Kinect Studio handled this camera request" and returns the uninitialised `0x54(r1)`
  (`nuiruntime.s:941-961`). With no XStudio attached it must return a failure HRESULT.
- `XamApp` (app `0xFE`) has no handlers for `0x2B001`-`0x2B004` (`src/xenia/kernel/xam/apps/xam_app.cc:28-107`).
  Every `XNuiDelayUI` from `MoveDir` logs "Unimplemented XAM message".
- `PsCamDeviceRequest`, `MicDeviceRequest` and `RmcDeviceRequest` return `X_E_FAIL`
  (`xboxkrnl_misc.cc:44-48`, `xboxkrnl_audio.cc:222-232`). A sensor that is absent should answer
  with the NTSTATUS the SDK tests for (device not connected), not a generic failure.
- `XamNuiGetDeviceStatus` zero-fills everything except `status` (`xam_nui.cc:50-59`). That is fine
  for "absent". "Present" needs the other fields measured (phase 0).

### 2.3 Skeleton frame format and the frame event

`NUI_SKELETON_FRAME` (`src/xdk/nui/nuiskeleton.h:21-39`) is big-endian, 16-byte aligned, and
0xAB0 bytes. The SDK copies exactly `0xab0` bytes (`nuiskeleton.s:876`).

```
NUI_SKELETON_FRAME (0xAB0)
  0x000 u64  liTimeStamp        ms. The consumer takes (int)Δ as elapsed ms (SkeletonUpdate.cpp:228-234)
                                and multiplies by 0.001 for seconds (Skeleton.cpp:101-105)
  0x008 u32  dwFrameNumber
  0x00C u32  dwFlags
  0x010 vec4 vFloorClipPlane    (A,B,C,D); D = sensor height
  0x020 vec4 vNormalToGravity   up vector; leveled by NuiTransformMatrixLevel (Skeleton.cpp:117-121)
  0x030 NUI_SKELETON_DATA[6], stride 0x1C0
NUI_SKELETON_DATA (0x1C0)
  0x000 u32  eTrackingState     0 NOT_TRACKED, 1 POSITION_ONLY, 2 TRACKED
  0x004 u32  dwTrackingID       the game requires > 0 (SkeletonChooser.cpp:472-476,525)
  0x008 u32  dwEnrollmentIndex  (the game copies it to SkeletonData::mClippedFlags, Skeleton.cpp:155)
  0x00C u32  dwUserIndex
  0x010 vec4 Position           hip/centre; the game levels it into mHipCenter (:158-161)
  0x020 vec4 SkeletonPositions[20]          NUI joint order (below), camera space, metres
  0x160 u32  eSkeletonPositionTrackingState[20]   0/1/2
  0x1B0 u32  dwQualityFlags     clip bits
```

The SDK confirms this layout. `NuipConvertSTSkeletons` (`nuiskeleton.s`) does the following:
- zeroes `0xA80` = 6×0x1C0;
- sets state 1 plus `Position` for every detected body;
- sets state 2, copies `0x140` bytes of joints and `0x50` bytes of joint states, and writes
  `+0x1B0`, for each tracked body;
- zeroes the ten leg joints when `ST_IsSeated()`.

**NUI joint order vs DC3 game order.** `Skeleton.cpp:46-50`, `sJointRemap` (dst = game, src = NUI):
- NUI 0-14 equal game 0-14;
- NUI 15 FOOT_LEFT is game 18;
- NUI 16-18 (HIP/KNEE/ANKLE_RIGHT) are game 15-17;
- NUI 19 is game 19.

That is the standard Kinect v1 order, so the native DC3_20 wire layout (game order) needs exactly
this permutation.

**`NuiSkeletonGetNextFrame(DWORD ms, NUI_SKELETON_FRAME*)` semantics** (`nuiskeleton.s:807-1000`):

| condition | HRESULT |
|---|---|
| `frame == NULL` | `0x80070057` E_INVALIDARG (:812-817) |
| tracking not enabled (`RuntimeState+0x874 == 0`) | `0x83010002` (:941-942) |
| device not connected (`+0x7C == 0`) | `0x8007048F` E_NUI_DEVICE_NOT_CONNECTED |
| wait on internal KEVENT `+0x878` with timeout ms (capped at 8000, or 60000 by a flag) times out | `0x8000000A` E_PENDING |
| ring slot (`+0x894` index, 0xAE0-byte entries at `+0x8A0`) status 1 | copy 0xAB0 bytes, mark the slot consumed, record latency, **S_OK** |
| slot status 2 | `0x8301000B` E_NUI_SYSTEM_UI_PRESENT |
| otherwise | `0x83010001` (no new frame) |

On any result except E_PENDING, it `KeResetEvent`s both the title event (`+0x888`) and the
internal one (`+0x878`) (:951-957). The title event is the handle passed to
`NuiSkeletonTrackingEnable`, referenced through `ObReferenceObjectByHandle(ExEventObjectType)`.

The producer sets both events once per depth frame, at 30 Hz, **whether or not anyone is in
view**. Kinect 1.8 documents it: "The runtime signals a skeleton frame event every time a depth
frame is available, even if no skeleton is detected" ([NuiInitialize](https://learn.microsoft.com/en-us/previous-versions/windows/kinect-1.8/hh855484(v=ieb.10))).

DC3's consumer is a manual-reset event (`SkeletonUpdate.cpp:593`). The worker waits INFINITE,
calls `GetNextFrame(0, …)`, and the call resets the event. This is exactly why
`skel.wait_33ms` exists: the stubbed `NuiSkeletonTrackingEnable` never stores or sets the event.

#### 2.3.1 Defects in today's constant-pose frame

`src/xenia/titles/dc3/dc3_nui_sequencer.cc`:

| line | defect | effect |
|---|---|---|
| 148 | `kFrameSize = 0x30 + 6*0x1B4` (= 0xA68) | Wrong stride. Only skeleton 0 is written, so offsets agree, but the memset leaves the last 0x48 bytes stale |
| 151 | `liTimeStamp = frame*33333` | ms units are expected, so every frame reads as Δ = 33 333 ms: `mElapsedMs` = 33 333 and the up-vector smoother is fed 33 s steps |
| 163-191 | `eSkeletonPositionTrackingState[]` never written (all 0), `dwQualityFlags` 0, joint `w` 0 | Every joint NOT_TRACKED. `SkeletonQualityFilter::UpdateIsConfident` (`SkeletonQualityFilter.cpp:71-84`) counts only TRACKED joints plus 4 foot/ankle joints, so confidence is 4 of 20 and the skeleton is never confident. In the shell, `mValid = mIsConfident && …` (`:63-67`) is always false. A never-valid skeleton cannot be chosen, so no player is bound, so `CheckForSkeletonLoss` (`Game.cpp:795-816`) sees 0 players playing. **This is the likely root cause of the six `calib.*` patches and `game.pause_for_skeleton_loss`** (to be tested in phase 2) |
| 205-218 | `TheGestureMgr+0x426D := 1` per frame (`seq.controller_mode`) | A host write to a title global |
| 223-227 | IK telemetry is driven by this callback | A tick coupling that has to move (phase 1) |

### 2.4 Calibration, speech, identity and image streams

Each item gets one decision: **Provide** (the device must emulate it), or **Unavailable**, which
means faithfully reporting the SDK's own "absent" answer, where the game has a non-fatal path.

| subsystem | DC3 dependency | decision | why |
|---|---|---|---|
| Skeleton stream | Gameplay, scoring, menus, `SkeletonChooser` calibration | **Provide** | the point of the device |
| Calibration (`SkeletonChooser`, `ShellInput`) | Not an SDK feature: game logic over `IsValid()`/`IsTracked()` skeletons and `SetTrackedSkeletons` | **Provide its inputs**: realistic per-joint states, stable tracking IDs, plausible floor and hip, and the title-sets-tracked-skeletons policy (flag 2, from `kinect.dta` `(title_tracked_skeletons TRUE)`, `LiveCameraInput.cpp:650-656`) | Then calibration runs natively. No `calib.*` patch |
| Tilt (`NuiCameraElevation*`, `AdjustTilt`, `GetNormalToGravity`) | `CameraTilt` state machine | **Provide** a motor model: angle in [-27, 27], a slew rate, moving flags; `AdjustTilt` completes its XOVERLAPPED through `KernelState::CompleteOverlapped` (`kernel_state.h:172-176`) | Cheap, and it lets the calibration-adjacent tilt flow finish |
| Camera properties and exposure ROI | `SetColorCameraProperty`, `DumpProperties`, `SetAutoexposureRegion` (`LiveCameraInput.cpp:69, 989-1053, 1148-1199`) | **Provide** a property store (get returns the last set, else the SDK default) | `GetExposureRegionOfInterest` failing raises `MILO_FAIL` (`:1161-1163`) |
| Image streams (colour YUV 640×480; depth+player 320×240) | Asserted open at init; polled each frame (colour only) | **Provide open/close** with real kernel handles (the dtor `CloseHandle`s them). **Frames: Unavailable** at first: `GetNextFrame` returns `0x83010001` (no frame data), which the game counts and ignores (`:924-926`). Real frames are phase 6 | `StreamBufferData` and the photo paths are null-safe (`:943-966`) |
| Identity (`NuiIdentity*`, `XamNuiIdentity*`, `XamUserNui*`) | Auto sign-in by face | **Unavailable, faithfully**: `GetEnrollmentInformation` writes a zeroed struct with S_OK (no one enrolled); `Identify/Enroll` return a non-pending failure, so `RequestIdentity` returns false (`Skeleton.cpp:523-528`); `Abort` S_OK | Today's stub returns S_OK **without writing the output**, so `SkeletonIdentifier` reads uninitialised stack (`SkeletonIdentifier.cpp:240-242`). Callbacks would need guest execution from a host thread, which is avoided by design |
| Speech (NUISP) | Voice commands; grammars created even when disabled | **Provide a state machine, "silent" by default**: `Enable` S_OK; grammars, rules and states as host objects with opaque guest handles; `Load/Commit/SetState` track state; `Start/Stop` toggle; `GetEvents` returns S_OK with 0 events; `EmulateRecognition` queues a scripted phrase (phase 6). Mode `unavailable`: `Enable` returns `E_NUI_DEVICE_NOT_CONNECTED`, the game logs "(no Kinect?)" (`SpeechMgr.cpp:210`), and grammar creation still succeeds | Grammar creation must succeed in both modes (`:145-147`, `VoiceControlPanel.cpp:205-216`) |
| Audio (NUIAUD beamforming) | `mVoiceDirection` and audio callbacks | **Unavailable**: `NuiAudioCreate` returns `E_NUI_DEVICE_NOT_READY`; the game wraps it in `if SUCCEEDED` (`LiveCameraInput.cpp:646-649`) | Avoids guest callbacks. Unchanged from today in effect, but with a meaningful code |
| Fitness, Wave, HeadOrientation/Position | `FitnessFilter`, `WaveToTurnOnLight` | **Unavailable** (named failure codes); head `*Disable` S_OK | MILO_NOTIFY-only paths. Revisit if a flow needs calories or the wave light |
| Kinect Guide (XNotify `0x6001A`) | `KinectGuideThread` | **Provide the plumbing**: an optional scripted "guide gesture" raises the notification; default never | System UI; not needed for S1 |
| `NuiMetaCpuEvent` | Called only by SDK internals and `ST` (`xdk/ST/*.s`), `FitnessFilter` and `WaveToTurnOnLight` | **Provide** a no-op S_OK | Its internal callers never run under the facade |

### 2.5 The native pose protocol

#### Wire format

The wire format is shared by `scripts/synthetic_kinect.py:286-302`, the parser
`native/src/platform/Skeleton_Native.cpp:210-357`, and `native/scripts/pose_server.py`.

```
transport  AF_UNIX SOCK_STREAM. The ENGINE is the client: it connects to $DC3_POSE_SOCKET
           (DC3_POSE=external DC3_POSE_NO_SPAWN=1). The server listens and accepts ONE
           client (synthetic_kinect.py:305-325).
framing    u32 little-endian length, then the body
v2 body    u32 magic 0x44503302 | u32 frame_id | u32 num_persons | f64 timestamp (seconds)
           | u16 w | u16 h | u8 num_landmarks | u8 layout | u16 pad          (28 bytes)
           per person: i32 track_id + num_landmarks × (f32 x, f32 y, f32 z, f32 conf)
layouts    0 COCO17 (normalised image coordinates); 1 DC3_20 (game joint order, camera-space metres)
v1         legacy, no magic; COCO17 only
conf       < 0.3 NOT_TRACKED, < 0.6 INFERRED, else TRACKED (Skeleton_Native.cpp:66-70)
ids        synthetic_kinect uses 5 + 4·i ("Kinect tracking ids are > 0", :299)
cadence    30 Hz on wall time, or one packet per answered target while performing; the
           timestamp follows SONG time while performing (:286-296)
```

#### Mapping to `NUI_SKELETON_FRAME`

This mapping is the `SocketPoseSource` / `TapePoseSource` contract.

| NUI field | from DC3_20 v2 |
|---|---|
| `liTimeStamp` | `round(timestamp_s × 1000)`. Native derives elapsed from Δtimestamp the same way (`GestureMgr_Native.cpp:348-376`) |
| `dwFrameNumber` | `frame_id` |
| `vFloorClipPlane` | `(0, 1, 0, 0)`: native's `FinalizeSkeletonFrame` uses floor y=0, up=+y (`Skeleton_Native.cpp:618-624`) |
| `vNormalToGravity` | `(0, 1, 0, 0)`, so `NuiTransformMatrixLevel` is identity and leveled positions equal camera positions, as on native, which writes `kCoordCamera` directly |
| skeleton slot | stable `track_id → slot`: a persisting id keeps its slot, and a new id takes the lowest free slot. This is the same policy as native's `AssignSlots` (`GestureMgr_Native.cpp:181-235`) |
| `eTrackingState` | 2 for every person present (native fills every slot at full fidelity; `GestureMgr.cpp:468-476`). The "faithful" policy may demote people the title did not select to 1 (§3.3.4) |
| `dwTrackingID` | `track_id` (must be > 0; the source rejects ≤ 0) |
| `Position` | joint HIP_CENTER (index 0) |
| `SkeletonPositions[n]` | `dc3[perm[n]]` with `perm = {0..14, 18, 15, 16, 17, 19}`, `w = 1.0` |
| `eSkeletonPositionTrackingState[n]` | conf thresholds above, applied to `dc3[perm[n]].conf` |
| `dwQualityFlags` | 0 (or computed clip bits, phase 2) |
| `dwEnrollmentIndex`, `dwUserIndex` | the SDK defaults from phase 0's read of `NuipConvertSTSkeletons` |

#### `/api/pose/target` and replay

`native/src/platform/PoseTarget_Native.cpp:1-36, 201-292` serves, for each player, the skeleton
the game wants. Sources, in order:
- the fatality target;
- the scheduled `DetectFrames` interpolated at `SongSeconds() - latency + lead` ("choreo");
- the current move's frames ("move");
- otherwise "none".

It reads native-only accessors (`NativePlayerDetectFrames`, `NativeFindDetector`,
`NativeMatchState`) that do not exist in the original XEX, so Xenia cannot serve the endpoint.

The parity design is therefore a **target tape**:
1. Run native with `synthetic_kinect.perform()`.
2. Record every packet it sends. While performing, the packets are stamped with song seconds.
3. Replay that byte stream into Xenia with `TapePoseSource` in song-clock mode.

Song-clock mode needs a guest song-time provider. That is title-specific, so it lives in
`titles/dc3` as an optional plug-in (`NuiClockProvider`). The DTA channel (S2) or the read-only
`TheTaskMgr` (0x82F64A58) can supply it. The device itself stays on the guest clock.

#### Known native/Xbox semantic gaps a timeline diff must expect

1. Native auto-binds players to skeletons (`BindPlayerSkeletons`, `GestureMgr_Native.cpp:270-332`).
   Xbox binds through `SkeletonChooser`.
2. Native clamps elapsed to [1, 200] ms. The Xbox consumer does not.
3. Native never calls `SkeletonFrame::Create`, so there is no up-vector smoothing.

None of these is a device defect. They belong in the parity report as expected divergence
classes.

### 2.6 RB3

RB3 has no Kinect use:
- `rb3-xenon/config/45410914/symbols.txt` has no NUI, `PsCam` or `XNui` symbols. The only
  substring hits are `XNotifyPositionUI` and `XamShowSigninUI`.
- `rb3/config/SZBE69_B8/symbols.txt` (Wii) hits only unrelated substrings.

The device therefore has to be inert for RB3, and S6 already checks that.

**DC1 is a Kinect title.** S6.1 boots DC1 TU0 (`tools/fork-regress/scenarios/S6.sh:3,13`). It is
the natural second-title test of title-agnosticism (phase 5): a different XDK NUI version means
a second signature table, with no DC1 code anywhere.

### 2.7 Upstream and public forks

- **Upstream xenia.** `xam_nui.cc` stubs `XamNuiGetDeviceStatus` as "not connected"; there is no
  NUI emulation ([xam_nui.cc](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xam/xam_nui.cc)).
  Two open requests have no maintainer replies and no code:
  - [#2339 "Add support for Kinect."](https://github.com/xenia-project/xenia/issues/2339), opened 2026-01-31;
  - [#2347 "I need help adding Kinect support…"](https://github.com/xenia-project/xenia/issues/2347),
    opened 2026-03-26. It links [weronika-saturday/xenia-canary](https://github.com/weronika-saturday/xenia-canary),
    whose landing page shows no Kinect work.
- **xenia-canary.** The FAQ says Kinect is unsupported ([FAQ](https://github.com/xenia-canary/xenia-canary/wiki/FAQ)).
  Two issues ask for it without answers: [#537](https://github.com/xenia-canary/xenia-canary/issues/537)
  and [#1188](https://github.com/xenia-canary/xenia-canary/issues/1188).
  [#754 (unimplemented kernel functions)](https://github.com/xenia-canary/xenia-canary/issues/754)
  lists:
  - `PsCamDeviceRequest` for 58480811, 4D5309C9, 5848081A, 4D5308C9;
  - `XamNuiCameraTiltSetCallback` and `MicDeviceRequest`.

  That is independent evidence that other Kinect titles hit the same kernel boundary as DC3.
  [RetroDECK](https://retrodeck.readthedocs.io/en/latest/wiki_controllers/xbox/xbox-360-kinect/)
  also states "There exists no Kinect Emulation".
- **Prior art for the method.** Cxbx-Reloaded's [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase)
  locates statically linked XDK library functions with OOVPA signatures, one table per library
  version, and HLEs them. Its [maintenance notes](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/wiki/Maintaining-OOVPAs-for-HLE-function-detection)
  stress unique signatures and XREFs to avoid false matches. §3.3.1 adopts that discipline.
- [KinectFaker](https://github.com/TinyTinni/KinectFaker) replays recorded skeletons through a
  Kinect-for-Windows proxy DLL, and implements `NuiSkeletonGetNextFrame`. It is the same idea on a
  PC SDK, not portable code.

---

## 3. Architecture

### 3.1 Component diagram

```
 guest title (any Kinect title)                        │ xenia host
───────────────────────────────────────────────────────┼──────────────────────────────────────────────
 game code (LiveCameraInput, SkeletonUpdate, SpeechMgr)│
     │ calls statically-linked NUI SDK entry points    │
     ▼                                                 │
 NUI SDK public API  ── RegisterGuestFunctionOverride ─┼─▶ kernel/nui/NuiSdkFacade
 (NuiInitialize, NuiSkeleton*, NuiImage*, NuiCamera*,  │     (per-XDK-version signature table,
  NuiSpeech*, NuiIdentity*, NuiAudio*, NuiFitness* …)  │      selected from XEX_HEADER_STATIC_LIBRARIES)
     │ (SDK internals: NuipInitialize, ST tracker,     │          │ API semantics, HRESULTs, handles
     │  NUISP/NUIAUD, …: NOT executed under facade)    │          ▼
     │                                                 │     kernel/nui/NuiDevice ◀── cvars, title profile
     │                                                 │      ├─ SkeletonStream ── ring[2], frame#, tracked-ID policy,
     │                                                 │      │                    title XEvent (ObjectTable ref)
     │                                                 │      ├─ ImageStreams ──── XObject handles, (frames: phase 6)
     │                                                 │      ├─ CameraModel ───── elevation motor, properties, ROI,
     │                                                 │      │                    AdjustTilt → CompleteOverlapped
     │                                                 │      ├─ SpeechModel ───── grammars/rules/states, recognising,
     │                                                 │      │                    event queue (silent | scripted)
     │                                                 │      ├─ IdentityModel ─── "no enrolments"
     │                                                 │      └─ FrameClock ────── 30 Hz on the GUEST clock
     │                                                 │                │ Sample(t)
     │                                                 │                ▼
     │                                                 │     kernel/nui/PoseSource (interface)
     │                                                 │      ├─ EmptyPoseSource     sensor present, nobody in view
     │                                                 │      ├─ ConstantPoseSource  standing pose (valid, all TRACKED)
     │                                                 │      ├─ TapePoseSource      recorded DC3_20/NUI_20 packet stream
     │                                                 │      └─ SocketPoseSource    DC3_POSE_SOCKET v2 client
     │                                                 │                ▲ optional clock plug-in (titles/dc3: song time)
     │ imports                                         │
     ▼                                                 │
 XAM / xboxkrnl exports  ──────────────────────────────┼─▶ faithful sensor surface (always on, upstreamable)
 XamNuiGetDeviceStatus, XamNuiCamera*, XamNuiIdentity*,│     xam_nui.cc / xam_voice.cc / xboxkrnl_{misc,audio}.cc
 XamVoiceGetMicArray*, XamXStudioRequest, XamShowNui*, │     apps/xam_app.cc: app 0xFE msgs 0x2B001-0x2B004
 PsCamDeviceRequest, MicDeviceRequest, RmcDeviceRequest│     XNotify 0x6001A (guide gesture), driven by NuiDevice
 XMsg app 0xFE                                         │     all of them read NuiDevice.present()
```

**Threading.** The FrameClock is a host thread. It only does three things:
1. samples the pose source;
2. writes the host-side ring;
3. calls `XEvent::Set` on the title event (`src/xenia/kernel/xevent.h:36`).

It never executes guest code. The title's own `SkeletonUpdate` worker wakes, calls the
`NuiSkeletonGetNextFrame` override on its own guest thread, and that override copies the ring
slot into guest memory. This removes the "host runs guest code on the skeleton worker" class
(gap analysis G6, O18) by construction.

### 3.2 Layer K: kernel/XAM sensor surface (faithful, upstreamable)

Everything reads one `NuiDevice::present()` flag. The existing `--nui_device_present`, profile-set
for DC3 at `src/xenia/titles/title_profile.cc:72`, stays the switch.

| export | absent (default, as upstream) | present |
|---|---|---|
| `XamNuiGetDeviceStatus` | as today | status = connected; other fields as measured in phase 0 |
| `XamNuiCamera*` | not-connected error | backed by `CameraModel` (shared with the SDK facade's `NuiCameraElevation*`) |
| `XamNuiIdentityGetSessionId`, `XamUserNui*`, biometrics | as today | "no enrolment" answers |
| `XamVoiceGetMicArray*`, `MicDeviceRequest`, `RmcDeviceRequest` | device-not-connected | device present, silent: status OK, audio calls return no data (an underrun-free empty buffer) |
| `PsCamDeviceRequest` | `STATUS_DEVICE_NOT_CONNECTED` | `STATUS_DEVICE_NOT_CONNECTED` too, until phase 6. Only the facade's API needs to know that a sensor exists |
| `XamXStudioRequest` | failure HRESULT (fixes §2.2.1) | failure HRESULT |
| app `0xFE` `0x2B001` DelayUI | S_OK, records the delay | S_OK |
| `0x2B002` InterpretFrame | S_OK | S_OK; optional guide-gesture detector raises XNotify `0x6001A` |
| `0x2B003`/`0x2B004` TitleInitialize/Shutdown | S_OK | S_OK, tracks title registration |

### 3.3 Layer S: the SDK facade

#### 3.3.1 Detection and resolution (title-agnostic)

At `UserModule` load, the facade does the following:

1. Read `XEX_HEADER_STATIC_LIBRARIES`. Xenia already parses it, at `src/xenia/kernel/user_module.cc:537-544`.
2. If `NUI` is present, select `kNuiSignatureTables[{major, minor, build}]`. For DC3 that is
   `{2,0,21173}`. If no table matches the version, log `NUI HLE: unsupported NUI <ver>` and
   leave the title on layer K alone. That is the honest "Kinect unplugged".
3. Resolve each API function in the table. The resolver must:
   - scan only `.text`;
   - use multi-word signatures with relocation masks, starting from the ones
     `dc3_nui_patch_resolver.cc:25-300` already has for 10+ functions;
   - anchor on XREFs (for example, `NuiSkeletonGetNextFrame` is the only function that references
     both `NuipRuntimeState+0x894` and `NuipSkeletonLock`);
   - **refuse on zero or multiple matches**, logging the function as unresolved;
   - only then call `Processor::RegisterGuestFunctionOverride` (`src/xenia/cpu/processor.h:119`).
4. Install a **tripwire override** on every SDK entry point the table knows but does not
   implement. If one is reached, the device logs `NUI HLE: unimplemented SDK entry <name> reached
   (TAINTED)`. An unpatched SDK body touching an uninitialised `NuipRuntimeState` is then loud,
   not silent heap corruption.

A `--nui_symbol_map=<symbols.txt>` cross-check mode (debug only) asserts that every resolved
address equals the map's address. Run it on DC3 original and decomp in CI.

#### 3.3.2 Data structures

```cpp
namespace xe::kernel::nui {

// Canonical pose: NUI joint order, camera space, metres, right-handed, +y up.
enum class JointState : uint8_t { kNotTracked = 0, kInferred = 1, kTracked = 2 };
struct PoseJoint { float x, y, z; JointState state; };
struct PosePerson {
  uint32_t   tracking_id;          // > 0, stable while the person persists
  PoseJoint  joints[20];           // NUI order (HIP_CENTER … FOOT_RIGHT)
  uint32_t   quality_flags = 0;    // NUI_SKELETON_QUALITY_CLIPPED_* bits
};
struct PoseFrame {
  uint64_t   sensor_time_ms;       // becomes liTimeStamp
  uint32_t   frame_number;         // becomes dwFrameNumber
  float      floor_clip_plane[4];  // default {0,1,0,0}
  float      normal_to_gravity[4]; // default {0,1,0,0}
  std::vector<PosePerson> persons; // ≤ 6
};

class PoseSource {
 public:
  virtual ~PoseSource() = default;
  virtual bool Open() = 0;
  // The newest frame at or before guest time `now_ms`. Returns false if the sensor has
  // no new depth frame (GetNextFrame then answers 0x83010001).
  virtual bool Sample(uint64_t now_ms, PoseFrame* out) = 0;
  virtual std::string Describe() const = 0;   // logged once: kind, path, layout, clock
};

// Wire-layout adapter shared by Tape and Socket (title-agnostic: a named permutation).
enum class WireLayout : uint8_t { kCoco17 = 0, kDc3_20 = 1, kNui20 = 2 /* new, canonical */ };
bool DecodeV2Packet(std::span<const uint8_t> body, PoseFrame* out, std::string* err);

// Guest-visible encoding (big-endian, 0xAB0 bytes).
void EncodeSkeletonFrame(const PoseFrame& f, const TrackedPolicy& policy,
                         const SlotMap& slots, uint8_t* guest_frame /*0xAB0*/);

struct SkeletonStream {
  bool       enabled = false;
  uint32_t   flags = 0;                      // 2 = title sets tracked skeletons, 4 = seated
  object_ref<XEvent> title_event;            // from NuiSkeletonTrackingEnable(h, …); may be null
  uint32_t   title_tracked[2] = {0, 0};      // NuiSkeletonSetTrackedSkeletons; ≤ 0 or ~0 = none
  SlotMap    slots;                          // track_id → 0..5 (AssignSlots policy, §2.5)
  struct Entry { enum { kEmpty, kNew, kSystemUi } status; uint8_t frame[0xAB0]; uint64_t produced_tb; };
  Entry      ring[2]; uint32_t write_index = 0;
  std::mutex lock;                           // mirrors NuipSkeletonLock
  uint64_t   frames_produced = 0, frames_served = 0, events_set = 0;  // instruments
};

class NuiDevice {
 public:
  static NuiDevice* Get();                   // owned by KernelState; null when !present
  bool present() const;
  SkeletonStream& skeletons();
  ImageStreams& images();                    // XObject-backed handles
  CameraModel& camera();                     // elevation, motor, properties, ROI
  SpeechModel& speech();                     // grammars/rules/states/recognising/events
  IdentityModel& identity();
  void StartFrameClock();                    // 30 Hz on Clock::QueryGuestUptimeMillis()
  void StopFrameClock();
};
}  // namespace xe::kernel::nui
```

#### 3.3.3 API semantics

All semantics below are copied from the SDK's code (§2.3), not invented.

| API | facade behaviour |
|---|---|
| `NuiInitialize(flags, …)` | `E_NUI_ALREADY_INITIALIZED` if already initialised; otherwise create device state and start the FrameClock, S_OK. Absent sensor: S_OK with `connected=false`, matching the SDK's "+0x7C == 0" state |
| `NuiShutdown` | stop the clock, drop references, S_OK |
| `NuiSkeletonTrackingEnable(h, flags)` | if `h` is non-null, `LookupObject<XEvent>(h)` (as `kernel_state.cc:705` does), else null. Store it and the flags, `enabled=true` |
| `NuiSkeletonTrackingDisable` | release the event, `enabled=false`, reset ring |
| `NuiSkeletonSetTrackedSkeletons(ids)` | record two ids; return E_INVALIDARG for an id that is non-zero, not -1, and unknown, if phase 0 shows the SDK does |
| `NuiSkeletonGetNextFrame(ms, f)` | exactly the table in §2.3. Waits are a host wait on the ring's condition, bounded by `ms` (capped at 8000), with no host-side busy loop |
| FrameClock tick (30 Hz guest time) | `Sample()`; if there is a frame, `EncodeSkeletonFrame` into `ring[w]`, `status=kNew`; then `title_event->Set(0,false)`. If there is no frame, still raise the event when the source says "empty sensor" (Kinect raises it every depth frame, §2.3) |
| `NuiImageStreamOpen(type,res,…,phStream)` | allocate an `XNuiImageStream : XObject` handle, write `*phStream`, S_OK. A second open of the same type gives `E_NUI_IMAGE_STREAM_IN_USE` (`0x83010003`) |
| `NuiImageStreamGetNextFrame` / `ReleaseFrame` | `0x83010001` / S_OK (frames in phase 6). `CloseHandle` works because the handle is a real object |
| `NuiCamera*` | `CameraModel` |
| `NuiSpeech*` | `SpeechModel` (§2.4) |
| `NuiIdentity*`, `NuiAudio*`, `NuiFitness*`, `NuiWave*`, `NuiHead*Disable`, `NuiMetaCpuEvent` | as decided in §2.4 |

#### 3.3.4 Tracking policy

There are two policies, chosen by cvar `nui_tracked_policy`:
- `faithful` (default): with flag 2 set, a person is TRACKED only once its id appears in
  `title_tracked[]`, and POSITION_ONLY otherwise. Before any selection, the first two persons are
  TRACKED. Phase 0 must confirm this against `NuipNuiHandsDefaultTrackingPolicy` /
  `ST_UpdatePlayerTrackingIDsWithSkeletons_TwoPlayer`.
- `all`: every present person is TRACKED. This is native parity, matching the comment in
  `GestureMgr.cpp:468-476`.

### 3.4 Pose sources

1. **`EmptyPoseSource`.** The sensor is present and the room is empty. The event is raised every
   frame and every slot is NOT_TRACKED. This is the honest default for a "present" sensor with no
   input. The game then shows its own "step in front of the sensor" UI.
2. **`ConstantPoseSource`.** One standing person, id 1, all joints TRACKED, `w=1`, floor plane
   from the pose. It replaces today's table. The joints are `synthetic_kinect.STAND`
   (`synthetic_kinect.py:73-80`) permuted to NUI order, so the constant pose is *the same* pose
   native's dummy uses.
3. **`TapePoseSource`.** The file format is the exact socket byte stream (u32 length + v2 body)
   with a 16-byte file header (`"NUITAPE1"`, `u32 version`, `u32 clock_kind`). The same bytes
   that drove native drive Xenia.
   - `clock=sensor`: a packet is due when `guest_ms - t0 >= ts - ts0`.
   - `clock=song`: due when the clock plug-in's song time reaches `ts`.

   The host tool `tools/nui/tape.py` (record from a socket, dump, diff) is DC3-agnostic.
4. **`SocketPoseSource`.** An AF_UNIX client of `$DC3_POSE_SOCKET` (cvar `nui_pose_socket`). It
   runs a reader thread with a back buffer, like `NativeSkeletonProvider::ReaderThread`, and
   decodes v1 and v2 with layouts 1 and 2. Frames are consumed at the FrameClock's cadence. The
   packet timestamp becomes `liTimeStamp`. If the timestamp does not advance, the source falls
   back to guest-ms, matching native's fallback.

Because `synthetic_kinect.py` accepts one client, a native-vs-Xenia A/B runs two server
instances from one script with the same pose schedule. For bit-identical streams, use a tape.

**Instrument.** `--nui_frame_dump=<path>` writes every served `NUI_SKELETON_FRAME`
(host-endian, plus the source's frame id). A sibling native dump of `PersonData` lets
`tools/nui/parity.py` compare decoded joints frame-by-frame. It is the precondition for any
timeline comparison.

### 3.5 What stays in `titles/dc3`

- The song-clock plug-in for `TapePoseSource`.
- Harness automation (`dc3_autonav.cc`), until gesture-scripted navigation replaces it (a
  separate, later lane).
- **Nothing else NUI-related.** The 59-entry table, `dc3_nui_sequencer.cc`, the NUI half of
  `dc3_nui_patch_resolver.cc` and `dc3_hack_pack_skeleton.cc` are deleted. The `CXbcImpl` entries
  move to their own `xbc.*` ids until the XAM LRC lane (O15-O17) retires them.

---

## 4. Hacks retired, mapped to PATCH_MANIFEST ids

The ids below are from `docs/fork/dc3/PATCH_MANIFEST.md` and `src/xenia/titles/dc3/dc3_hacks.cc`.
The `O*` numbers are from `docs/fork/cleanup/DC3_HACK_GAP_ANALYSIS.md`.

| PATCH_MANIFEST id | site | gap id | retired in phase | how |
|---|---|---|---|---|
| `nui.get_next_frame` | `NuiSkeletonGetNextFrame` 0x829C2790 | O2 | 1 | facade GetNextFrame + pose source |
| `nui.NuiInitialize`, `nui.NuiShutdown`, `nui.NuiSkeletonTrackingEnable`, `nui.NuiSkeletonTrackingDisable`, `nui.NuiSkeletonSetTrackedSkeletons` (5) | 0x829D1200, 0x829CEDA0, 0x829C25F0, 0x829C1E18, 0x829C1F90 | O1 | 1 | facade lifecycle + skeleton stream |
| `skel.wait_33ms` | `SkeletonUpdateThread`+0xA4, 0x8242E74C | O4 | 1 | the title event is stored and set at 30 Hz |
| `skel.is_override_nop` | `SkeletonUpdate::Update`+0x40, 0x8242E1B0 | O5 | 1 (A/B) | expected unnecessary with valid frames; A/B decides |
| `calib.player_present_guard`, `calib.choose_player_sides`, `calib.warning_data`, `calib.nav_data`, `calib.wait_recovery`, `calib.exit_controller_mode` (6) | 0x8290834C, 0x82909968, 0x82907880, 0x82909340, 0x82904CD0, 0x82902748 | O8-O13 | 2 | valid, confident skeletons (§2.3.1); tracking policy |
| `game.pause_for_skeleton_loss` | 0x82866D50 | O14 | 2 | the player is bound and IsPlaying |
| `seq.controller_mode` | `TheGestureMgr`+0x426D | O6 | 2 (A/B) | the device writes no title globals. Controller mode exits on a pad-idle timeout (`ShellInput.cpp:181-183, 307-309`), so scripted pad input keeps it faithfully |
| `nui.NuiImageStream{Open,GetNextFrame,ReleaseFrame}`, `nui.NuiImageGetColorPixelCoordinatesFromDepthPixel` (4) | 0x829C9330, 0x829C86F0, 0x829C8A18, 0x829C91C8 | O1 | 3 | ImageStreams |
| `nui.NuiCamera*` (9: Set/GetProperty, GetPropertyF, Set/GetExposureROI, ElevationSet/GetAngle, AdjustTilt, GetNormalToGravity) | 0x829C7F48 … 0x829C4E38 | O1 | 3 | CameraModel |
| `nui.NuiAudio*` (7) | 0x82A0E028 … 0x82A0C108 | O1 | 3 | Unavailable with a named code |
| `nui.NuiIdentity*` (4) | 0x829C36B0 … 0x829C3BB0 | O1 | 3 | IdentityModel (also fixes the uninitialised read) |
| `nui.NuiFitness*` (5), `nui.NuiWave*` (2), `nui.NuiHead*Disable` (2), `nui.NuiMetaCpuEvent` (1) | 0x829D1B68 … 0x82B57560 | O1 | 3 | Unavailable / no-op |
| `nui.NuiSpeech*` (16) | 0x82A21068 … 0x82A24B88 | O1 | 3 | SpeechModel |
| `speech.grammar_unload` | `SpeechMgr::Grammar::Unload` 0x82439F38 | O7 | 3 (A/B) | consistent recognising state. If the assert still fires, it is a flow-order finding (nav-bridge timing), not a device fault, and it is filed rather than patched |
| profile `nui_device_present` (G7) | `title_profile.cc:72` | G7 | 3 | kept as the device switch; semantics now complete |
| cvars `stub_nui_functions`, `fake_kinect_data`, `dc3_guest_overrides`'s NUI half, `dc3_nui_*` resolver cvars | `dc3_flags.cc:103-115`, oracle toml | — | 3 | deleted; S0 lists the cvar removals |
| decomp layout: 85 decomp NUI table entries + 13 decomp-pack NUI stubs (LiveCameraInput ×2, GestureMgr ×3, SkeletonIdentifier ×2, SkeletonUpdate ×2, SkeletonHistoryArchive, ShellInput::Init, VoiceInputPanel::LoadVoiceContexts, CRT `dc3_crt_skip_nui`) | `titles/dc3/decomp/` | §1b | 5 | decomp-layout signature variants (the resolver already carries orig and decomp words) |
| **not retired**: `nui.CXbcImpl::{Initialize,DoWork,SendJSON}` (3) | 0x82606078, 0x82605960, 0x82605DF8 | O15-O17 | — | SmartGlass/XLRC, not Kinect. Re-id as `xbc.*` |

**Count, original layout.**

Retired:
- 56 NUI table entries (55 `nui.<Function>` + `nui.get_next_frame`);
- `skel.wait_33ms` and `skel.is_override_nop`;
- `speech.grammar_unload`;
- 6 `calib.*`;
- `game.pause_for_skeleton_loss`;
- `seq.controller_mode`.

That is **67 ids**, plus 2 transitional cvars. The gap analysis's "69" also counted the
already-deleted PPC GetNextFrame stub and the G7 cvar.

**Count, decomp layout.** 98 more, retired in phase 5.

**Related, not counted.** The DC3 profile's `min_guest_thread_stack_size` of 4 MiB (gap G6)
exists only because host automation ran UI code on the SkeletonUpdate worker
(`title_profile.cc:67-70`). The device never runs guest code there, so G6 becomes an A/B
candidate in phase 1.

---

## 5. Phased plan, each phase with its definition of done

**Common definition of done (DoD), applied to every phase.**

S1 via `tools/fork-regress/run.sh … --scenarios S1` meets its README criteria
(`tools/fork-regress/README.md:22`):
- title ≤ 30 s and game_screen ≤ 60 s;
- ≥ 60 `gpState=2` samples, and `gpState=3` (song end) seen;
- rc 0 plus the TIMEOUT line, and SIGSEGV 0;
- at the README's repeat/threshold, with `compare.py --paired` against a control binary showing no
  pass-rate regression.

On top of that:
- **zero** `DC3 TRIPWIRE … TAINTED` lines (no worker `Debug::Fail`, no `mFailing` latch;
  `dc3_fail_tripwire.cc`);
- **zero** `NUI HLE: unimplemented SDK entry … (TAINTED)` lines;
- S6 still shows no new leaking pattern (the device is off for RB3 and DC1 by default);
- S0's cvar diff lists exactly the intended cvar changes.

Each retirement is first measured with the PATCH_MANIFEST A/B recipe
(`ab.sh … --extra-a "--dc3_disable_hacks=<ids>"`) with the device on, then deleted in a
separate commit.

### Phase 0: evidence and spec

No behaviour change.

- Read from the target asm and record in `docs/fork/core/NUI_DEVICE.md`:
  - the `NuipConvertSTSkeletons` defaults for `dwEnrollmentIndex`, `dwUserIndex` and joint `w`;
  - the `liTimeStamp` source and units in `NuipPostProcessSkeletonFrame`;
  - the tracked-skeleton default policy;
  - the HRESULTs of every facade function;
  - the `XamNuiGetDeviceStatus` "present" fields.
- Fix only the instruments: `XamXStudioRequest` returns failure (§2.2.1). That is upstreamable
  and inert while the NUI table is active.
- Run one **probe** (not S1) with the table disabled (`--stub_nui_functions=false`) and the device
  absent, to record what the real SDK does on Xenia with no sensor. That is the honest
  "unplugged" baseline.
- *DoD:* the spec doc exists with an `nuiskeleton.s` line cited per row; the probe log is
  archived; S1 is unchanged (paired compare, no regression).

### Phase 1: device core and skeleton stream

- Create `src/xenia/kernel/nui/`: `NuiDevice`, `SkeletonStream`, `FrameClock`, `PoseSource`
  (Empty, Constant), and `NuiSdkFacade` with the `2.0.21173` table for the five lifecycle and
  skeleton functions plus GetNextFrame.
- The other 50 SDK entries move into the facade as "legacy-equivalent" stubs: the same return
  values as today, now resolved by SDK version and not by DC3 address. This keeps S1 behaviour
  equal while the dc3 table is deleted.
- Move IK telemetry off the NUI tick onto the fail-tripwire probe thread.
- Tests:
  - host unit tests for `EncodeSkeletonFrame` (golden 0xAB0 bytes, BE floats, stride 0x1C0);
  - host unit tests for the DC3_20→NUI permutation;
  - host unit tests for the `AssignSlots` policy;
  - a resolver test that the 2.0.21173 table resolves to the `symbols.txt` addresses on the
    original XEX, extending `titles/dc3/testing/dc3_nui_patch_resolver_test.cc` into a
    `kernel/nui/testing` target.
- *DoD:*
  - common DoD with `nui.get_next_frame`, `skel.wait_33ms`, the 5 lifecycle/skeleton
    `nui.*` ids and the dc3 table gone;
  - the log shows `NUI HLE: NUI 2.0.21173 resolved N/N` and FrameClock ~30 events/s;
  - the override audit shows the GetNextFrame handler hit ≈ events set (the worker wakes per
    frame rather than on a 33 ms timeout);
  - `skel.is_override_nop` A/B'd and deleted if S1 holds;
  - G6 (4 MiB stack) A/B'd.

### Phase 2: faithful frames and native calibration

- Correct per-joint states, `w=1`, quality flags, hip `Position`, ms timestamps, the floor plane,
  stable ids and the tracking policy.
- *DoD:*
  - common DoD with the 6 `calib.*` ids, `game.pause_for_skeleton_loss` and
    `seq.controller_mode` deleted;
  - S2's DTA channel reports that player 0's skeleton tracking id equals the emulated id after
    song select;
  - no `pause_game` from `PauseForSkeletonLoss` in the run;
  - `SkeletonQualityFilter` validity observed true (a DTA query of `gesture_mgr` skeleton
    validity, or the existing telemetry).
- *If calibration still fails:* check whether the autonav nav bridge skips the calibration screen
  entirely (`seq.nav_bridge` jumps straight to game_screen). If it does, the gap is harness-owned.
  Fix it in the harness, by driving the real chooser with a raised-hand pose
  (`synthetic_kinect.RAISE_RIGHT`), not with a calibration patch.

### Phase 3: complete the SDK surface and layer K

- ImageStreams, CameraModel (motor + XOVERLAPPED), properties and ROI, IdentityModel,
  SpeechModel (silent), Audio/Fitness/Wave/Head unavailable codes.
- XAM app `0xFE` messages; `PsCam`/`Mic`/`Rmc` not-connected status.
- *DoD:*
  - common DoD with **zero** `DC3 HACK on: nui.` lines (all 56 gone), `speech.grammar_unload`
    deleted (or filed as a flow-order finding with evidence);
  - the transitional cvars deleted;
  - S2 passes;
  - S1V reaches title (no new Milo fail screen attributable to NUI);
  - the decomp layout untouched (S3 still 627).

### Phase 4: pose sources (b) and the parity instrument

- Add `SocketPoseSource`, `TapePoseSource`, `tools/nui/tape.py`, `--nui_frame_dump`,
  `tools/nui/parity.py`, and the `titles/dc3` song-clock plug-in.
- *DoD:*
  - the common DoD holds with `nui_pose_source=socket:<path>` served by `synthetic_kinect.py`
    (stand pose);
  - a native run and a Xenia run fed the same tape produce frame dumps whose decoded joints are
    identical for every common frame id (tolerance 0: the floats are copied, not computed);
  - the expected-divergence classes of §2.5 are listed in the parity report.

### Phase 5: decomp layout and a second title

- Add the decomp-layout signature variants and delete the 85 decomp NUI entries plus the 13 pack
  stubs.
- S3's trap count and histogram are a fingerprint of the decomp boot. The change is expected to
  move them, so **re-baseline S3 in the same commit, with the decision recorded**.
- Run DC1 TU0 with `--nui_device_present=true`. Either it resolves an older NUI version table
  (add it) or it logs `unsupported NUI <ver>` and stays on layer K. Either way, no DC1-specific
  code is added.
- *DoD:* common DoD; S3 re-baselined; S6.1 unchanged with the device off; the DC1 probe log
  archived.

### Phase 6 (optional research)

- Colour and depth frames for photos: D3D texture headers in physical memory, a silhouette
  rendered from the pose.
- Scripted speech recognition via `NuiSpeechEmulateRecognition` (needs `NUI_SPEECH_EVENT`
  semantic trees in guest memory).
- The guide gesture.
- Separately, a B0 feasibility study: `PsCamDeviceRequest` emulation with synthetic depth,
  running the real ST pipeline. That is only useful as an Exemplar oracle, never as the parity
  path.

---

## 6. Risks

| risk | likelihood / impact | mitigation |
|---|---|---|
| A signature false match puts an override on the wrong function | low / severe | Require a static-lib version match, multi-word masked signatures plus an XREF anchor, and refuse on ambiguity. The `--nui_symbol_map` cross-check in CI on DC3 original and decomp |
| An SDK entry point outside the table is reached with an uninitialised `NuipRuntimeState` (the facade skips `NuipInitialize`) | medium / crash or heap damage | A tripwire override on every known SDK export, logged TAINTED. The DC3 entry set is closed and enumerated in §2.1.3 |
| The calibration hypothesis (§2.3.1) is wrong, so the `calib.*` patches are still needed | medium / phase 2 slips | Phase 2 DoD names the falsifier. A remaining gap is most likely the harness skipping the chooser screen, which is fixed in the harness, not with patches |
| The `PauseForSkeletonLoss` false positive persists because the harness never binds players | medium | Same. The game's own `HamGameData::AutoAssignSkeletons` runs only in EditMode (`MoveDir.cpp:846-848`), so the faithful fix is driving the chooser |
| The `speech.grammar_unload` assert is a flow-order bug, not a stub artifact | medium / one id survives | A/B in phase 3. If it persists, file it with the tripwire evidence; do not re-patch it |
| Timing and load sensitivity (S1 is load sensitive per the README) | medium | The FrameClock runs on the guest clock and GetNextFrame never blocks the main thread. The device adds one host thread at 30 Hz |
| Native/Xbox semantic gaps make the timeline diff look broken | high / analysis noise | §2.5 lists the expected classes. Parity is asserted on the *frames served*, not on game state at bind time |
| Struct-field semantics uncertain (`dwEnrollmentIndex` copied into `mClippedFlags`, `dwUserIndex`, `w`) | low | Phase 0 reads `NuipConvertSTSkeletons`; defaults follow the SDK |
| Three lanes are editing xenia concurrently (`xam_nui.cc`, `titles/dc3/*`) | high / merge conflicts | Phase 0 and 1 touch only the new `kernel/nui/` plus small `xam_nui.cc` edits. The dc3 table deletion lands last in its phase, rebased on the lane that owns `dc3_title.cc` |
| Photos and depth UI show nothing (no image frames) | certain / cosmetic | Accepted until phase 6. Null-safe in the game (`LiveCameraInput.cpp:943-966`) |
| Upstreaming the SDK facade is contentious (HLE of a statically linked library) | medium | Upstream layer K separately: it is plainly correct. Offer the facade with the Cxbx-Reloaded OOVPA precedent and per-version tables |

---

## 7. Evidence index

### xenia (`main` 1f309687c)

- `src/xenia/kernel/xam/xam_nui.cc:28-60, 99-131, 226-230`
- `src/xenia/kernel/xam/xam_voice.cc:47-64`
- `src/xenia/kernel/xboxkrnl/xboxkrnl_misc.cc:44-48`
- `src/xenia/kernel/xboxkrnl/xboxkrnl_audio.cc:222-232`
- `src/xenia/kernel/xam/apps/xam_app.cc:22-107`
- `src/xenia/kernel/user_module.cc:537-544`
- `src/xenia/cpu/processor.h:119`
- `src/xenia/kernel/xevent.h:36`
- `src/xenia/kernel/kernel_state.h:172-176`
- `src/xenia/titles/dc3/dc3_title.cc:197-416, 727-837`
- `src/xenia/titles/dc3/dc3_nui_sequencer.cc:110-227`
- `src/xenia/titles/dc3/dc3_hack_pack_skeleton.cc:15-46`
- `src/xenia/titles/title_profile.cc:62-73`
- `docs/fork/dc3/PATCH_MANIFEST.md`
- `docs/fork/cleanup/DC3_HACK_GAP_ANALYSIS.md` (§L8, O1-O18)
- `tools/fork-regress/README.md:21-28`

### dc3-decomp (`main` a7cf32774)

Config and headers:
- `config/373307D9/symbols.txt:75,111,222,354,174679,174843-174844,187640`
- `config/373307D9/splits.txt:77, 7025-7227`
- `src/xdk/nui/nuiskeleton.h:21-117`
- `src/xdk/nui/nuidetroit.h:95-168`
- `src/xdk/xapilibi/winerror.h:30-44`

Game source:
- `src/system/gesture/LiveCameraInput.cpp:629-721, 893-927, 1205-1210`
- `src/system/gesture/SkeletonUpdate.cpp:116-133, 227-268, 496-557, 592-595`
- `src/system/gesture/Skeleton.cpp:46-54, 97-161, 519-570`
- `src/system/gesture/SkeletonQualityFilter.cpp:60-84`
- `src/system/gesture/SpeechMgr.cpp:53-63, 137-150, 188-238, 318-347, 626-634`
- `src/system/gesture/GestureMgr.cpp:215, 401-480`
- `src/system/gesture/CameraTilt.cpp:83-93, 150-160, 344`
- `src/lazer/game/Game.cpp:795-893`
- `src/lazer/meta_ham/ShellInput.cpp:170-183, 255-309`
- `src/lazer/meta_ham/SkeletonChooser.cpp:472-612`
- `src/lazer/meta_ham/SkeletonIdentifier.cpp:238-245`
- `src/lazer/meta_ham/VoiceControlPanel.cpp:205-216`
- `src/App.cpp:1395-1430`
- `orig-assets/extracted/config/kinect.dta`

Target asm listings (`build/373307D9/asm/xdk/`):
- `nuiapi/nuiskeleton.s:121-1000`
- `nuiapi/nuiruntime.s:941-961, 5106, 7624-7767, 8466, 9050-9354`
- `nuiapi/nuixam.s:31-138`
- `xapilibi/xnui.s:43-149`

Native port:
- `native/src/platform/Skeleton_Native.cpp:58-70, 210-357, 604-634`
- `native/src/platform/GestureMgr_Native.cpp:181-235, 256-376`
- `native/src/platform/PoseTarget_Native.cpp:1-36, 201-292`
- `scripts/synthetic_kinect.py:1-80, 286-345`

### RB3

- `rb3-xenon/config/45410914/symbols.txt` (no NUI)
- `rb3/config/SZBE69_B8/symbols.txt` (no NUI)

---

## 8. As built (phases 0-1, lane nui-hle)

Code: `src/xenia/kernel/nui/` (device, facade, frame, pose sources, signatures),
`tools/nui/gen_signatures.py`, wired in `Emulator::CompleteLaunch` before the title hooks.
Spec read from the SDK: `NUI_DEVICE_SPEC.md`. Measurements: `docs/fork/dc3/BASELINE.md`,
"Lane NUI-HLE".

What differs from the design above, each with its evidence:

| design said | as built | why |
|---|---|---|
| `NuiInitialize` with the sensor absent: S_OK, `connected=false` (§3.3.3) | returns `0x8301000D` | measured: the real SDK on Xenia with `XamNuiGetDeviceStatus` "not connected" returns it, and DC3 MILO_FAILs on it (phase-0 probe) |
| faithful policy: "before any selection, the first two persons are TRACKED" (§3.3.4) | with the title-sets-tracked flag nobody is TRACKED until `NuiSkeletonSetTrackedSkeletons`; without it, the two nearest (0.3 m hysteresis) | `NuiSkeletonTrackingEnable` clears the ids; `NuipPostProcessSkeletonFrame` skips the policy when `R+0x1F10` is set (`NUI_DEVICE_SPEC.md`) |
| per-version table hand-written from the resolver's 10+ signatures (§3.3.1) | generated from a split listing: every word of the first 24, relocated fields masked; thunks (body < 8 words, or not unique) carry an XREF anchor on their tail branch's target | 56/56 unique on DC3; `--nui_symbol_map` agrees 56/56 |
| tripwire overrides on known-but-unimplemented entries | none in phase 1: all 56 are installed (6 device-backed, `NuiIdentityGetEnrollmentInformation` faithful, 49 legacy return values) and the facade is all-or-nothing | a partial resolve installs nothing; SDK internals are only reachable through those 56 |
| ConstantPoseSource id 1 | id 5 | `synthetic_kinect.py` gives its first person id 5; the same pose and id on both sides |
| phase-2 "correct frames" as a separate step | the frames were correct from phase 1 (0xAB0, stride 0x1C0, joint states, `w=1`, ms stamps, hip `Position`, stable ids) | one encoder, unit-tested |

Phase 2 status. Code reading (NOT yet measured: the first exploration run, e2, died at boot of
finding 1 before reaching the title) points at controller mode, not calibration, as what made the title ignore confirms when `calib.*` was off:
`ShellInput::OnMsg(ButtonDownMsg)` swallows the first press made out of controller mode
(`EnterControllerMode`, `return 0`); `ShellInput::Poll` exits controller mode 5 s after the last
press (helpbar `controller_mode_timeout 5000`); `HamNavList` acts on pad input only in
controller mode. `seq.controller_mode` forces the flag on and `calib.exit_controller_mode`
stubs the exit; with only the second removed, the forced flag and the per-frame exit fight.
The experiments that decide it were queued at hand-off (outputs under
`/home/free/tmp/nui-hle-runs/exp/`): e2b (calib.* + pause off), e3 (also seq.controller_mode
off), e4 (the five non-controller calib ids + pause off), e6 (all off, with an `l3` press
before each screen's first action to enter controller mode the way a 360 player must).

Findings the design did not anticipate:

1. **`NuiIdentityGetEnrollmentInformation` must write its output.** The legacy S_OK stub left
   `NUI_ENROLLMENT_INFORMATION` unwritten; `SkeletonIdentifier::UpdateEnrolledPlayers` read
   uninitialised stack (a random path). Fixed in phase 1: `{0xFE, 0}` per `identityapi.s`.
2. **DC3's KinectGuideThread makes the SkeletonUpdate worker spin at boot, faithfully.** It
   calls `NuiSkeletonTrackingDisable` (which sets and drops the title event, `sk:120-167`) and
   `Enable(0, 0)`; the manual-reset `sNewSkeletonEvent` stays signaled with nobody to reset it,
   so the worker loops `GetNextFrame(0)` -> E_PENDING until `Enable(event, 2)` at the end of
   boot. The SDK does exactly this; the old stubs never set the event (`skel.wait_33ms` polled
   at 33 ms), so it never showed. On Xenia it is ~400k E_PENDING calls, but it is **not** what
   made the tip's title_screen 24-27 s against main's 15-18 s. That delta was the facade's
   symbol-map cross-check: `std::regex` over 211,849 lines of symbols.txt, 10.8 s in a Checked
   build, before any guest thread starts (fixed in `584a10ab2`). After the fix, S1 x3 interleaved
   with main gives title_screen 18/18/21 s against 18/15/18 s.
