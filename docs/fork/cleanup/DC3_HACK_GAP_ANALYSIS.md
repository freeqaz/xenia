# DC3 hack packs: which Xenia gap is each one masking?

**Tree:** `/home/free/code/milohax/xenia` `main` @ `fee3503b1`. Everything was read with `git show main:`;
copies are in `scratch/` next to this file. Nothing was built, run or committed.
**Cross-references:** dc3-decomp `config/373307D9/symbols.txt` (original `debug.xex` layout),
dc3-decomp `build/373307D9/default.map` (the decomp build of 2026-08-24 that S3 boots),
dc3-decomp `build/373307D9/asm/**` (dtk listings of the original image), dc3-decomp `src/`.
**Builds on:** `FORK_CLEANUP_PLAN.md` §2.1–2.4, `docs/fork-cleanup-review.md`,
`docs/dc3-oracle/BASELINE.md`, dc3-decomp `docs/runtime/XENIA_ASYNC_COMPLETION_STALL.md`,
`docs/plans/XENIA_ORACLE.md`, and the memory notes `project_xenia_async_stall`, `project_xenia_timer_blocker`.
**Date:** 2026-10-02.

Confidence tags: **V** = VERIFIED (code read plus commit, measurement or disassembly evidence);
**I** = INFERRED (mechanism reasoned from code and docs, not yet measured).

---

## 0. Headline findings

1. **Two root causes named in the brief have already been refuted by Xenia's own history.**
   - **The OVERLAPPED claim.** "Xenia `NtReadFile` never updates the guest `OVERLAPPED.Internal`" is
     refuted by **`ac0052e5b`** (2026-08-26). Its ABI probe showed that for every async read,
     `iosb == apc_ctx == the OVERLAPPED`: DC3's XAPILIB passes the OVERLAPPED itself as the
     IO_STATUS_BLOCK, so Xenia's ordinary `io_status_block->status` write already clears
     `Internal`. The same false rationale still appears in three places: the
     `io_force_synchronous_completion` help text (`xboxkrnl_io.cc:30-43`), the `CDReadDone` comment
     (`dc3_hack_pack.cc:2777-2784`), and the dc3-decomp memory note. As a result,
     `CDReadDone→1` and `io_force_synchronous_completion=true` have **no surviving mechanism**.
   - **The `__mftb` claim.** "`__mftb` frozen" was retracted on 2026-04-06
     (`project_xenia_timer_blocker`: the timebase is driven by host `rdtsc`).
2. **A real POSIX threading bug, fixed in core only yesterday, is the most likely root of 9 of the
   original-layout hacks.**
   - **The fix.** `c12de876f` (2026-10-01) fixes an upstream lost-resume race: a thread created
     suspended and then resumed could never run. DC3's own boot showed it on the Main XThread and
     on KinectGuideThread ("created suspended, XSetThreadProcessor, ResumeThread"). Boot health
     went from 7/13 to 16/16.
   - **The match.** `Splash::BeginSplasher` (`Splash.cpp:227-231`) and `BinkStartAsyncThread` use
     exactly that pattern: CreateThread suspended, XSetThreadProcessor, ResumeThread, then wait.
     Every hack in the Bink/Splash/MoviePanel/GotoFirstScreen chain predates the fix, and each
     one's rationale says "hangs headless" or "intermittent, 4/5 boots" (**I**, strong).
3. **The decomp-layout pack is not evidence of Xenia gaps.**
   - **Defects of the rebuilt image.** Of its 173 active stopgaps, 149 (86%) are defects of the
     rebuilt image itself:
     - `/FORCE:MULTIPLE` merging of `link_glue` ALTERNATENAME no-op stubs over real code;
     - `build_xex.py` resolving only ~196 of 379 imports;
     - CRT `__xc` initializers that never run;
     - decomp DTB checksums.
   - **The discriminator.** The *original* `debug.xex` runs every one of those functions unpatched,
     under the same Xenia, to gameplay (S1).
   - **What the 627-trap bar really counts.** Its LR histogram resolves (against the 2026-08-24
     decomp map) into MSVC CRT/XAPI static-library code. The top sites are `_lock` ×271,
     `onexit` ×271, `_getptd_noexit` ×15, `__crtInitCritSecAndSpinCount` ×14, `rtlheap` ×22, and
     XAPI time/file helpers. The original image executes the same library code trap-free. So the
     bar measures how far the decomp image's CRT/import init gets, not Xenia (**V** location,
     **I** mechanism).
4. **The largest set of original-layout hacks with one cause is self-inflicted.** They exist
   because host code runs guest UI code on the SkeletonUpdate worker (`processor->Execute` from
   `Dc3NuiSequencerExtern`). That produces:
   - the "BinkMovieImpl::Ready called in the wrong thread" FAIL that the `Debug::Fail` patch then
     latches (`mFailing` stuck at 1, so every later MILO_FAIL is silent: BASELINE, measured);
   - the 256 KB worker-stack overflows behind the DC3 4 MB stack clamp (`671a31585`), from deep
     UI chains running on a stack sized for skeleton work;
   - the `merge_busy` hold;
   - the `ObjectDir::FindObject` race (BASELINE k1).

   It is not a Xenia gap. The fix is to drive automation from the guest main thread, through the
   `HolmesClientPollKeyboard` hook the DTA channel already uses.
5. **The genuine title requirement is Kinect.** 69 original-layout hacks are NUI device absence or
   its knock-on effects:
   - 56 NUI SDK overrides;
   - the constant-pose `GetNextFrame`;
   - the 33 ms wait patch;
   - 6 SkeletonChooser calibration patches;
   - `PauseForSkeletonLoss`;
   - speech `Grammar::Unload`;
   - `mInControllerMode`.

   Seven more are harness navigation that exists only because DC3 menus need gestures. The proper
   fix is a title-agnostic **NUI HLE module with a pose source**, not per-address stubs. Below the
   API level, the faithful device route is to feed depth frames to `PsCamDeviceRequest` so the
   statically linked guest tracker runs; that is possible but XL.
6. **Audio is one cluster that should collapse.** Seven hacks live in it: `XMAHALAllocateContexts→0`,
   the `--nop_audio_driver` auto→dummy override, `HamAudio::IsReady+0x70`, `HandleWait+0x90`,
   the unpause nudge, and both 120 BPM beat drives. The XMA stub is now self-defeating: `c2c8c87d4`
   shows that the paced driver faults *because* the contexts are stubbed. Xenia already has the
   pieces (an XMA decoder for every APU backend, and a paced nop driver).
7. **The fork's override API has a hole that can make "retirement" A/Bs lie.**
   `RegisterGuestFunctionOverride` → `GuestFunction::SetupExtern` binds direct `bl` sites.
   BASELINE measured the `BinkMovieImpl::Ready` guest body running anyway, via the vtable path,
   while the override was registered. Any hack reached only through `bctrl` may be partly inert
   today. Fix this before measuring which hacks can be removed.
8. **New genuine emulator gaps surfaced.** They are upstreamable.
   - **Guest C++ exceptions.** `RtlRaiseException(0xE06D7363)` → `xe::debugging::Break()`
     (`xboxkrnl_debug.cc:120-151`). There is no SEH dispatch or unwind, so MILO_TRY/MILO_CATCH
     cannot work. The DTA channel works around it with a `longjmp` throw hook.
   - **Cross-title content enumeration.** The path behind `ContentMgr::RefreshDone` is
     **V** by disassembly:
     - `XContentCreateCrossTitleEnumerator` delay-loads `xam.xex` ordinal `0x279`
       (`XamContentAggregateCreateEnumerator`, `kStub`);
     - `XEnumerateCrossTitle` → `XamGetPrivateEnumStructureFromHandle` (`kStub`);
     - → `ScheduleSystemRequest` → `XamTaskSchedule` (`kSketchy`);
     - → task `XMsgInProcessCall` → `XMsgCompleteIORequest`.

     That is four XAM HLE pieces in a row.
   - **Mounts the headless app leaves off by default.** `--mount_cache` and `--devkit_root` default
     off. Both explain decomp-pack entries (`ReadError`, `Locale::Init`).

---

## 1. Counts by gap class

### 1a. Original layout (`debug.xex`, the oracle): 104 hacks

| Gap class | Hacks | Which |
|---|---:|---|
| NUI/Kinect (no device: a title requirement, fixed by device/API emulation) | 69 | 56 NUI overrides; `GetNextFrame` extern (constant pose); dead PPC GetNextFrame stub; 33 ms wait; IsOverride nop; `mInControllerMode` write; `Grammar::Unload`; 6 SkeletonChooser/ShellInput calibration patches; `PauseForSkeletonLoss` |
| Threading (POSIX thread lifecycle; fixed by `c12de876f`, A/B pending) | 9 | Splash ×4, `BinkMovieSys::Init`, `BinkMovieImpl::Ready`, `MoviePanel::IsLoaded`, `Movie::Poll`, `GotoFirstScreen` override |
| XMA/audio (headless audio pipeline) | 7 | `XMAHALAllocateContexts`, nop-driver auto→dummy, `HamAudio::IsReady+0x70`, `HandleWait+0x90`, unpause nudge, beat drive A, beat drive B |
| Genuine headless/test-harness need (Kinect-less navigation) | 7 | nav bridge, transition force, LoadSong repair, attract A-press, attract→title force, `SongAnim`→EXPERT, content wipe |
| Self-inflicted (host→guest calls off the main thread) | 4 | `Debug::Fail` spin, `HamPanel::FocusComponent`, `HamScreen::IsEventDialogOnTop`, `merge_busy` hold (and the core 4 MB stack clamp, §3) |
| Kernel/HLE semantics (XAM LRC / SmartGlass) | 3 | `CXbcImpl::Initialize/DoWork/SendJSON` |
| Content/VFS + XAM async (enumeration, saves) | 2 | `ContentMgr::RefreshDone`, `SaveLoadManager::Activate` |
| Async I/O completion (mechanism refuted) | 1 | `CDReadDone` |
| Decomp-only (inert on this layout) | 1 | MMIO soft-fault range `0x83320000-0x836C0000` (outside the original image) |
| Diagnostic/instrument (not a gap; wrong addresses) | 1 | IK telemetry caves |

### 1b. Decomp layout (`ApplyDc3HackPack`): 173 active stopgaps, plus 85 decomp-table NUI entries in `emulator.cc`

| Gap class | Stopgaps | Which |
|---|---:|---|
| **Decomp-build defect** (the fix belongs in dc3-decomp, or the pack is deleted) | **149** | link_glue/`/FORCE` corruption, unresolved imports, CRT init, host allocators, Holmes, STL sentinels, GPU-init stubs (original runs them on `--gpu=null`), CS overrides, etc. (§2b) |
| NUI/Kinect | 13 | LiveCameraInput ×2, GestureMgr ×3, SkeletonIdentifier ×2, SkeletonUpdate ×2, SkeletonHistoryArchive, ShellInput::Init, VoiceInputPanel::LoadVoiceContexts, CRT "NUI skip" of 191 `__xc` slots (XDK overrides counted under decomp-build) |
| Threading (lost-resume, likely) | 7 | decomp Splash ×4, `BinkStartAsyncThread`, `BinkMovieSys::PlatformInit`, decomp `GotoFirstScreen` ("ChunkStream threads fail to start") |
| Harness config (an existing cvar covers it) | 3 | `DebugBreak` (`--break_on_debugbreak`), `ReadError` (`--mount_cache`), `Locale::Init` (`--devkit_root`) |
| Async I/O (refuted) | 1 | `CDReadDone` in the debug stub table |
| Off by default / diagnostics (not counted above) | ~9 | ReadCacheStream null/probe, MemOrPool probe, FindArray modes, SystemConfig probe, DataInput dump, Debug::Print redirect, JIT-indirection read, `_cinit` dump |
| The 85 decomp NUI table entries (`emulator.cc` ~6054-6166) | 85 | Same NUI functions at decomp addresses; they die with the decomp layout |

### 1c. Core "forced gates" DC3 depends on (all titles, defaults ON): 13 (§3)

---

## 2. Per-hack tables

### 2a. Original layout

Line numbers refer to `main@fee3503b1`. "Guest fn" is resolved through `symbols.txt` (**V** for every
address below unless marked).

| # | Hack (where) | Guest fn | Forces | Why it was added (evidence) | Gap underneath | Proper fix (where · size · upstream?) | Conf |
|---|---|---|---|---|---|---|---|
| O1 | NUI table, 56 entries (`emulator.cc:5901-6046`, override loop) | `NuiInitialize`…`NuiMetaCpuEvent` (SDK statically linked) | 44 return 0, 12 return -1 | Asserts on HRESULT failure in `LiveCameraInput` ctor and `SpeechMgr`; no device | **NUI/Kinect.** DC3 imports `XamNui*`, `PsCamDeviceRequest`, `MicDeviceRequest`, `RmcDeviceRequest`, `XamVoiceGetMicArray*`; all are stubs | `src/xenia/kernel/nui/` NUI HLE module: signature-matched SDK API (the resolver already matches by signature), device-present cvar, valid handles and events, and pose/speech sources. L · partly upstreamable (Kinect titles) | V |
| O2 | `NuiSkeletonGetNextFrame` → `Dc3NuiSequencerExtern` (`emulator.cc:3155-3251`) | `NuiSkeletonGetNextFrame` 0x829C2790 | One tracked skeleton, constant 20-joint pose; also the host's per-frame tick | Fake input for gameplay | NUI | NUI HLE pose source (constant / recorded / `DC3_POSE_SOCKET`); move the "tick" duties out (see O30-O34) | V |
| O3 | PPC GetNextFrame stub (`dc3_hack_pack_skeleton.cc:103-204`) | same | Copies a constant frame | Pre-override version | none (dead: O2 registers first) | Delete | V (plan §2.2) |
| O4 | `0x8242E74C ← li r28,0x21` (`skeleton.cc:211`) | `SkeletonUpdateThread`+0xA4 | INFINITE wait becomes 33 ms | Worker waits on `sNewSkeletonEvent`, which the stubbed `NuiSkeletonTrackingEnable` never stores or sets | NUI | NUI HLE signals the next-frame event from a 30 Hz host timer, leaving the wait unpatched. S | V (memory note) |
| O5 | `0x8242E1B0` nop (`skeleton.cc:214`) | `SkeletonUpdate::Update`+0x40 | Skips the "IsOverride" branch | Feb bring-up (`a224a6846`) | NUI | Retires with the HLE once frames carry valid tracking state; A/B | I |
| O6 | GestureMgr `mInControllerMode := 1` per NUI call (`emulator.cc:3282-3297`) | `TheGestureMgr` 0x82F5F7B4 +0x426D | Forces controller mode so pad A navigates | DC3 menus are gesture-driven | NUI / harness | Harness: DTA call on the main thread. Faithful: gesture input from the NUI HLE | V |
| O7 | `SpeechMgr::Grammar::Unload` → blr (`emulator.cc:6829`) | 0x82439F38 | Skips Unload | `eb5653f9e` "title bypass crash". Stubbed `NuiSpeechLoadGrammar`/`StartRecognition` return S_OK, so `mLoaded`/recognising are set and the `!IsRecognizing()` assert fires (`SpeechMgr.cpp:55`) | NUI (self-inflicted by O1's S_OK stubs) | NUI HLE speech state machine (start/stop/unload consistent). S once HLE exists | I |
| O8-O13 | Calibration bypass (`emulator.cc:6871-6961`): SetPlayerPresent guard nop; ChoosePlayerSides, SetPlayerSkeletonWarningData, ExitControllerMode → blr; SetPlayerSkeletonNavData rewritten as 2× SetPlayerPresent; ShouldWaitForRecovery → 0 | `SkeletonChooser::*` 0x8290834C/0x82909968/0x82907880/0x82909340/0x82904CD0; `ShellInput::ExitControllerMode` 0x82902748 | Marks players present, no recovery wait | One constant skeleton is not a calibrated player | NUI | NUI HLE emits realistic frames (≥1 tracked id, plausible floor/hip positions, stable ids), so SkeletonChooser calibrates natively. M | I |
| O14 | `Game::PauseForSkeletonLoss` → blr (`emulator.cc:6978`) | 0x82866D50 | No auto-pause | `4123b97f9`: the fake skeleton is never registered "playing", so `CheckForSkeletonLoss` pauses about 2 s in | NUI | Same as O8-O13 (player registered as playing) | V (commit) |
| O15-O17 | `CXbcImpl::Initialize/DoWork/SendJSON` → 0 (NUI table) | 0x82606078 / 0x82605960 / 0x82605DF8 | SmartGlass init succeeds | "Failed to initialize Xbox SmartGlass library" crash | **Kernel/HLE (XAM LRC)**. `XamLrc*` became `kStub` exports in `6394d2a7f`; the failure may now be elsewhere (XNet/socket) | A/B without them. If still needed, implement `XamLrc*` / `XNetStartup` semantics so `CXbcImpl` succeeds natively. S-M · upstreamable | I |
| O18 | `Debug::Fail` spin `0x825CE2DC ← b +0x90` (`skeleton.cc:215-228`) | `Debug::Fail`+0x10C | Worker returns instead of the devkit "wait for debugger" spin | `07b11d791`. **Trigger measured:** `mFailThreadMsg = 'BinkMovieImpl::Ready called in the wrong thread (expected 6, cur 15)'` (BASELINE). Skips `MemPopHeap` and `mFailing=0`, so every later FAIL is silent | **Self-inflicted** (host→guest UI calls on the worker) plus the genuine headless fact that no debugger attaches | Remove the trigger (L5), restore the faithful spin, add the `TheDebug+0x104` tripwire. Fallback is plan §2.4(c) | V |
| O19 | `BinkMovieSys::Init` → `li r0,1; stb r0,4(r3); blr` (`emulator.cc:6850-6865`) | 0x82E214A8 | Sets `isInitalized`, skips `BinkSetMemory`, `PlatformInit`, CS creation and **`BinkStartAsyncThread`** | `8cc604e0e`/`6323f54ac`: "BinkStartAsyncThread hangs headless" | **Threading**: suspended-create + XSetThreadProcessor + Resume + wait, exactly the lost-resume pattern fixed in `c12de876f` | Remove the patch on a post-`c12de876f` binary and confirm the async threads start (log `XThread::Execute` for both cores). XS once the fix is in. The fix is upstreamable (upstream has the same code) | I (strong) |
| O20 | `BinkMovieImpl::Ready` → 1 (`skeleton.cc:272-291`) | 0x82E221C8 | Movie always "ready" | `fe6428fec`: attract movie read "never completes" | Threading (O19 starved Bink of its async thread). **Override partly bypassed**: guest body still ran via vtable (BASELINE) | Retires with O19 | I |
| O21 | `MoviePanel::IsLoaded` → 1 (`skeleton.cc:293-315`) | 0x82E0EFE8 | Panel always loaded | `5084c6acd`: "Bink framebuffer setup INTERMITTENT, 4/5 boots stall" | Threading (intermittent = race) | Retires with O19 | I |
| O22 | `Movie::Poll` → 0 (`emulator.cc:6989`) | 0x82555CB8 | No movie polling | `69f8fdae2` (restore; rationale lost) | Threading / Bink chain | Retires with O19 | I |
| O23-O26 | Splash `PrepareNext`→0, `BeginSplasher`/`Suspend`/`Resume`→blr (`emulator.cc:6791-6826`) | 0x82554388 / 0x825554C8 / 0x82553BE0 / 0x82553D68 | No threaded loading-screen renderer | Carried from decomp (`8cc604e0e`). `BeginSplasher` = `CreateThread(...,4=SUSPENDED)`; `XSetThreadProcessor(5)`; `ResumeThread`; `WaitForState(kResumed)` (`Splash.cpp:219-231`) | **Threading** (lost-resume); possibly GPU device handoff second | A/B removal on a post-`c12de876f` binary; if it still hangs, instrument `D3DDevice_Suspend/Resume` and `CBlocker` | I (strong) |
| O27 | `UIManager::GotoFirstScreen` gated host override (`skeleton.cc:341-356`) | 0x8277B140 | Navigates only after `FindObject("attract_screen")` | `5084c6acd`: "boot-ordering race, ~5/6 boots" | Threading race (lost-resume) and/or self-inflicted; it also Executes `FindObject`/`GotoScreen` itself | Remove after O19-O26 are gone and boots are 16/16. M | I |
| O28 | `HamPanel::FocusComponent` → `b UIPanel::FocusComponent` (`emulator.cc:6748`) | 0x828EFE90 → 0x827A6310 | Skips the EventDialog lookup | `e82551f69` "focus crash": `TheHamUI.EventDialogPanel()` null (`HamPanel.cpp:42-43`) | **Self-inflicted** (screens forced before HamUI finished loading) or silenced-assert fall-through | Retires with L5 (game-driven navigation); A/B | I |
| O29 | `HamScreen::IsEventDialogOnTop` → 0 (`emulator.cc:6763`) | 0x829626D8 | Same | `e097276d7`; `HamScreen.cpp:61-62` asserts `event_dialog` | Same as O28 | Same | I |
| O30 | Transition force (`emulator.cc:3528-3618`) | writes `UIManager`+0x2C/+0x48/+0x4C; Executes `UIScreen::Enter` | Completes stuck transitions | Screens never reached `CheckIsLoaded` (Bink chain, content) | Harness (+ threading chain) | Delete after O19-O27; any residual automation goes main-thread behind `--dc3_headless_autonav` | V |
| O31 | Nav bridge `GotoScreen` (`emulator.cc:3895-4088`) | Executes 0x8277B378 from the worker | Walks attract→…→game_screen | Kinect-only menus; scripted A does not navigate (memory note) | **Genuine harness need** caused by NUI; executed on the wrong thread | DTA-channel `{goto ...}` on the main thread (L5); later NUI gesture scripting | V |
| O32 | `merge_busy` hold (`emulator.cc:3339-3356`, `3948`) | reads `TheFileMergerOrganizer` | Delays the forced game_screen until the song merge ends | `4123b97f9`: forcing game_screen early corrupted `mSongAnims` | Self-inflicted by O31 | Dies with O31 (the game drives its own transition) | V |
| O33 | LoadSong repair (`emulator.cc:4090-4333`) | Executes `DataReadFile`, `HamSongMgr::AddSongs`, `GetShortNameFromSongID`, `SetAssociatedPadNum` ×2 | Injects ymca (7011) | Song select needs gestures; `RefreshSynchronously` "blocks forever" | Harness (NUI) + content (O36) | DTA `{set_song ymca}` on the main thread; delete | V |
| O34 | `HamDirector::SongAnim` → `li r4,2; b SongAnimByDifficulty` (`emulator.cc:7043-7066`) | 0x82475578 | Always the EXPERT song.anim | "Routine-builder anim empty headless (remixer never runs)": a consequence of O33's injected song/difficulty | Harness (self-inflicted by O33) | Drive difficulty and player setup through the real flow or DTA; delete. **Perturbs anim selection** | I |
| O35 | `CDReadDone` → 1 (`emulator.cc:6773`) | 0x826026E0 | Async ARK block read always "done" | Copied from decomp `kAddr` (`41e5c2f0d`). Rationale = "separate IOSB, Internal stays PENDING" | **Async I/O: mechanism refuted** by `ac0052e5b`. With Internal = SUCCESS, `GetOverlappedResult` returns TRUE (`CDReader.cpp:71`) | Delete (A/B). If a stall appears, it is a new finding: log `NtReadFile` status/iosb for the BlockMgr reads | V (refutation) / I (unneeded) |
| O36 | `ContentMgr::RefreshDone` → 1 (`emulator.cc:6781`) | 0x825FEB48 (`mState == kDiscoveryEnumerating`) | Content discovery always "done" | `69f8fdae2`; memory note "enumeration never completes" | **Content/XAM.** Path **V** by disassembly: `0x279 XamContentAggregateCreateEnumerator` (kStub) → `XamGetPrivateEnumStructureFromHandle` (kStub) → `XamTaskSchedule` (kSketchy) → `XMsgInProcessCall` → `XMsgCompleteIORequest`. `PollRefresh` needs `XGetOverlappedResult ≠ 0x3E4` and ≥6 mounted contents (`ContentMgr_Xbox.cpp:463-596`) | Instrument `mState`, then fix whichever XAM piece leaves the overlapped incomplete (likely `XMsgInProcessCall` for the aggregate-enumerate message or `XamTaskSchedule`). M · upstreamable | I (path V) |
| O37 | `SaveLoadManager::Activate` → blr (`emulator.cc:6739`) | 0x82894A10 | No save/content load | Decomp rationale (`dc3_hack_pack.cc:4278-4285`): "garbage file size, **bug in decomp `CacheXbox::ThreadGetFileSize`**" — a decomp source bug, copied to the original block | Content/XAM (unknown on the original); the content wipe also empties saves each launch | A/B on the original with a private storage root; if it fails, trace `XamContentCreate*`/`XamCache*`. S | I |
| O38 | `XMAHALAllocateContexts` → 0 (`emulator.cc:7012-7020`) | 0x82E77250 | No XMA contexts | `69f8fdae2` (rationale lost). `c2c8c87d4`: with the paced driver the callback faults in `XMAHALWriteAndUnlockContexts` *because* contexts are stubbed | **XMA/audio** (self-defeating stub; Xenia's `XmaDecoder` exists for every backend) | Remove + `--nop_audio_driver=paced`; debug any real `XMACreateContext` failure. M | V (c2c8c87d4) |
| O39 | `--nop_audio_driver` auto → dummy (`emulator.cc:7032`) | – | Render callback never runs | `c2c8c87d4` | Consequence of O38 | Dies with O38 | V |
| O40 | `HamAudio::IsReady+0x70` bctrl → `li r3,1` (`emulator.cc:7040`) | 0x8252BA50 | Stream always ready | Memory note: "the .mogg async-read completions never land" (I/O; now refuted), so really the stream never reaches kReady with no audio pump | XMA/audio | Retires with O38 + paced driver | I |
| O41 | `HandleWait+0x90` bne → b (`emulator.cc:7038`) | 0x82867318 | Skips the audio-ready wait branch | Same as O40 | XMA/audio | Same | I |
| O42 | Beat drive A (`emulator.cc:4350-4455`) | `TheTaskMgr` 0x82F64A58 +0x48 := 0; seconds/beats/ui timelines +1/30 s | 120 BPM song clock | Song time comes from the audio stream, which never advances | XMA/audio | Paced audio advances the real clock; else one main-thread drive (plan §2.4). **Perturbs oracle** | V |
| O43 | Beat drive B (`nop_input_driver.cc:423-583`) | same fields, wall clock | Second 120 BPM drive | same | XMA/audio | Delete outright (plan §2.4) | V |
| O44 | Unpause nudge (`nop_input_driver.cc:642-664`) | game+0xA4/+0x60/+0x5E, gp+0xF8 | `mPaused=0`, `mRealTime=1` | `PostWaitStart` is only reached through `HandleWait` once audio is ready (`4123b97f9`) | XMA/audio | Retires with O38-O41 (the game unpauses itself) | V |
| O45 | Attract A-press fallback (`nop_input_driver.cc:858-877`) | pad input | Press A every 3 s | Attract stalls | Harness | Keep in the DC3 adapter, or drop after O19-O27 | V |
| O46 | Attract→title force (`nop_input_driver.cc:878-922`) | 4 MiB scan + UIManager stomp | Jumps screens | Attract stalls | Harness (+ threading chain) | Delete (plan) | V |
| O47 | Content wipe (`dc3_hack_pack.cc:41-50, 6695-6713`; call `emulator.cc:5781`) | host FS | `remove_all(<content>/373307D9)` | Decomp bring-up clean slate | Harness | Delete; the harness already uses a private `--storage_root` | V |
| O48 | MMIO soft-fault range (`emulator.cc:5791`) | `0x83320000-0x836C0000` | Makes writes there RW | "64KB-vs-4KB protect granularity" (decomp .data) | Decomp-only: **outside the original image** (resolves past every symbol in `symbols.txt`) | Move to the decomp archive; never arm on the original | V |
| O49 | IK telemetry caves (`emulator.cc:7071-7080` → `dc3_hack_pack.cc:5890-6150`) | caves at 0x8262F6B8 / hook 0x82631C58 | Telemetry | Instrument | Not a gap. **On the original, these addresses are inside `UtilDrawPlane` and `ObjDirItr<RndMat>::Advance`** | Allocate caves with `SystemHeapAlloc`; use original addresses (plan) | V |

### 2b. Decomp layout (`ApplyDc3HackPack`, `dc3_hack_pack.cc`)

**The discriminator rule applies throughout.** If the original image runs the same function unpatched
under the same Xenia to gameplay (S1), the stopgap is a defect of the rebuilt image. The pack's own
comments say so in most rows: "/FORCE:MULTIPLE", "ALTERNATENAME noop", "379 imports, ~196 resolved",
"__xc initializers ICF'd to blr", "BSS shifts", "corrupt vtables".

| Group (lines) | n | Items | Gap class | Why / evidence | Where the fix goes | Conf |
|---|---:|---|---|---|---|---|
| Import stopgaps (2796-2933) | 4 | `XapiCallThreadNotifyRoutines` stub (uninitialised `XapiThreadNotifyRoutineList`); `XRegisterThreadNotifyRoutine` stub (`XapiProcessLock` never initialised, so `_mtinit` deadlocks); `SetWind` blr (wind `Rand` never constructed); sweep stubbing every unrewritten PE thunk / XEX marker to `li r3,0` | Decomp-build | `build_xex.py` (last touched 2026-02-25) converts only ~196 of 379 imports; CRT/XAPI static data never initialised. The 627 traps sit in exactly these CRT/XAPI callers | dc3-decomp `scripts/build/build_xex.py` (import completeness check: every PE IAT ordinal must become an XEX import record) + CRT init order | V (comments) / I |
| Debug stub table (2709-2790) | 49 | `XGetLocale`, `XTLGetLanguage`, `DataNode::Print`; **40 Holmes client functions**; `FileCache::GetFileAll`, `CharClip::Init`, `SetFileChecksumData` | Decomp-build (corruption; the original runs Holmes unpatched and even uses `HolmesClientPollKeyboard` as the DTA-channel hook) | "corrupt list nodes after 339-unit promotion", "corrupt vector buffer" | dc3-decomp | V |
|  | 1 | `DebugBreak` → 0 | Harness config | `--break_on_debugbreak=false` already exists | Delete | V |
|  | 1 | `ReadError` → 0 | Harness config / VFS | "cache paths return ERROR_INVALID_NAME". The headless app leaves `--mount_cache=false` (`xenia_headless_main.cc:65`) | Pass `--mount_cache=true`; consider defaulting it on in headless | I |
|  | 1 | `CDReadDone` → 1 | Async I/O (refuted) | See O35 | Delete | V |
| ApplyDebugStubs bespoke (2951-3807) | 19 | host `Debug::Fail` (log+return); `DoCrucible` guard; host `RegisterFactory`/`NewObject` map; `__RTDynamicCast` pass-through; cycle-safe `SetDirty_Force`; `LoadMetaMaterials`; `Object::SetName` null-dir guard; `CreateAndSetMetaMat`; `AllocType`; `NgPostProc::RebuildTex`, `NgDOFProc::Init`, `DxRnd::Suspend`; `TextStream<<` null; `XMPOverride/RestoreBackgroundMusic`; `String::operator+=` blr | Decomp-build | Every comment cites `/FORCE` duplicate type_info, a corrupt `sFactories` tree, a corrupt switch table or "unbounded PE image corruption"; the GPU ones block only on the decomp image (the original reaches gameplay on `--gpu=null`) | dc3-decomp | V |
| CRT bridges (1494-1787) | 6 | `_write_nolock`, `_write`, `_output_l`, `_woutput_l` host bridges; `GetSystemLanguage`/`Locale` identity | Decomp-build (CRT `_ioinit`/locale not initialised) | – | dc3-decomp | I |
| DataArray safety (2391-2516) | 2 | merged `DataArray::Node` bounds override; `SystemConfig` SetupFont literal repair | Decomp-build (ICF-merged Node; corrupted string literal relocation) | "investigate decomp object/relink freshness and PPC hi/ha relocation" (in-code) | dc3-decomp | V |
| MemAlloc bootstrap (2007-2262) | 8 | host `operator new`/`delete`, `MemAlloc`, `PoolAlloc`, `MemOrPoolAlloc`, `MemFree`, `PoolFree`, `MemOrPoolFree` + bump pool + `TheArchive` sync to the original address | Decomp-build | "`/FORCE:MULTIPLE` merged link_glue stubs over MemMgr.obj; `operator new` is the epilogue of `MemPrintOverview`" | dc3-decomp | V |
| XDK overrides (3808-3889) | 1 group | every manifest symbol matching CX2/XAUDIO2/Nui/CSp/NUISPEECH → return 0, plus prologue scan over XDK ranges | Decomp-build (XAudio2 deadlocks only in the decomp; the original runs XAudio2 with the dummy driver) | – | dc3-decomp | I |
| Runtime stopgaps: image/memory (3890-3960, 4424-4444, 4544-4578) | 7 | module stack ≥4 MB; whole PE RW; +16 MB heap at 0x83F60000; Linux `mmap` RW at 0x7F000000 and 0xFFFFF000; zero page RW + host guard below membase | Decomp-build (overflows, writes to .text/.rdata, larger image, null derefs from corruption) | – | dc3-decomp; a faithful emulator should keep page 0 unmapped | V |
| Runtime: CS overrides (3961-4120) | 3 | `CriticalSection::ctor/Enter/Exit` host versions | Decomp-build ("IAT entries remain null", so `bctr` to 0) | – | `build_xex.py` | V |
| Runtime: STL/BSS (4122-4185) | 2 | `gConditional`, `gDataArrayConditional` sentinels | Decomp-build (`__xc` never ran) | – | dc3-decomp link | V |
| Runtime: CreateDefaults NOPs (4187-4205) | 2 | 2nd/3rd `CreateDefaults` | Decomp-build ("RndCam dtor, corrupted ref list from /FORCE") | – | dc3-decomp | V |
| Runtime: GPU/Bink blr (4207-4250) | 8 | `NgPostProc::RebuildTex`, `NgDOFProc::Init`, `RndShadowMap::Init`, `DxRndOcclusionQueryMgr` ctor, `DxRnd::InitBuffers`, `DxRnd::CreatePostTextures`; `BinkStartAsyncThread`, `BinkMovieSys::PlatformInit` | 6 decomp-build (original OK on null GPU); 2 **threading** (as O19) | – | dc3-decomp / covered by `c12de876f` | I |
| Runtime "ALL layouts" block (4256-4334); unreachable on the original, addresses are decomp | 11 | `LiveCameraInput::PreInit/Init`; `GestureMgr::Poll/GetSkeleton/UpdateTrackedSkeletons`; `SkeletonIdentifier::Init/Poll`; `OSCMessenger::Poll`; `HasFileChecksumData`; `App::DrawRegular`; `SaveLoadManager::Activate` | 7 NUI; `OSCMessenger` (UDP Holmes) / `HasFileChecksumData` (decomp DTBs) / `DrawRegular` (decomp VdSwap) / SaveLoad (decomp source bug) → 4 decomp-build | The comment calls them "required regardless of layout", but the original-layout path never runs this function, and on the original these addresses are other functions (e.g. 0x827D0EC0 = `LoadMgr::Init`+0x38) | dc3-decomp / NUI HLE | V |
| Runtime "decomp-only" stubs (4336-4420) | 17 | Splash ×4; `VoiceInputPanel::LoadVoiceContexts`; `Fader::UpdateValue`; `Synth::InitSecurity`; `ShellInput::Init`; `MoveMgr::Init`; `HamSongMgr::Init`; `Locale::Init`; `UIManager::GotoFirstScreen`; `list<RndTransformable*>::remove`; `FlowManager::Poll`; `SkeletonUpdate::InstanceHandle/PostUpdate`; `SkeletonHistoryArchive::AddToHistory` | Splash ×4 + GotoFirstScreen = threading (5); VoiceInput/ShellInput/SkeletonUpdate ×2/History = NUI (5); `Locale::Init` = harness (`--devkit_root`) (1); the rest decomp-build (6). `HamSongMgr::Init` also notes "C++ EH doesn't work in JIT", a real gap (§3 G13), but the throw comes from corrupt config | Pack comment: "/FORCE:MULTIPLE BSS shifts, corrupt vtables" | mixed | I |
| Runtime: CRT traps etc. (4446-4542) | 7 | `FileIsLocal(game:)` assert bypass; `_errno` override; `_invalid_parameter_noinfo`, `_call_reportfault`, `_amsg_exit`, `__report_gsfailure` → 0; `Hx_snprintf` callsite | Decomp-build | – | dc3-decomp | V |
| CRT patches (4582-5550) | 27 | host `StringTable::Add`; host `Symbol::PreInit`; `ReadSystemConfig` and `DataReadStream` code caves (in ProtocolDebugString); `CheckForArchive` → `gUsingCD=1`; host `ArkFile::Read` (BlockMgr ALTERNATENAME no-op'd); CRT `__xc` sanitizer + **NUI skip of 191 slots** (`75,98-101,142-328`); `.CRT$XCU` injection; `_ioinit` into `__xi_a`; host `StringTable` + `gHashTable`; 12 STL sentinel/mPlatform writes; `PreInitSystem` store offset ×2; early `gUsingCD` | Decomp-build (the NUI-skip slot is NUI ×1) | "link_glue __xc initializers ICF'd to blr" (in-code) | dc3-decomp link/CRT | V |
| IK instrumentation (5551-6150) | 1 group | code caves on `HamIKEffector` | Instrument | – | `titles/dc3/dc3_ik_telemetry.cc` (plan) | V |
| Decomp NUI table (`emulator.cc` ~6054-6166) | 85 | NUI at decomp addresses | NUI (dies with the layout) | – | delete with the layout | V |

**The decomp-side work the pack implies**, if anyone wants a decomp-boot bar again. Everything here is
in dc3-decomp:
- **Imports.** `build_xex.py` must turn every IAT ordinal into an XEX import record. Gate it:
  `count(PE imports) == count(XEX import records)`.
- **Link.** Stop using `/FORCE:MULTIPLE` with link_glue ALTERNATENAME no-ops for undecompiled
  bodies. Link real objects, or keep undecompiled functions out of the boot path.
- **CRT.** Merge `.CRT$XCU` into the `__xc` table at link time, and make sure `_ioinit`/`_mtinit`
  run in CRT order.
- **Data.** Rebuild DTB checksums, or ship the decomp DTBs with the original's checksum table.

The acceptance test is: "the rebuilt xex boots with `ApplyDc3HackPack` disabled". S3 (exactly 627
traps) is a fingerprint of the defect list above. It is a core-change tripwire, not a quality bar,
and a legitimate Xenia fix may move it.

---

## 3. Core "forced gates" DC3 relies on (all titles, defaults ON)

These are not in the hack packs, but they are where the Xenia deficiencies show up. FORK_CLEANUP_PLAN
§2.5 / §3.4 already wants each one flipped to the upstream default with a per-title profile.

| # | Gate (file) | What it masks | Real gap? | Proper fix | Conf |
|---|---|---|---|---|---|
| G1 | `io_force_synchronous_completion` (`xboxkrnl_io.cc:30`) | Upstream returns STATUS_PENDING but writes the IOSB | **No current mechanism** (`ac0052e5b`): upstream semantics are NT-legal for DC3's XAPILIB | Flip to `false`; A/B S1/S2/S4. A real async worker for timing fidelity is optional (L) | V/I |
| G2 | xam_enum overlapped NO_MORE_FILES → SUCCESS/0 (`xam_enum.cc:83`) | "DC3 only handles 0 and 0x65B" | Doubtful: `ContentMgr::PollRefresh` handles `0x12` explicitly (`ContentMgr_Xbox.cpp:566-570`) | Needs hardware evidence; test with upstream behaviour once O36 is fixed | I |
| G3 | `soft_fault_unmapped_reads` (`mmio_handler.cc`) | Null/unmapped reads return 0 | No; it masks guest-side divergence (on hardware these crash) | Flip off; any DC3 read that needs it names a self-inflicted state (forced screens) | I |
| G4 | `tolerate_null_guest_calls` (`x64_emitter.cc`) | Calls through null become no-ops | No (same) | Flip off; A/B | I |
| G5 | CS auto-init + force-release (`xboxkrnl_rtl.cc`) | Uninitialised/unowned critical sections | No; the comment cites "DC3 decomp's /FORCE-linked unresolved externs" | Flip off; decomp-only profile, then delete | V |
| G6 | DC3 4 MB min worker stack (`xboxkrnl_threading.cc:384-396`) | 256 KB worker overflow | No: **self-inflicted** (UI chains Executed on the SkeletonUpdate worker; `0xBCBCBCBC` = uninitialised objects the forced flow raced) | Delete after L5 | I |
| G7 | `XamNuiGetDeviceStatus` → connected for all titles (`xam_nui.cc:41-48`) | Kinect present | NUI | Gate on a title-agnostic `kinect_device_present` cvar; part of the NUI HLE | V |
| G8 | Dummy audio driver handles (`xboxkrnl_audio.cc`) and the nop APU | Audio callbacks never run | XMA/audio | Paced nop driver (`77d85acaa`) is the fix; upstream it | V |
| G9 | `fault_spin_limit=4096` (`exception_handler_posix.cc`) | Livelocked fault loops | No | Default 0; RB3DX profile only | V (plan) |
| G10 | `headless_skip_submission_wait`, `headless_capture_only_draws`, async-pipeline draw drop (`vulkan_command_processor.cc`) | Headless Vulkan speed | Harness | Default off; scripts opt in (plan) | V |
| G11 | `dc3_persist_render_state` (Vulkan deferred capture) | Fork capture path state loss | Fork-infra bug in deferred replay, not upstream | Rename `headless_*`; fix replay | I |
| G12 | Override API (`processor.cc:224-271`) | – | **Fork-infra defect**: overrides are not guaranteed on indirect calls (BASELINE: the `BinkMovieImpl::Ready` body ran). `ArkFile::Read`'s comment: "SetupExtern on an already-resolved function corrupts state" | Make overrides patch the indirection-table entry and invalidate compiled callers; add a unit test | I |
| G13 | Guest C++ throw → `Break()` (`xboxkrnl_debug.cc:120-151`) | No SEH/EH dispatch | **Yes, real upstream gap.** MILO_TRY/CATCH cannot work; the DTA channel `longjmp`s around it | Implement `RtlRaiseException` dispatch: walk `.pdata`, call the guest `__C_specific_handler`/`__CxxFrameHandler`, implement `RtlUnwind`. L-XL · upstreamable | V |

---

## 4. Root-cause clusters, ranked by hacks retired ÷ effort

Effort: XS < ½ day, S ≈ 1-2 days, M ≈ 1 week, L ≈ 2-4 weeks, XL > 1 month.
"Oracle" means the hack perturbs a subsystem the native port is compared against
(XENIA_ORACLE.md: the DTA interpreter, ObjectDir/merge, Char/CharClip/RndPropAnim, skinning,
HamIKEffector, math, Rnd draw submission, the allocator).

| Rank | Cluster | Retires (O = original, C = core, D = decomp) | Effort | Upstream? | Oracle risk today | Conf |
|---:|---|---|---|---|---|---|
| 1 | **K1 POSIX thread lifecycle** (lost-resume, already fixed in `c12de876f`) | O19-O27 (9) + D Splash/Bink/GotoFirstScreen (7) + boot flakiness (BASELINE x1 "thread 6 never ran") | **S** (A/B only) | **Yes**: send `c12de876f` upstream | Low (movies/splash), but O27 Executes `FindObject` | I (strong) |
| 2 | **K2 Async-I/O mechanism refuted** | O35 + G1 (+ D `CDReadDone`) | **S** | Returns to upstream | Low-med (load ordering) | V/I |
| 3 | **K3 Harness config already in Xenia** | O47 content wipe, D `DebugBreak`/`ReadError`/`Locale::Init` | **XS** | n/a (`--mount_cache` default could go upstream for headless) | None | V |
| 4 | **K4 Main-thread automation** (stop host→guest Execute on the worker) | O18 (restore spin + tripwire), O28, O29, O32, G6; replaces O30, O31, O33, O34, O46 with DTA-channel calls (≈10 total) | **M** | No (title harness) | **HIGH**: O18 silences every MILO_FAIL (DTA interpreter); worker Executes race ObjectDir (BASELINE k1); O34 changes the evaluated anim | V (trigger measured) |
| 5 | **K5 Headless audio pipeline** (paced driver + real XMA HAL) | O38-O44 (7) + G8 | **M** | Paced nop driver: yes | **HIGH**: two 120 BPM drives write the TaskMgr timelines that drive RndPropAnim/CharClip; song time is non-monotonic | V (c2c8c87d4) / I |
| 6 | **K6 Content enumeration / XAM async** | O36, O37 (+ maybe O15-O17), G2 | **M** | **Yes** (XAM HLE) | Low-med | I (path V) |
| 7 | **K7 Override-API correctness** | Makes every later A/B trustworthy; may reveal inert hacks (O20) | **S-M** | Fork infra | Indirect | I |
| 8 | **K8 Core masking flips** | G3, G4, G5, G9 (+ G10/G11 harness) | **S each** (A/B) | Returns to upstream | **MED-HIGH**: silent all-subsystem divergence (allocator, ObjectDir) | I |
| 9 | **K9 NUI HLE + pose source** | O1-O14 (69) + G7, D NUI (13 + 85), then lets K4's automation become gesture-driven | **L** (API HLE); XL for depth-frame device emulation | Partly (signature tables per SDK) | Low on oracle-grade subsystems (input only), but O8-O14 gate gameplay | V (need) / I (design) |
| 10 | **K10 Guest C++ SEH/EH** | G13; unblocks faithful MILO_TRY/CATCH and removes the DTA throw hook | **L-XL** | **Yes**, high value | **HIGH** for DTA semantics (MILO_TRY in data loading) | V (gap) |
| — | **K11 Decomp-build defects** | 149 of 173 decomp stopgaps + 85 decomp NUI + O48 | XS to delete; XL to fix in dc3-decomp | n/a, belongs to dc3-decomp | None on the original | V |

**Per-hack ratio:** K1 (≈16 retired / S), K3 (4 / XS), K2 (2-3 / S), K4 (≈10 / M), K5 (8 / M),
K9 (≈70 / L), K6 (2-5 / M), K10 (1 gate, but correctness / L-XL).

---

## 5. Implementation lanes, in order

Each lane runs in its own xenia worktree off `main`, follows the FORK_CLEANUP_PLAN rules (disjoint
ownership, `--no-ff` landing), and uses `tools/fork-regress/`.

The shared verification on every lane:
- `run.sh <bin> <out> --scenarios S0,S1,S2,S3` before and after the change, then
  `compare.py baselines/integrate-6bf623353-*.json <out>` exits 0 (S1 needs ≥2 PASS of 5;
  S3 is recorded).
- Any S3 change must be named in the commit message, because S3 fingerprints the decomp image.
- S6 inertness may not gain a leak.

**L0: make each hack individually removable, and make silenced asserts visible (prerequisite).**
- Every original-layout hack gets a name, and a `--dc3_disable_patch=<name,...>` list switch, in the
  `titles/dc3/` module that plan Lane A creates.
- Add the `TheDebug+0x104` (`mFailThreadMsg`) tripwire. S1 already counts `tainted_lines`.
  Without the tripwire, any removal A/B is measured with asserts silenced and can "pass" while
  broken.
- *DoD:* with all hacks on, S1 shows no regression; each hack logs one applied line; the tripwire
  fires on today's binary (that is the expected baseline: the Bink wrong-thread FAIL).

**L1 (K7): override API.**
- `Processor::RegisterGuestFunctionOverride` must also route indirect calls: rewrite the
  indirection-table slot and drop already-compiled code for that address.
- *DoD:* a `xenia-cpu-ppc-tests` case calls an overridden function via `bctrl` after it was already
  JIT-resolved, and the handler fires; S1 shows no regression.

**L2 (K1): thread-lifecycle retirement.**
- On a binary containing `c12de876f`, remove in this order, with one A/B each:
  1. O19, restoring the real `BinkMovieSys::Init` (log both Bink async threads reaching
     `XThread::Execute`);
  2. O22, O20, O21;
  3. O23-O26;
  4. O27 last.
- *DoD:* S1 meets its criteria with all nine gone; 16/16 early-boot health (the `c12de876f`
  method); S1V frames show the attract movie; no new tripwire FAIL. Open the upstream PR for
  `c12de876f`.

**L3 (K2 + K3): refuted async gate and harness-config cleanups.**
- Set `io_force_synchronous_completion=false` (upstream), remove O35, delete O47, remove the decomp
  `DebugBreak`/`ReadError`/`Locale::Init` stubs in favour of cvars.
- *DoD:* S1/S2 meet their criteria; S4/S5 (RB3) meet theirs with the RB3 profile; NtReadFile
  returns STATUS_PENDING for async files in the log; the BlockMgr reads complete without O35.

**L4 (K4): automation onto the guest main thread.** This is plan §2.4 decisions 1 and 3.
- Run the nav bridge, transition force and LoadSong repair through the `HolmesClientPollKeyboard`
  hook as DTA calls (`{goto ...}`, `{set_song ...}`), behind `--dc3_headless_autonav` (default off).
- Restore the faithful `Debug::Fail` spin (delete O18). Delete O32, O34 and G6. A/B O28/O29.
- *DoD:* no `processor->Execute` from any guest thread other than the main thread (assert with a
  log of the thread id); tripwire 0 in S1 (5/5 runs); S1 meets its criteria with the 4 MB clamp
  removed; `analyze_run` shows game-driven transitions.

**L5 (K5): headless audio.**
- Remove O38 and O39. Run `--nop_audio_driver=paced`. If `XMACreateContext` or the HAL fails,
  debug it in `apu/xma_*` (it is now a real XMA bug, not a stub).
- Then remove O40, O41, O44, and both beat drives (O42, O43).
- *DoD:* the game reaches `gpState=2 paused=0` by itself (`Game::PostWaitStart`); TaskMgr seconds
  are monotonic and track audio; ≥60 gpState=2 samples in S1. Upstream the paced nop driver.

**L6 (K6): content enumeration.**
- Instrument `ContentMgr::mState` and the cross-title chain (`0x279` aggregate enumerator,
  `XamGetPrivateEnumStructureFromHandle`, `XamTaskSchedule`, the `XMsgInProcessCall` message id,
  `XMsgCompleteIORequest`).
- Fix the XAM piece that leaves the overlapped incomplete. Remove O36, then A/B O37 and O15-O17.
- *DoD:* `RefreshDone` turns true naturally in S1; `MainMenuPanel`/`SongSelectPanel` content paths
  work; a xam unit test for overlapped completion through `XamTaskSchedule`.

**L7 (K8): flip the core masking gates to upstream defaults.**
- For G3, G4, G5, G9 and G10, run S1 per gate. If DC3 still needs one, the DC3 profile opts in
  with the named guest site that requires it, which is a bug to file.
- *DoD:* defaults equal upstream; S0's cvar-diff lists each change; S1/S4/S5 meet their criteria
  with the profiles.

**L8 (K9): NUI HLE.**
- Write the design doc first: the API surface, events, a pose source interface
  (constant / recorded / socket), and a speech state machine.
- Implement it in `src/xenia/kernel/nui/` (or `hid/nui`), signature-resolved and title-agnostic,
  gated on `kinect_device_present`.
- Retire, in this order: O1+O2+O4 (events), O7, O8-O14, G7, O6. Then evaluate gesture-scripted
  navigation as the faithful replacement for L4's DTA-driven automation.
- *DoD:* S1 meets its criteria with zero DC3 NUI address patches; SkeletonChooser calibrates on
  emulated players; no `PauseForSkeletonLoss` false positive; S6 shows no leak into DC1/RB3.

**L9 (K10): guest SEH/C++ exceptions.**
- Implement `RtlRaiseException` dispatch via `.pdata`, the guest frame handlers, `RtlUnwind`
  (currently a no-op in the fork) and the `__C_specific_handler` path.
- *DoD:* a PPC test with try/catch passes; in S2, `{no_such_func 1}` is refused through the real
  MILO_CATCH with the DTA `g_cpp_throw_hook` removed. Open the upstream PR.

**L10 (K11, dc3-decomp side, independent).**
- Archive-tag and delete `ApplyDc3HackPack`, the decomp NUI table, O48 and G5's decomp rationale.
  Retire S3 in the same commit, with the decision recorded (plan §6 risk row).
- File the decomp worklist from §2b (imports, link, CRT, DTB).
- *DoD:* xenia `main` has no decomp-layout code; dc3-decomp owns a "rebuilt xex boots unpatched"
  gate if it wants one.

**Ordering:**
1. L0, then L1.
2. L2 and L3 in parallel (both S).
3. L4 (it makes every later A/B honest about asserts and threads).
4. L5 and L6 in parallel.
5. L7.
6. L8.
7. L9.

L10 can run any time; it touches no core.

---

## 6. Caveats and open measurements

- **Rationales that are lost.** Twelve original-layout patches arrived in `69f8fdae2` "Restore DC3
  boot instrumentation" (+2650 lines, a one-line message) and `8cc604e0e`. Their original reasons
  are not in history:
  - `SaveLoadManager::Activate`, `ContentMgr::RefreshDone`;
  - the calibration set;
  - `Movie::Poll`, `XMAHALAllocateContexts`, `HandleWait`, `IsReady`, `SongAnim`;
  - Splash, `BinkMovieSys::Init`.

  The I-rated classifications above are hypotheses to be confirmed by L0's per-hack A/B, not
  facts.
- **Host load.** S1 itself is load-sensitive (BASELINE: 0/6 at load 100-220; the harness gates on
  mean load 80). A single A/B under load is INCONCLUSIVE, not a pass.
- **O20 may already be inert** (BASELINE: guest body ran on the vtable path). A removal that "changes
  nothing" may just mean the override never fired. That is why L1 comes before L2.
- **Holmes on the original is not stubbed.** The DTA channel and the IK reader hook Holmes
  functions as main-thread entry points. Removing hacks must not remove those instruments
  (plan bucket 3).
- **Counting conventions.** The 56 NUI rows count functions, not call sites. Plan §2's 258 "rows"
  are decisions; the counts here are guest-visible mutations.
