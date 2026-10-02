# DC3 on Xenia: baseline (step 1 of XENIA_ORACLE.md)

Branch `dc3-oracle`, based on xenia `main` `90eb07f81`. The baseline binary is
built from clean `main` sources: xxh3 `45bf3c063e801035`, Checked,
`make -C build xenia-headless config=checked_linux`. Target `debug.xex` sha256
`2d5e4a320aabf272ef21f1ac6ae6518460fdf4892ec88fd8920f770bf29c4728`
(`dc3-decomp/orig/373307D9/debug.xex`, the same bytes as
`orig-assets/debug.xex`). Measured 2026-09-30 / 10-01.

## The exact command

`docs/dc3-oracle/run_dc3_oracle.sh <run-dir> null 240` writes the full argv
into `<run-dir>/cmd.txt`. For run b1 it was:

```
xenia-headless \
  --storage_root=<run>/storage \
  --config=docs/dc3-oracle/xenia.dc3-oracle.defaults.toml \
  --target=/home/free/code/milohax/dc3-decomp/orig/373307D9/debug.xex \
  --gpu=null --vulkan_device=1 \
  --dc3_nui_patch_layout=original --dc3_crt_skip_nui=true \
  --stub_nui_functions=true --fake_kinect_data=true \
  --scripted_input_file=/home/free/code/milohax/dc3-decomp/scripts/dc3-input-flows/xenia-ymca.txt \
  --headless_timeout_ms=230000
```

Notes on the command:

- `--config` points at a pinned, all-defaults toml (this binary's own output
  into an empty storage root), and `--storage_root` is private. The shared
  `~/.local/share/Xenia/xenia.config.toml` is never read or rewritten; without
  `--storage_root`, `SetupConfig` re-saves it on every run.
- `--stub_nui_functions=true` is now explicit. It used to come only from the
  shared toml.
- `--dc3_crt_skip_nui=true` is inert on this layout: it only acts inside the
  decomp-layout hack pack. It is kept for parity with the documented command.
- `--devkit_root` is not passed, so no `devkit:` device is mounted. This
  matches every documented gameplay run. Mounting
  `orig-assets/extracted` would expose loose files, writable
  (`HostPathDevice(..., read_only=false)`), and could change which files the
  title loads.
- Vulkan adds `--gpu=vulkan --dump_frames_path=<run>/frames
  --headless_capture_interval=300` and runs on GPU 1.

## Result

| run | GPU | channel | load avg | title | game_screen | playing (gpState=2) | outcome |
|---|---|---|---|---|---|---|---|
| b1 | null | — | ~12-20 | 18 s | 45 s | 45 s → 190 s (74 samples) | song ran, then gpState=3 at 190 s; rc 0 at the 230 s timeout |
| b2 | null | — | ~12-20 | 9 s | 33 s | 39 s → 186 s (75 samples) | same |
| c1, c2 | null | on, successful evals only | ~20 | 9 s | 33 s | 33-36 s → 184 s | same, 0 SIGSEGV |
| b3 | vulkan | — | ~20 | 9 s | **never** | — | stuck in loading→game_screen; skeleton worker silent after `Nav goto (real, game_screen)`; main thread still swapping (13,282 VdSwaps) |
| b4 | vulkan | — | ~20 | 18 s | **never** | — | flow went backwards: `song_select_screen` transitioning to `choose_mode_screen`, never left |
| b5 | null | — | ~100-127 | — | **never** | — | guest fault in `FreestyleMotionFilter::IsActive` (0x82DECDB0); main thread parked in `D3D::CBlocker::Check` |
| k1 | null | on, **no evals** | ~100-127 | 18 s | **never** | — | main thread faulted in `ObjectDir::FindObject` (0x82595A5C) during `real_loading_screen` |
| k2 | null | on, no eval reached | ~100-127 | 18 s | **never** | — | main thread blocked in `WaitForSingleObjectEx`; thread 9 kept presenting; the hook never fired |
| k3, c4 | null | on | ~100+ | — | — | — | **boot hang**: host launch thread in a futex wait right after `END OF ACHIEVEMENTS`, before the NUI stub block; 4 host threads; ptrace is blocked (`ptrace_scope=1`), so there is no backtrace |
| x1 | null | on | ~80-100 | — | — | — | **boot hang**: guest thread 6 created (`XThreadF8000028 (6)`) and never ran |
| x2 | null | on | ~80-100 | yes | yes | — | used for the channel round trip (see SPIKE_LOG) |
| b6 | null | — | 157→219 | — | — | — | **boot hang** at the same point as c4/k3 (after `END OF ACHIEVEMENTS`), with no channel |
| b7 | null | — | 219 | 18 s | **never** | — | rc 133 at 75 s: guest fault in `SongSort::BuildItemList` (0x829894B0), the same site as channel run c3 |

**Reading:**

- **Gameplay still works on null GPU at low host load.** 2/2 baseline runs and
  2/2 channel runs reached `game_screen`, unpaused, and ran the host beat
  drive for about 150 s. The repo's two Xenia docs claim the same, so this is
  a re-verification, not a new result.
- **The Vulkan gameplay path failed 2/2, at two different points.** This is a
  finding. The menus render, but the flow is wall-clock driven (script `+N` is
  N×50 ms; nav thresholds count NUI calls at 30 Hz), and the slower Vulkan
  frame rate makes the inputs and the nav bridge land differently.
- **Under heavy host load (load avg ~100-220, from other sessions), 0 of 6
  runs reached `game_screen`, three of them channel-less baselines (b5, b6,
  b7).** 4 of 9 boots hung (c4, k3, x1, b6). The boot-hang and fault classes
  belong to the baseline itself. The failures are different every time, and
  they are the races the plan predicts. k1 faulted in `ObjectDir::FindObject`
  on the MAIN thread while the skeleton-worker nav bridge was calling engine
  code (`GotoScreen`/`FindObject` via `processor->Execute`) at the same time.
  That is the June 2026-06-02 race class.
- **Repeatability is a property of host load, not of the command.** Run time
  to `game_screen` is 33-45 s when it works. Nothing makes the flow
  deterministic.

## Patch manifest (original layout, the command above)

This was verified against the source at `90eb07f81` (read-only survey plus my
own spot checks). Everything in it is active unless marked otherwise.
"Override" means `RegisterGuestFunctionOverride` (a host function replaces the
guest one). "Byte patch" means guest `.text` is rewritten. Line numbers are in
`src/xenia/emulator.cc` unless a file is named.

| Subsystem perturbed | Where | What |
|---|---|---|
| Kinect / NUI input | 3994-4139, 4583-4686 | 59 overrides: 56 NUI + 3 `CXbcImpl` (Initialize 0x82606078, DoWork 0x82605960, SendJSON 0x82605DF8). 46 return 0 and 12 return -1. Resolved by the hybrid resolver (symbols.txt 56, signature 3) |
| Kinect / NUI input | 1447-1494 | `NuiSkeletonGetNextFrame` 0x829C2790 → `Dc3NuiSequencerExtern`: one tracked skeleton, a constant 20-joint pose (1475-1491) |
| Kinect / NUI input | 1527-1540 | GestureMgr `mInControllerMode` (*0x82F5F7B4 + 0x426D) := 1 on every NUI call |
| Kinect / NUI input | dc3_hack_pack_skeleton.cc:212-214 | 0x8242E74C `li r28,0x21`: SkeletonUpdateThread's INFINITE wait becomes 33 ms. 0x8242E1B0 nop |
| Kinect / NUI input | 4963-5054 | Calibration bypass: SetPlayerPresent guard nop; ChoosePlayerSides, SetPlayerSkeletonWarningData and ExitControllerMode → blr; SetPlayerSkeletonNavData rewritten; ShouldWaitForRecovery → 0 |
| Kinect / NUI input | kernel/xam/xam_nui.cc:39-48 (+~25 XamNui stubs) | Device reported as connected |
| UI flow / transitions | 1724-1863 | After 120/180/240 stuck NUI calls: force-enter, force-complete or clear the transition via UIManager +0x2C/+0x48/+0x4C. Calls `UIScreen::Enter` |
| UI flow / transitions | 2215-2408 | Nav bridge: on stable-screen thresholds, calls `GotoScreen` (0x8277B378) **from the skeleton worker thread** along attract→…→game_screen. game_screen is held on `merge_busy` |
| UI flow / transitions | skeleton.cc:348-351, 305-312 | `UIManager::GotoFirstScreen` override; `MoviePanel::IsLoaded` → 1 |
| UI flow / transitions | 4840-4863 | `HamPanel::FocusComponent` → UIPanel's; `HamScreen::IsEventDialogOnTop` → 0 |
| UI flow / transitions | hid/nop/nop_input_driver.cc:767-831 | On attract: A after 1.5 s; after 5 s, scans the heap and force-sets the title screen. Not title-gated |
| Song selection | 2410-2654 | "LoadSong repair": if the song is empty on loading_screen, inject `ymca` (id 7011) and set pads. Probe calls run even when the song is set |
| Pause / wait | nop_input_driver.cc:552-574 | Unpause nudge: game+0xA4=0, gp+0xF8=0, game+0x60=1, game+0x5E=0. **Not title-gated** |
| Pause / wait | 5070-5079, 5115 | `Game::PauseForSkeletonLoss` 0x82866D50 → blr; HandleWait 0x82867318 → `b +0x24` |
| Song clock / beat | 2689-2776 | Beat drive A: 1/30 s per NUI call, 120 BPM, writes TheTaskMgr 0x82F64A58 timelines |
| Song clock / beat | nop_input_driver.cc:336-493 | Beat drive B: wall clock, 120 BPM, same fields |
| Audio | 5105-5118 | `XMAHALAllocateContexts` 0x82E77250 → 0; `HamAudio::IsReady` 0x8252BA50 → 1 |
| Audio | kernel/xboxkrnl/xboxkrnl_audio.cc:66-110 | Nop APU dummy driver: render callbacks never fire |
| Anim selection | 5120-5144 | `HamDirector::SongAnim` 0x82475578 → `li r4,2; b SongAnimByDifficulty` (EXPERT) |
| Movies / splash / save / speech | 4831, 4883-4927, 4950, 5081; skeleton.cc:281-288 | SaveLoadManager::Activate blr; Splash PrepareNext→0 and Begin/Suspend/Resume blr; SpeechMgr::Grammar::Unload blr; BinkMovieSys::Init; Movie::Poll → 0; `BinkMovieImpl::Ready` → 1 |
| File-I/O completion | 4865-4881 | `CDReadDone` 0x826026E0 → 1; `ContentMgr::RefreshDone` 0x825FEB48 → 1 |
| File-I/O completion | kernel/xboxkrnl/xboxkrnl_io.cc:221-365 | Nt* I/O always completes synchronously. **Hardcoded, all titles** (no cvar on `main`) |
| File-I/O completion | kernel/xam/xam_enum.cc:83 | Overlapped enumerate NO_MORE_FILES → SUCCESS. Hardcoded |
| **Error semantics** | skeleton.cc:227 | `Debug::Fail` 0x825CE2DC: the worker-thread spin becomes `b` to the epilogue. **Measured consequence:** by the first main-loop frame, `TheDebug.mFailing` is stuck at 1, with `mFailThreadMsg = 'BinkMovieImpl::Ready called in the wrong thread (expected 6, cur thread is 15)'`. Debug::Fail starts with `if (!mNoDebug && !mFailing)`, so **every later MILO_FAIL / MILO_ASSERT on every thread is a silent no-op for the rest of the run**, and the code falls through |
| Error semantics | kernel/xboxkrnl/xboxkrnl_debug.cc:103-139 | A guest C++ `throw` cannot be caught: `RtlRaiseException` → `HandleCppException` → `xe::debugging::Break()` (SIGTRAP; the second one kills the process). There is no unwinder, so **MILO_TRY/MILO_CATCH cannot work under Xenia** |
| Error semantics | cpu/mmio_handler.cc:542-561 | Unmapped guest READ: the destination is zeroed and the instruction skipped. Hardcoded, all titles |
| Error semantics | cpu/backend/x64/x64_emitter.cc:589-647 | Indirect call to null/unresolved → no-op. Hardcoded |
| Memory | cpu/mmio_handler.cc:481-502 | A write fault in guest 0x83320000-0x836C0000 is made RW and continues. Hardcoded, no title gate |
| Threading / CS | kernel/xboxkrnl/xboxkrnl_rtl.cc:537-624 | RtlEnter/LeaveCriticalSection auto-init; Leave forces ownership and clamps recursion. Hardcoded |
| Threading | kernel/xboxkrnl/xboxkrnl_threading.cc:151-154 | DC3 threads get a minimum 4 MB stack |
| GPU / render | gpu/vulkan/vulkan_command_processor.cc | `dc3_persist_render_state=true` (Vulkan only) |
| *inactive* | 3888-3915 | Content wipe + manifest load: dead code (gated on `dc3_is_decomp_layout`, which is still false at that point) |
| *inactive* | 4748-4803 | The whole decomp-layout hack pack, the CRT skip included |
| *inactive* | 1865-1941, 1955-2202 | Gameplay bootstrap, gameplay probe, IK telemetry (cvars off) |

### Disagreements with the plan's §1 table

1. **Error semantics are wider than "the Debug::Fail thread spin returns".**
   The patch leaves `mFailing` stuck at 1, which silences every later
   MILO_FAIL on every thread, the main thread included (measured, quoted
   above). On top of that, Xenia cannot catch any guest C++ throw. Any golden
   recorded under this layout reflects "run past every assert" behaviour.
2. **The plan's "no patch touches the DTA interpreter / ObjectDir" holds for
   code bytes, but not for their failure behaviour.** A DTA script error in
   any game code is a fall-through, not a MILO_FAIL.
3. **The skeleton constant-pose lines are 1475-1491**, not 3217-3238. The
   beat drives are at 2689-2776 and nop_input_driver.cc:420-481.
4. **The plan's table omits:** the per-call `mInControllerMode` write, the
   calibration byte patches, `Movie::Poll`, `BinkMovieSys::Init`, the
   `HamPanel`/`HamScreen` patches, the `GotoFirstScreen` override, and the
   dummy XAudio driver.
5. **Cvars named in the plan do not exist on `main`**
   (`io_force_synchronous_completion`, `soft_fault_unmapped_reads`,
   `autoinit_critical_sections`, `rtl_leave_critical_section_force_release`,
   `tolerate_null_guest_calls`, `dc3_clean_content_cache`). Their behaviour is
   hardcoded ON here, for every title. The exception is the content wipe,
   which is dead code here.
6. **The unpause nudge and the attract→title force are not title-gated.**
   They fire for any title when a script file is set.
