# DC3 (Dance Central 3) NUI Boot Roadmap

## Status: Initial Stubs Implemented

DC3 requires Kinect (NUI) for gameplay. On Xenia, the game boots but loops at init doing VdSwap with zero draw commands. This roadmap tracks the stubs needed to get past init.

## Implemented Stubs

### XAM (63 functions)

#### NUI Camera/Tilt (7)
- [x] `XamNuiCameraElevationSetAngle` → return 0
- [x] `XamNuiCameraElevationGetAngle` → *angle=0, return 0
- [x] `XamNuiCameraElevationStopMovement` → return 0
- [x] `XamNuiCameraTiltGetStatus` → return 0
- [x] `XamNuiCameraTiltReportStatus` → return 0
- [x] `XamNuiCameraTiltSetCallback` → return 0
- [x] `XamNuiCameraRememberFloor` → return 0

#### NUI Identity/User (5)
- [x] `XamNuiGetDeviceSerialNumber` → return E_FAIL
- [x] `XamNuiIdentityGetSessionId` → return 0
- [x] `XamUserNuiGetUserIndex` → return E_FAIL
- [x] `XamUserNuiGetEnrollmentIndex` → return 0
- [x] `XamUserNuiEnableBiometric` → return 0

#### NUI Biometric (2)
- [x] `XamReadBiometricData` → return error
- [x] `XamWriteBiometricData` → return 0

#### NUI UI Dialogs (9)
- [x] `XamShowNuiGuideUI` → return 0
- [x] `XamShowNuiSigninUI` → return 0
- [x] `XamShowNuiControllerRequiredUI` → return 0
- [x] `XamShowNuiFriendsUI` → return 0
- [x] `XamShowNuiGamerCardUIForXUID` → return 0
- [x] `XamShowNuiMarketplaceUI` → return 0
- [x] `XamShowNuiPartyUI` → return 0
- [x] `XamShowNuiDeviceSelectorUI` → return 0
- [x] `XamShowNuiDirtyDiscErrorUI` → return 0

#### Standard UI Dialogs (5)
- [x] `XamShowMessageBoxUIEx` → return 0
- [x] `XamShowFriendsUI` → return 0
- [x] `XamShowGamerCardUIForXUID` → return 0
- [x] `XamShowMarketplaceUI` → return 0
- [x] `XamShowMarketplaceDownloadItemsUI` → return 0

#### User/Profile (6)
- [x] `XamUserGetIndexFromXUID` → return error
- [x] `XamUserGetOnlineCountryFromXUID` → return error
- [x] `XamUserGetMembershipTierFromXUID` → return error
- [x] `XamUserGetCachedUserFlags` → return 0
- [x] `XamProfileCreateEnumerator` → return error
- [x] `XamProfileEnumerate` → return error

#### Voice/Mic (4)
- [x] `XamVoiceSubmitPacket` → return 0
- [x] `XamVoiceGetMicArrayAudioEx` → return error
- [x] `XamVoiceGetMicArrayUnderrunStatus` → return 0
- [x] `XamVoiceGetMicArrayStatus` → return 0

#### Input (3)
- [x] `XamInputSendStayAliveRequest` → return 0
- [x] `XamInputControl` → return 0
- [x] `XamInputRawState` → return error

#### Cache (3)
- [x] `XamCacheOpenFile` → return error
- [x] `XamCacheCloseFile` → return 0
- [x] `XamCacheReset` → return 0

#### LRC (3)
- [x] `XamLrcSetTitlePort` → return 0
- [x] `XamLrcVerifyClientId` → return 0
- [x] `XamLrcEncryptDecryptTitleMessage` → return 0

#### XLFS (4)
- [x] `XamXlfsInitializeUploadQueue` → return 0
- [x] `XamXlfsUninitializeUploadQueue` → return 0
- [x] `XamXlfsMountUploadQueueInstance` → return 0
- [x] `XamXlfsUnmountUploadQueueInstance` → return 0

#### Network (8)
- [x] `NetDll_XnpGetConfigStatus` → return 0
- [x] `NetDll_getpeername` → return -1
- [x] `NetDll_getsockname` → return -1
- [x] `NetDll_getsockopt` → return -1
- [x] `NetDll_XNetGetConnectStatus` → return 0
- [x] `NetDll_XNetConnect` → return 0
- [x] `NetDll_XNetUnregisterInAddr` → return 0
- [x] `NetDll_XNetServerToInAddr` → return -1

#### Misc XAM (4)
- [x] `XamBackgroundDownloadSetMode` → return 0
- [x] `XamXStudioRequest` → return 0
- [x] `XamGetActiveDashAppInfo` → return error
- [x] `XNetLogonGetTitleID` → return error

### XBOXKRNL (39 functions + 2 variables)

#### Event Tracing (3)
- [x] `EtxProducerRegister` → return 0
- [x] `EtxProducerUnregister` → return 0
- [x] `EtxProducerLog` → no-op

#### Camera (1)
- [x] `PsCamDeviceRequest` → return error

#### Object Manager (2)
- [x] `ObCreateObject` → return error (TODO: real impl)
- [x] `ObReferenceObject` → no-op (TODO: real impl)

#### Memory (1)
- [x] `ExAllocatePoolWithTag` → SystemHeapAlloc

#### Variables (2)
- [x] `ExEventObjectType` → dummy type pointer
- [x] `ExThreadObjectType` → dummy type pointer

#### Timers (3)
- [x] `KeInitializeTimerEx` → zero struct (TODO: real impl)
- [x] `KeCancelTimer` → return 0 (TODO: real impl)
- [x] `KeSetTimer` → return 0 (TODO: real impl)

#### Mutants (2)
- [x] `KeInitializeMutant` → zero struct (TODO: real impl)
- [x] `KeReleaseMutant` → return 0 (TODO: real impl)

#### FPU State (2)
- [x] `KeSaveFloatingPointState` → no-op
- [x] `KeRestoreFloatingPointState` → no-op

#### Exception Handling (3)
- [x] `RtlCaptureContext` → zero context (TODO: real impl)
- [x] `RtlUnwind` → no-op (TODO: real impl)
- [x] `__C_specific_handler` → return ExceptionContinueSearch (TODO: real impl)

#### Crypto (3)
- [x] `XeKeysAesCbc` → return error (TODO: real impl)
- [x] `XeKeysSetKey` → return 0
- [x] `XeKeysGetConsoleID` → dummy ID

#### LDI Decompression (3)
- [x] `LDICreateDecompression` → return error (TODO: LZX support)
- [x] `LDIDecompress` → return error
- [x] `LDIDestroyDecompression` → return 0

#### Audio (13)
- [x] `XAudioCaptureRenderDriverFrame` → return 0
- [x] `XAudioGetRenderDriverTic` → return 0
- [x] `XAudioUnregisterRenderDriverMECClient` → return 0
- [x] `XAudioRegisterRenderDriverMECClient` → return 0
- [x] `XAudioOverrideSpeakerConfig` → return 0
- [x] `XAudioGetDuckerLevel` → return 0
- [x] `XAudioGetDuckerReleaseTime` → return 0
- [x] `XAudioGetDuckerAttackTime` → return 0
- [x] `XAudioGetDuckerHoldTime` → return 0
- [x] `XAudioGetDuckerThreshold` → return 0
- [x] `MicDeviceRequest` → return error
- [x] `RmcDeviceRequest` → return error

#### String (2)
- [x] `RtlUpcaseUnicodeChar` → real impl (toupper)
- [x] `RtlDowncaseUnicodeChar` → real impl (tolower)

### Key Change
- [x] `XamNuiGetDeviceStatus` → now reports **connected** (status=1)

## Bugs Found & Fixed

### 1. ObReferenceObjectByHandle Type-Checking Bug (Critical)

**Symptom:** DecompressionThread (thread 8) spinning at 99.6% CPU. `WaitForSingleObject` was returning immediately instead of blocking.

**Root Cause:** `ObReferenceObjectByHandle` in `xboxkrnl_ob.cc` compared the `object_type_ptr` parameter against hardcoded sentinel values (e.g., `0xD00EBEEF`). However, games pass the **guest address** of the `ExXxxObjectType` export variable (e.g., `0x0002A000`), NOT the sentinel value stored within it.

The call chain was:
```
WaitForSingleObject → NtWaitForSingleObjectEx → ObReferenceObjectByHandle
```

`ObReferenceObjectByHandle` returned `STATUS_OBJECT_TYPE_MISMATCH` (0xC0000024) because it compared `0x0002A000` (the variable address the game passed) against `0xD00BEEF1` (the sentinel stored in the variable). This caused `WaitForSingleObject` to fail instead of blocking, so the decompression thread's wait-loop became a busy-spin.

**Fix:** Look up the actual export variable guest address via `export_resolver->GetExportByOrdinal()` and compare against that. The ordinals are:
- `ExEventObjectType` = 0x0E
- `ExSemaphoreObjectType` = 0x17
- `ExThreadObjectType` = 0x1B

**Diagnostic trace that revealed it:**
```
[DC3-DBG] ObRefByHandle MISMATCH: handle=F8000088 obj_type=12
  expected_type_ptr=0002A000 actual_type_ptr=D00BEEF1
```

### 2. Symlink Handling in VFS (filesystem_posix.cc)

**Symptom:** Game assets symlinked into the XEX directory were invisible to Xenia.

**Root Cause:** `HostPathDevice::PopulateEntry` used `ent->d_type == DT_DIR` to detect directories, but symlinks have `d_type == DT_LNK`.

**Fix:** Use `S_ISDIR(st.st_mode)` from the `stat()` call that was already being made.

### 3. Missing ExSemaphoreObjectType Init (xboxkrnl_module.cc)

`ExSemaphoreObjectType` was not being initialized in `XboxkrnlModule` constructor. Added allocation + `SetVariableMapping` for ordinal 0x17.

### 4. Missing DmGetSystemInfo Stub (xbdm_misc.cc)

DC3 is a debug build that calls `DmGetSystemInfo`. Added stub that zeroes 0x24 bytes and returns success.

## Current Status (Session 38 — 2026-02-25)

### Boot progression
```
mainCRTStartup
  -> _cinit                         ✓ (gStringTable + gHashTable host-constructed)
  -> main()                         ✓
  -> SystemPreInit                  ✓ (archive loading, config parsing, MemInit)
  -> SystemInit                     ✓ (Symbol::Init, object factories)
  -> Game initialization            ✗ CURRENT BLOCKER: factory instantiation + DTB issues
```

### What works
- Full CRT initialization (host-constructed StringTable/gHashTable bypass)
- File system: disc mode (`gUsingCD=1`), archive loading (`.hdr`/`.ark` files)
- Config parsing: `ham_preinit_keep.dta` loads successfully
- Memory: MemInit allocates 16MB heap growth region
- 16+ guest threads spawned, DecompressionThread properly blocks
- VdSwap running at ~26fps
- Fake Kinect skeleton injection (`--fake_kinect_data=true`) working
- **Manifest address automation**: 73/67 kAddr fields auto-resolved, 89 hack_pack_stubs

### Current blockers
1. **Object factory instantiation**: `Couldn't instantiate class Mat/MetaMaterial/Cam` — sFactories appears populated but lookup fails
2. **`Data is not Array` cascades**: DTB config structure issues during game init
3. **~200 guest-code patches**: ~130 permanent (headless), ~25 fixable (decomp), ~8 fixable (APU)

### Hack pack summary (~200 patches)
| Tier | Count | Category | Status |
|---|---|---|---|
| 1: Permanent | ~130 | NUI/Kinect, Holmes, GPU null, XBC, XMP, Bink | Required for headless — no action needed |
| 2: Decomp artifacts | ~25 | /FORCE linker corruption, CRT ordering | Fixable by resolving 275 LNK4006 + 666 LNK2001 |
| 3: XAudio2 cascade | ~8 | Synth CS deadlock, Splash I/O | Fixable by xenia nop APU CS init fix |

## Next Steps

1. **Fix object factory instantiation** — investigate sFactories lookup failure and /FORCE duplicate addresses
2. **Fix `Data is not Array` cascades** — DTB schema validation / config parsing issues
3. **XAudio2 CS deadlock** — highest-leverage fix: one xenia APU change eliminates 3 stubs
4. **.milo file I/O completion** — second-highest leverage: fixes ~6 stubs (Splash, UIScreen, ObjRef)
5. **Decomp linker cleanup** — resolving LNK4006/LNK2001 eliminates most Tier 2 stubs
