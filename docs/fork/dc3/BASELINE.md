# DC3 on Xenia: baseline (re-measured by fork-cleanup Lane B, 2026-10-02)

This file supersedes the dc3-oracle baseline of 2026-09-30/10-01 (branch
`dc3-oracle`, `main` `90eb07f81`), which used to live at
`docs/dc3-oracle/BASELINE.md`. That text is in git history. Its two
conclusions that still matter are restated under "History" below.

The patch manifest for this baseline is `PATCH_MANIFEST.md` (same directory).

## What is measured, and how

- **Binary.** `lane-b-dc3`, Checked, `make -C build xenia-headless
  config=checked_linux`. That branch already contains two core fixes from
  Lane D, which land with it: `lane-d-override` (guest function overrides
  also intercept indirect calls) and `lane-d-mmio-vector` (MMIO emulates
  128-bit vector accesses).
- **Target.** `debug.xex`, sha256 `2d5e4a32…`, the original layout.
- **Harness.** `tools/fork-regress` scenario S1: null GPU, the ymca flow,
  230 s. The command is `dc3_original_args` in `lib/common.sh`. Since this
  lane, it includes `--dc3_headless_autonav=true`.
  `docs/fork/dc3/run_dc3_oracle.sh` passes the same cvars.
- **One-hack A/Bs.** Same binary, with `--dc3_disable_hacks=<id,...>` on one
  side only (`run.sh --extra-arg`, `ab.sh --extra-a`). Every run went through
  the shared slot lock (`/home/free/tmp/fr-slot.sh`). Load is recorded per run.

Two instruments were added in this lane. Neither changes guest state.

- **Debug::Fail tripwire** (`dc3_fail_tripwire.cc`). A host thread reads
  `TheDebug` 0x82F655D8 every 100 ms.
  - When a **worker** thread fails, it logs `mFailThreadMsg` and the captured
    guest stack.
  - When a **main-thread** fail leaves `mFailing` latched for 2 s, it logs the
    message. A main-thread `Debug::Modal(kModalFail)` ends in `Exit()`, so the
    latch means the game is dead. The message is recovered by scanning the
    main thread's guest stack for its strings. It is logged as
    `mFailThreadMsg=00000000 '<msg>'`, so the harness records it.
- **Override audit and fault split.**
  - Every 30 s: for each DC3 override, the handler hits, and whether the
    address was resolved through the indirection table.
  - Every 3 s: `DC3 FAULTS: SIGSEGV= XMA= NON_XMA=`. S1 now gates on
    `NON_XMA == 0`, because every guest write to the XMA register aperture is
    a trapped and emulated device write. With real audio that is about
    100,000 benign "faults" per run.

## Findings: failures the old hacks were hiding

Each finding below was invisible on `main`. The reason is the same in every
case: `Debug::mFailing` was latched before the failure happened, so
`MILO_FAIL` was a silent no-op.

1. **Overrides reached through a vtable were inert.**
   - `BinkMovieImpl::Ready -> 1` and `MoviePanel::IsLoaded -> 1` are virtual
     methods. Before `lane-d-override`, the override audit read
     `handler_hits=0 resolved_indirect=1` for both: the guest body ran.
   - The mechanism: `DemandFunction` compiled the original body for the
     indirection table, so a `bctrl` call bypassed the handler.
   - All 56 NUI overrides, the NUI sequencer, `GotoFirstScreen` and
     `HolmesClientPollKeyboard` are reached by direct call, and were effective.
2. **The early latch was self-inflicted.**
   - On `main`, host automation ran `UIManager::GotoScreen` and other UI code
     on the SkeletonUpdate worker.
   - The guest `BinkMovieImpl::Ready` body (see 1) then failed its
     CHECK_THREAD: "called in the wrong thread (expected 6, cur 15)".
   - The `debug.fail_spin` patch turned that worker fail into a return that
     left `mFailing = 1` for the rest of the run.
   - This lane moved the automation to the main thread and restored the
     faithful spin. With `lane-d-override`, Ready's body no longer runs on the
     worker. **No worker-thread fail has been seen since.**
3. **`Could not find preview.tmov in dir "song_info (ui/song_info.milo)"`**
   (main thread, song_select at about 20 s).
   - Cause: the `bink.sys_init` stub. It skipped `BinkSetMemory`,
     `PlatformInit` and `BinkStartAsyncThread`.
   - With the stub on, 3/3 runs failed (fix-ctrl r1, two-cand r1 and r2).
   - With only the stub off, song_select passes (bis-binksys).
   - On `main` this FAIL decided the S1 pass rate. When the worker fail from
     finding 2 latched `mFailing` first, the run continued (2/5 integrate
     baseline runs). When this FAIL came first, the run stalled at
     song_select. That race was the "loading->game_screen stall" class.
4. **`StandardStream::Play() failed. IsReady=0 mState=0`** (main thread, at
   the start of the song, about 37 s).
   - Cause: `HamAudio::IsReady+0x70 -> li r3,1`, `Game::HandleWait+0x90` and
     the unpause nudge. Together they started the song as soon as
     `HamAudio::FinishLoad` had created its stream, while the stream was still
     `kInit`, before a single `PollStream`.
   - With all three off, `Game::HandleWait` waits and `Game::PostWaitStart`
     unpauses the game by itself.
5. **XMA context kicks livelocked the MMIO handler.**
   - With real XMA contexts (`audio.xmahal_alloc` off), the paced driver's
     render callback reaches `XMAHALWriteAndUnlockContexts`. Its
     `stvx128 v63, r7, r30` (0x82E77C7C) stores 16 bytes to the XMA kick
     registers at 0x7FEA1940.
   - Xenia could not emulate a 128-bit store to MMIO, so the run ended in
     FAULT_LIVELOCK_ABORT, rc 70 (2/2: c4a, c4b).
   - Fixed in core by `lane-d-mmio-vector`. The XMA stub had hidden it, and
     the stub is also what made the paced driver fault in Aug-29 (null
     contexts).
6. **The song clock needs no host drive.**
   - With real contexts and the paced driver, the game reaches gpState=3 on
     its own audio clock (m2: gpState=3 at 198 s).
   - The two 120 BPM drives were writing the TaskMgr timelines at 2x
     (e1: gpState=3 at 195 s, with a later start).

## S1 results

The bar for landing Lane B: at least 3 of 5 S1 runs reach game_screen, the
tripwire reports no `mFailing` latch on those runs, and gpState=3 is reached
on the game's own clock.

### Exploration (one run each, lane-b-dc3 plus the override fix)

| run | hacks off (`--dc3_disable_hacks`) | title | song_select | game_screen | gpState=3 | first main-thread FAIL (tripwire) |
|---|---|---|---|---|---|---|
| fix-ctrl r1 | none | 9 s | 21 s | — | — | 20.4 s `Could not find preview.tmov…` |
| two-cand r1, r2 | bink.impl_ready, movie.panel_is_loaded | 9 s | 18 s | — | — | 19.5 s `Could not find preview.tmov…` (both) |
| bis-binksys | bink.sys_init | 9 s | 18 s | 33 s | — | `StandardStream::Play() failed. IsReady=0 mState=0` |
| c1-cand | all of cluster 1 | 12 s | 21 s | 36 s | — | 37.5 s `StandardStream::Play() failed…` |
| c4a, c4b | cluster 1 + XMA stub + dummy driver (± audio waits) | 12/21 s | 24/30 s | — | — | FAULT_LIVELOCK_ABORT, rc 70 (finding 5; before the mmio fix) |
| e1 | cluster 1 + IsReady/HandleWait/nudge | 12 s | 24 s | 36 s | **195 s** (two host drives) | **none** |
| e2 | e1 + `input.beat_drive` | 12 s | 24 s | 39 s | not within 230 s (one drive) | **none** |
| m2 | e1 + XMA stub + dummy driver + both drives (mmio fix) | 12 s | 21 s | 36 s | **198 s on the game's clock** | **none** (NON_XMA 0) |

### S1 x5, final state (`ebc57f43a`) against the remaining hacks on (`378049f40`)

PENDING

### Before (for comparison)

- **integrate-6bf623353** (the harness baseline, equal to `main`): S1 2/5.
  The three FAILs stalled between song_select and game_screen.
- **This lane's L0 smoke run**, same as `main` plus instruments: a main-thread
  FAIL at 19.7 s latched `mFailing`, and the run stalled.

## What is still patched (original layout)

These hacks remain; see `PATCH_MANIFEST.md` for the full list.

- **NUI.** The NUI SDK overrides and the constant-pose sequencer. The
  calibration bypass, `PauseForSkeletonLoss`, `SpeechMgr::Grammar::Unload` and
  the 33 ms SkeletonUpdate wait. These are Kinect device absence; the fix is a
  NUI HLE (analysis L8).
- **Content/XAM.** `ContentMgr::RefreshDone` and `SaveLoadManager::Activate`
  (analysis L6).
- **UI.** `HamPanel::FocusComponent` and `HamScreen::IsEventDialogOnTop`. Not
  yet A/B'd.
- **Anim.** `HamDirector::SongAnim -> EXPERT`. This changes which animation
  plays.
- **Harness automation, behind `--dc3_headless_autonav`.** Transition force,
  nav bridge, LoadSong repair, and the attract press/force. All of it now runs
  on the guest main thread.

## History (the 2026-09-30 baseline, condensed)

- Gameplay reached game_screen on a null GPU at low host load, in 2/2 runs.
  At load average 100-220 it failed in 0/6 runs. The flow was wall-clock
  driven, and Vulkan failed 2/2 at different points.
- That baseline's patch manifest was taken at `90eb07f81`. It documented the
  measured `mFailing` latch (`'BinkMovieImpl::Ready called in the wrong thread
  (expected 6, cur thread is 15)'`), which is finding 2 above.
