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

The runs are interleaved run by run, one slot per run. Load is the mean of the
1-minute load average over the run.

| run | binary | title | game_screen | gpState=2 playing | gpState=3 | gp2 samples | NON_XMA | latch | load |
|---|---|---|---|---|---|---|---|---|---|
| fin-cand r1 | ebc57f43a (no host clock) | 12 s | 36 s | 42 s | 198.3 s | 78 | 0 | none | 29 |
| fin-cand r2 | ebc57f43a | 12 s | 36 s | 42 s | 198.2 s | 78 | 0 | none | 20 |
| fin-cand r3 | ebc57f43a | 15 s | 39 s | 45 s | 201.5 s | 78 | 0 | none | 103 |
| fin-cand r4 | ebc57f43a | 12 s | 36 s | 42 s | 198.2 s | 78 | 0 | none | 47 |
| fin-cand r5 | ebc57f43a | 12 s | 36 s | 42 s | 198.2 s | 78 | 0 | none | 21 |
| fin-ctrl r1 | 378049f40 (XMA stub, dummy driver, two drives) | 12 s | 36 s | 42 s | 189.2 s | 75 | – | none | 19 |
| fin-ctrl r2 | 378049f40 | 12 s | 36 s | 42 s | 213.4 s | 86 | – | none | 66 |
| fin-ctrl r3 | 378049f40 | 12 s | 36 s | 42 s | 210.6 s | 85 | – | none | 62 |
| fin-ctrl r4 | 378049f40 | 12 s | 36 s | 42 s | 189.4 s | 75 | – | none | 25 |
| fin-ctrl r5 | 378049f40 | 15 s | 39 s | 45 s | 207.6 s | 81 | – | none | 51 |

- **Result: 5/5 PASS on the final state**, gpState=3 on the game's own clock,
  and no `mFailing` latch in any run. The landing bar was at least 3/5.
- **The game's clock is deterministic.** gpState=3 lands at 198.2-201.5 s,
  even at load 103, because the paced audio driver sets the song time.
- **The host drives were not.** They put gpState=3 anywhere from 189 to 213 s
  (5/5 PASS, but by a different clock each run),
  depending on load. This is a direct measure of how much the 120 BPM drives
  perturbed the oracle (TaskMgr timelines drive RndPropAnim/CharClip).
- **S2 (DTA channel) on ebc57f43a: PASS.** The answers are identical to the
  baseline (`3`, refused `no_such_func`, `10`, `object_list main` = 702).
  The first poll is on guest thread 00000006, at 13 s (it was 9 s; the
  scratch block is now allocated on the first request).
- **S1V (Vulkan) on ebc57f43a: PASS, with 38 frames and 0 fail-screen
  frames.** On every earlier baseline, the Milo red fail screen ("Could not
  find preview.tmov") appeared from swap 1200 at song_select. It is gone
  (finding 3). The kept frame is game_screen at swap 2400. This run used the
  pre-merge S1V scenario, i.e. the deferred-replay capture; Lane E
  recommends `--headless_inline_render` for visual goldens.
- **Fault gate sabotage.** `--dc3_disable_hacks=audio.dummy_driver` on
  378049f40 means the paced driver runs with stubbed XMA contexts, the Aug-29
  configuration. That run fails with `max NON_XMA faults 1 != 0 (SIGSEGV 1,
  XMA 0)`, from a null guest access at 12 s, and stalls. The gate catches a
  real fault while the ~700,000 XMA register writes in the PASS runs do not
  trip it.
- **The branch tip after merging main (`4138f93f8`)** passed S0 and 2/2 S1
  runs (gpState=3 at 201.6 and 198.2 s, no latch). compare.py against
  integrate-6bf623353 reports no regression. Its S0 output lists the changed
  defaults: `dc3_clean_content_cache` is now false, and the two
  `headless_capture_*` defaults are Lane E's. Cvars added: `dc3_disable_hacks`,
  `dc3_fail_tripwire`, `dc3_headless_autonav`, `dc3_decomp_disable_stubs`, and
  four from Lane E. Cvars removed: `dc3_gameplay_probe`, `dc3_guest_overrides`,
  `dc3_runtime_telemetry_include_ppc_words`. The ratchets improved:
  title-ID literals outside `titles/` went from 65 to 14, and `/home/free`
  in `src/` from 3 to 0.

### Before (for comparison)

- **integrate-6bf623353** (the harness baseline, equal to `main`): S1 2/5.
  The three FAILs stalled between song_select and game_screen.
- **This lane's L0 smoke run**, same as `main` plus instruments: a main-thread
  FAIL at 19.7 s latched `mFailing`, and the run stalled.

## Lane B2 (2026-10-02)

Branch `dc3-b2` off `main` `1f309687c`. Every run went through
`/home/free/tmp/fr-slot.sh`; the outputs are under
`/home/free/tmp/dc3-b2-runs/`.

### Retirements: four original-layout hacks and seven decomp stubs (`0d44a727d`)

Retired on Lane B's leftover evidence (lane-b-dc3 `c4f2fa891`, x1: S1 5/5
with `seq.loadsong_repair`, `seq.transition_force`, `ui.hampanel_focus` and
`ui.event_dialog_on_top` disabled; d7: S3 2/2 at 627 with the seven decomp
stubs disabled), then re-measured with the code deleted.

S1 x5, `0d44a727d` (cand) interleaved with `main` `1f309687c` (ctrl), the
pinned xenia-ymca flow with `--dc3_headless_autonav`:

| run | title | game_screen | gpState=2 playing | gpState=3 | gp2 samples | NON_XMA | latch | load |
|---|---|---|---|---|---|---|---|---|
| cand r1 | 12.0 s | 39.1 s | 45.1 s | 201.7 s | 79 | 0 | none | 202 |
| cand r2 | 12.0 s | 36.0 s | 42.0 s | 198.3 s | 78 | 0 | none | 27 |
| cand r3 | 15.1 s | 42.1 s | 45.1 s | 201.3 s | 78 | 0 | none | 32 |
| cand r4 | 12.0 s | 36.0 s | 42.0 s | 198.2 s | 78 | 0 | none | 13 |
| cand r5 | 12.0 s | 36.0 s | 42.0 s | 198.2 s | 78 | 0 | none | 21 |
| ctrl r1 | 12.1 s | 36.1 s | 42.1 s | 198.2 s | 78 | 0 | none | 58 |
| ctrl r2 | 12.0 s | 36.0 s | 42.0 s | 198.2 s | 78 | 0 | none | 25 |
| ctrl r3 | 12.0 s | 36.0 s | 42.0 s | 198.2 s | 78 | 0 | none | 21 |
| ctrl r4 | 12.0 s | 36.0 s | 42.0 s | 198.4 s | 78 | 0 | none | 34 |
| ctrl r5 | 12.0 s | 36.0 s | 42.0 s | 198.1 s | 79 | 0 | none | 16 |

- **S1: cand 5/5, ctrl 5/5.** No tripwire line in any run. The two slower
  cand runs (r1, r3) are 3 s later at every milestone from title on, i.e. a
  later boot; r1 ran at load 202.
- **S3 x2 on `0d44a727d`: PASS 2/2**, 627 traps, histogram identical to s66,
  43 distinct LRs. The seven stubs no longer appear in the log.
- S2 x2 on `0d44a727d`: r2 PASS (answers `3`, refused, `10`, 702). r1
  FAILed at 5 s on the `Rnd::DoWorldEnd` boot race ("Other findings"), before
  the channel polled. S1V was run on the final state instead.
- `0d44a727d` is this commit before the rebase onto `5ac644b7d` (`92f762358`
  after it); the later runs use rebased binaries.

### SongAnim: the patch sustained the symptom it was written for (`87f5abce3`)

`anim.song_anim_expert` patched `HamDirector::SongAnim(player)` to return
`SongAnimByDifficulty(kDifficultyExpert)`, because "the routine-builder anim is
empty headless (the remixer never runs)". The remixer does run:
`perform.dta` calls `{[remixer] start_reset}`, whose `ham_objects.dta` body
ends in `{$this reset}`, i.e. `OriginalChoreoRemixer::Reset -> SelectMove ->
DanceRemixer::AddRoutineMove -> MoveMgr::InsertMoveInSong`. In the image,
`InsertMoveInSong` writes its clip and move keys into
`TheHamDirector->SongAnim(player)`. With the patch on, that is the authored
EXPERT `song.anim`. So the routine-builder anim stayed empty because of the
patch, and the remix was written over the authored expert anim.

Measured with S2's gameplay DTA queries (sent 10 s after the first
`gpState=2 paused=0`), binary `0d44a727d`, song `thehustle`, player 0
difficulty 3 (beginner), `merge_moves` 1:

| | `player_song_anim 0` | its clip keys | routine-builder clip keys | expert `song.anim` clip keys | S2 |
|---|---|---|---|---|---|
| patch on (r1-cand S2 r2) | `song.anim` (expert) | 85 | 0 | 85 | PASS |
| patch off (e3 r1, r2) | `player_1_routine_builder.anim` | 71 | 71 | 17 | PASS 2/2 (game_screen 45/36 s, gpState=3 204/198 s; r1 at load 196) |

The 68 extra keys in the expert anim with the patch on are the remix. The
patch is deleted. **This is an intentional oracle change:** the dancers now
evaluate the remixed routine-builder anim, as the 360 does, and the authored
expert anim is no longer overwritten.

### Flow: one flow file, the native port's semantics (`0ec53d6f6`, `a4b5ab551`, `3962c2c83`)

**What the player did before.** `+N` meant N x 50 ms after the last
satisfied `wait_screen`, a press was held 350 ms, `cancel` (and `option`,
`select`, `l1`/`r1`/`l2`/`r2`/`l3`/`r3`, absolute `N` lines) did not parse,
and `wait_screen` was satisfied mid-transition. So the harness ran its own
`xenia-ymca.txt`, which selected **`thehustle`**, not ymca (S2's
`{gamedata get song}` on `0d44a727d`), while every S1 passed.

**What it does now** (`hid/nop/nop_input_driver.cc`, with the DC3 adapter
supplying a frame clock): `Joypad_Native.cpp`'s `GetScriptedButtons` line for
line. The clock is the main-thread hook (one tick per `SystemPoll`, i.e. per
`JoypadPoll`); measured 56.6-59.4 frames/s. DC3's `JoypadPoll` reads presses
that the XinputJoypad thread latched every 4 ms, so a one-frame press is
evaluated in `GetState` on the first call of each frame (a frame no call
landed in is still stepped, and its press is delivered on the next).
`wait_screen` reads `UIManager::InTransition` (`+0x2C`).

**What the original game needs that the native port does not.** On Xenia:
- A HamNavList ignores a confirm while its enter animation runs:
  `{{title_panel find right_hand.hnl} is_animating}` stays 1 for ~0.55 s
  after the title settles (e4, DTA probe), so `+30` (0.5 s) lands inside it.
- A press rejected that way blocks the next ones for about a second: with
  `+30 +45 +60 +75` on the title only `+75` navigated (e2); a single `+45`
  navigates title, main and choose_mode every time (e5, e7, e8, e11, e13).
- Multiuser panes switch ~0.5 s later than native: `seldiff -> startgame` at
  1.4 s, `readywait` ~0.7 s after `play` (e10, e12: `s0_pane`/`s0_ready`
  sampled by DTA). `+110` hits the startgame list's enter animation, so
  `play` must wait to `+150` and `skip_waiting` to `+270`.

`tools/fork-regress/flows/dc3-ymca.txt` is dc3-decomp's `ymca.txt`
(`6d9785b8f`) with those seven lines moved:

| screen | ymca.txt | dc3-ymca.txt |
|---|---|---|
| title, main, choose_mode | `+30 confirm` | `+45 confirm` |
| song_select | `+30 down`, `+90 confirm` | `+45 down`, `+105 confirm` |
| multiuser | `+40 +110 +180 confirm` | `+40 +150 +270 confirm` |

The same file drives the native port (`dc3-native`, `DC3_FAST_BOOT=1`, the
DtaFlow command line) to `title_screen_menu`, `gameplay`, `perform`, **`ymca`
(selected=3 first=2)**, `beginner`, `play`, `skip_waiting` and game_screen,
exactly as `ymca.txt` does; the title's 60-frame fast-boot advance does not
pre-empt `+45`. **The dc3-decomp copy should take this edit** so one file
drives both; until then the harness pins its own.

**The nav bridge goes.** With the flow alone (e13, `f1-b96d7d587`,
`--dc3_disable_hacks=seq.nav_bridge,input.attract_force`): attract ->
autosave_warning -> title through the game after the attract A-press; every
menu, the song and multiuser (`beginner`, `play`, `skip_waiting`) by the flow;
loading -> preloading -> real_loading -> game_screen by the game's own DTA;
`DC3 Script: song 'ymca'` and `{gamedata get song}` = `ymca`;
game_screen 33 s, gpState=3 189 s, 76 gp2 samples, NON_XMA 0, PASS. The
bridge had been skipping each screen's own handler: with the native timing
(e1), title -> wait_main without title_panel's `NAV_SELECT_MSG` left
`$post_load_dest_screen` unset and latched a main-thread FAIL, "Data 0 is not
String (file ui/title/title.dta, line 261)". Without autonav at all (e6), the
attract movie does not end within 30 s, which is why `input.attract_press`
stays.

Final S1 x5 (`3962c2c83`, shared flow, attract press only) interleaved with
`main` `5ac644b7d` on its own `xenia-ymca.txt`: FINAL_PLACEHOLDER

### The valid-skeleton NUI frame does not retire `calib.*` (`5fdc816fa`, reverted in `0f430ecf2`)

Tested at the coordinator's request (docs/fork/nui/NUI_HLE_DESIGN.md: the
constant-pose frame marked all 20 joints NOT_TRACKED, used a 0x1B4 stride and
stamped microseconds where the game reads ms). The fixed frame (0xAB0 bytes,
stride 0x1C0, every joint TRACKED with w = 1, ms timestamps) was measured with
one S2 run per arm, flow dc3-ymca, nav bridge off:

| run | calib.* + pause_for_skeleton_loss | result |
|---|---|---|
| e8 | on | same as the old frame through multiuser |
| e10 | on | DTA at multiuser: `s0_present`/`s1_present` 1, `shell_input num_tracked_skeletons` 1, controller mode 1 |
| e9 | **off** | the title ignores the flow's confirms: one of four lands, at 95 s; never reaches song_select |

So a confident skeleton alone does not let the calibration patches go; the
old frame also shows both sides present at multiuser (e12, `f1`). The fix
was reverted so this lane's final A/B measures only its listed items; it
stays in history for the NUI HLE lane.

### Other findings

- **A boot race in `Rnd::DoWorldEnd`** (`0x82662B18`, null+8 read) on the
  splash render thread (guest thread 0x12) at ~5 s: 1 of the 27 lane-B2 S1/S2
  runs (r1-cand S2 r1, load 14). `mPostProcessors` is walked while the main
  thread is still building it. The splash thread is real since lane B
  retired the `Splash::*` stubs. Recorded, not fixed.
- The harness provenance said `headless=false` for every run; `fr.py` now
  records `true` for `xenia-headless`, which forces it (`701de7e39`).

## What is still patched (original layout)

These hacks remain; see `PATCH_MANIFEST.md` for the full list.

- **NUI.** The 56 NUI SDK overrides and the constant-pose sequencer
  (`nui.*`, `seq.controller_mode`); the calibration bypass (`calib.*`, six),
  `game.pause_for_skeleton_loss`, `speech.grammar_unload` and the two
  SkeletonUpdate patches (`skel.*`). Kinect device absence; the fix is a NUI
  HLE (docs/fork/nui/NUI_HLE_DESIGN.md). A valid skeleton alone does not
  retire `calib.*` (above).
- **Content/XAM.** `content.refresh_done` and `saveload.activate` (analysis
  L6), waiting on core lane D2.
- **Harness input.** `input.attract_press`, behind `--dc3_headless_autonav`:
  A on the attract screen, because the attract movie plays for real.
- **Not a guest change but in the oracle's path:** `mmio.soft_fault_range`
  (inert on the original image).

## History (the 2026-09-30 baseline, condensed)

- Gameplay reached game_screen on a null GPU at low host load, in 2/2 runs.
  At load average 100-220 it failed in 0/6 runs. The flow was wall-clock
  driven, and Vulkan failed 2/2 at different points.
- That baseline's patch manifest was taken at `90eb07f81`. It documented the
  measured `mFailing` latch (`'BinkMovieImpl::Ready called in the wrong thread
  (expected 6, cur thread is 15)'`), which is finding 2 above.
