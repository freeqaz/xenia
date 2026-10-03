# DC3 patch manifest (original `debug.xex`)

Every guest-visible change the `titles/dc3` module makes to Dance Central 3
(title `373307D9`) when it runs the original `debug.xex`
(sha256 `2d5e4a32…`). The ids are the ones `--dc3_disable_hacks` takes
(`src/xenia/titles/dc3/dc3_hacks.cc`). Each applied hack logs one
`DC3 HACK on: <id>` line, and a runtime hack also logs `DC3 HACK fired: <id>`
the first time it acts.

Scope:
- The decomp-layout pack (`titles/dc3/decomp/`) is not listed here. It only
  applies to a rebuilt image detected as the decomp layout, where harness S3
  fingerprints it as a whole. To skip one of its stubs, use
  `--dc3_decomp_disable_stubs=<name,...>`. Lane B2 deleted seven of its stubs
  (see "Retired in lane B2" below).
- "Masks" names the gap each hack covers, using the ids from
  `docs/fork/cleanup/DC3_HACK_GAP_ANALYSIS.md` (O1-O49).
- "Status" is this lane's verdict (Lane B, 2026-10-02). See the end of this
  file for what each status means.

## How to A/B one hack

```
tools/fork-regress/ab.sh $BIN out/cand $BIN out/ctrl --scenarios S1 --repeat 5 \
    --slot --extra-a "--dc3_disable_hacks=<id>"
tools/fork-regress/compare.py --paired out/cand out/ctrl
```

- `--dc3_disable_hacks` accepts exact ids and `prefix*` entries.
- An unknown id is logged as `DC3 HACK: UNKNOWN id … (TAINTED)`. Check that the
  candidate's logs contain the `DC3 HACK off:` lines you expect.
- Before believing an A/B, check two more things:
  - **Tripwire.** `DC3 TRIPWIRE … TAINTED` means Debug::Fail fired on a worker,
    or `mFailing` latched.
  - **Override audit.** Every 30 s, `DC3 HACK audit: <id> <addr> handler_hits=…
    resolved_indirect=…` reports one of: effective, INERT (the guest body ran),
    MIXED, or not reached.

## Launch-time image patches and overrides (`dc3_title.cc`)

| id | guest site | change | masks | status |
|---|---|---|---|---|
| `nui.<Function>` (56 functions + `CXbcImpl::Initialize/DoWork/SendJSON`) | NUI SDK / XBC, statically linked (table in `dc3_title.cc`) | override: return 0 (44) or -1 (12) | O1, O15-O17: no Kinect device; SmartGlass init | kept (title requirement until a NUI HLE exists) |
| `nui.get_next_frame` | `NuiSkeletonGetNextFrame` 0x829C2790 | override: the constant-pose fake skeleton frame (`dc3_nui_sequencer.cc`), and `mInControllerMode` | O2, O6 | kept |
| `mmio.soft_fault_range` | 0x83320000-0x836C0000 | MMIO write soft-fault range | O48: decomp `.data`, **outside the original image** | kept: inert on the original, decomp-only |
| `content.wipe` | host FS | `remove_all(<content>/373307D9)` | O47 | **default off** (`--dc3_clean_content_cache=false`) |
| `saveload.activate` | `SaveLoadManager::Activate` 0x82894A10 | `blr` | O37 | kept (L6, content/XAM) |
| `content.refresh_done` | `ContentMgr::RefreshDone` 0x825FEB48 | `return 1` | O36: XAM cross-title enumeration | kept (L6) |
| `speech.grammar_unload` | `SpeechMgr::Grammar::Unload` 0x82439F38 | `blr` | O7: the NUI S_OK stubs | kept (NUI) |
| `calib.player_present_guard`, `calib.choose_player_sides`, `calib.warning_data`, `calib.nav_data`, `calib.wait_recovery`, `calib.exit_controller_mode` | SkeletonChooser / ShellInput (0x8290834C, 0x82909968, 0x82907880, 0x82909340, 0x82904CD0, 0x82902748) | `nop` / `blr` / rewrite | O8-O13: a constant skeleton is not a calibrated player | kept (NUI) |
| `game.pause_for_skeleton_loss` | `Game::PauseForSkeletonLoss` 0x82866D50 | `blr` | O14 | kept (NUI) |

## `dc3_hack_pack_skeleton.cc` (with `--fake_kinect_data`)

| id | guest site | change | masks | status |
|---|---|---|---|---|
| `skel.wait_33ms` | `SkeletonUpdateThread`+0xA4 0x8242E74C | INFINITE wait -> 33 ms | O4 | kept (NUI) |
| `skel.is_override_nop` | `SkeletonUpdate::Update`+0x40 0x8242E1B0 | `nop` | O5 | kept (NUI) |

These were removed:

| what | id | removed |
|---|---|---|
| PPC constant-frame stub over 0x829C2790 | `skel.ppc_get_next_frame` | deleted. Shadowed by `nui.get_next_frame`: the audit measured handler hits on every frame and showed the guest body never resolved |
| `Debug::Fail` thread-fail spin -> return, 0x825CE2DC | `debug.fail_spin` | deleted. The spin is faithful again (see BASELINE.md, "Debug::Fail") |

## Runtime guest writers and guest calls

| id | where | what | masks | status |
|---|---|---|---|---|
| `seq.controller_mode` | NUI callback (worker) | `TheGestureMgr`+0x426D := 1 each frame | O6 | kept (NUI) |
| `input.attract_press` | pad poll | press A every 3 s while attract blocks `wait_screen title_screen` | O45: the attract movie plays for real (Bink is real since lane B), and the shared flow has no attract step because the native port's movie fails to open | `--dc3_headless_autonav` only; the one host input left. It takes attract -> autosave_warning -> title through the game's own handlers |

These were removed:

| what | id | removed |
|---|---|---|
| "Transition diag": Executes `UIScreen::CheckIsLoaded/Exiting/Entering` off-thread | `seq.transition_diag` | deleted |
| `--dc3_gameplay_probe` GATE PROBE / PKPROBE (guest Executes per frame) | – | deleted |

## Instruments (no guest change)

| what | where | notes |
|---|---|---|
| Debug::Fail tripwire | host probe thread (`dc3_fail_tripwire.cc`) | `--dc3_fail_tripwire` (on). Reads `TheDebug` 0x82F655D8 |
| Override audit | same thread, every 30 s | per override: handler hits, `resolved_indirect` |
| `gpState=` gameplay probe | pad poll (`dc3_scripted_input.cc`) | read-only; counted by the harness |
| `screen ->` and `song` lines | pad poll (`dc3_scripted_input.cc`) | read-only: one line per screen change, and the selected song (`TheGameData`+0x30) once on game_screen. S1 milestones and the S1 song criterion |
| DTA channel | main-thread hook | `--dc3_dta_channel`. Scratch is allocated on the first request |
| main-thread hook | `HolmesClientPollKeyboard` 0x825F0F78 override | installed only when a task exists: the DTA channel, or the scripted-input frame clock (`input_frame_clock`, a no-op task whose poll count is the flow's frame number) when a screen-aware flow is loaded. It skips the stock body, which does nothing while `gHolmesStream` is 0 (logged; a non-zero value is logged as TAINTED) |
| IK telemetry | code caves in the zero padding after `.text` | `--dc3_ik_telemetry`. Refuses if there is no padding. It no longer writes inside `UtilDrawPlane` |

## Retired in this lane (2026-10-02)

All of these are measured in `BASELINE.md`, on lane-b-dc3 with the cpu
override fix merged.

| id | site | why it existed | evidence it is gone for good |
|---|---|---|---|
| `bink.sys_init` | `BinkMovieSys::Init` 0x82E214A8 | "BinkStartAsyncThread hangs headless" (lost-resume) | With the stub ON, song_select MILO_FAILs "Could not find preview.tmov" 3/3. Off, the real Init runs and song_select passes. Finding 3 |
| `bink.impl_ready`, `movie.panel_is_loaded` | 0x82E221C8, 0x82E0EFE8 (overrides) | attract movie / panel never ready | INERT before the override fix (vtable only). With the fix they do not prevent the preview.tmov FAIL (2/2). Not needed once Init is real |
| `movie.poll` | `Movie::Poll` 0x82555CB8 | Bink chain | not needed: S1 x5 A/B |
| `splash.prepare_next/begin_splasher/suspend/resume` | 0x82554388, 0x825554C8, 0x82553BE0, 0x82553D68 | lost-resume (CreateThread suspended + ResumeThread) | not needed: S1 x5 A/B |
| `ui.goto_first_screen` | 0x8277B140 (override) | "boot-ordering race" | not needed: S1 x5 A/B |
| `io.cd_read_done` | `CDReadDone` 0x826026E0 | the separate-IOSB OVERLAPPED theory, refuted by `ac0052e5b` | not needed: S1 x5 A/B |
| `audio.hamaudio_ready`, `audio.handle_wait`, `input.unpause_nudge` | 0x8252BA50, 0x82867318, Game +0x5E/+0x60/+0xA4 | "HamAudio never reaches IsReady headless" | They CAUSED "StandardStream::Play() failed. IsReady=0 mState=0" (finding 4). Without them, Game::PostWaitStart unpauses by itself |
| `audio.xmahal_alloc`, `audio.dummy_driver` | `XMAHALAllocateContexts` 0x82E77250; `--nop_audio_driver` | "stubbed contexts; the render callback must not run" | Once the MMIO handler emulates the HAL's 16-byte `stvx128` context kicks (lane-d-mmio-vector), real contexts and the paced driver run cleanly (finding 5) |
| `seq.beat_drive`, `input.beat_drive` | TheTaskMgr 0x82F64A58 timelines | "song time comes from the audio stream, which never advances" | With real audio the game's clock reaches gpState=3 with no host clock writer (finding 6) |
| `skel.ppc_get_next_frame` | 0x829C2790 | pre-override fake frame | shadowed by `nui.get_next_frame` (audit) |
| `debug.fail_spin` | 0x825CE2DC | survive a worker FAIL | faithful spin restored; the worker FAIL it survived was self-inflicted (finding 2) |
| `seq.transition_diag`, `--dc3_gameplay_probe` | off-thread Executes | diagnostics | deleted |

## Retired in lane B2 (2026-10-02)

Measured in `BASELINE.md`, "Lane B2". The ids are gone from the known-id
table, so naming one in `--dc3_disable_hacks` is now a launch error.

| id / stub | site | why it existed | evidence it is gone for good |
|---|---|---|---|
| `ui.hampanel_focus` | `HamPanel::FocusComponent` 0x828EFE90 | "focus crash": `TheHamUI.EventDialogPanel()` null when the host forced screens before HamUI loaded (O28) | Lane B x1: S1 5/5 with it off. B2 r1: S1 5/5 with the code deleted, 5/5 for main interleaved |
| `ui.event_dialog_on_top` | `HamScreen::IsEventDialogOnTop` 0x829626D8 | same (O29) | same runs |
| `seq.transition_force` | main-thread autonav | force-enter/complete transitions stuck for 120 ticks (O30) | same runs. Every transition completes by itself once the Bink/Splash/HamAudio chain is real |
| `seq.loadsong_repair` | main-thread autonav | `DataReadFile` + `HamSongMgr::AddSongs` on a guessed path and a constructed `ymca` Symbol (O33) | same runs. The flow selects the song on song_select |
| decomp `Splash::PrepareNext`, `Splash::BeginSplasher`, `Splash::Suspend`, `Splash::Resume` | decomp layout | lost-resume (thread created suspended, resume lost) | Lane B d7: S3 2/2 PASS, 627 traps, histogram identical, with the 7 off. B2: S3 2/2 PASS, 627, with the code deleted |
| decomp `BinkStartAsyncThread`, `BinkMovieSys::PlatformInit` | decomp layout (both resolve to one noop 0x826B0EF0) | same | same |
| `seq.nav_bridge` | main-thread autonav: `UIManager::GotoScreen` walk attract -> ... -> game_screen, `merge_busy` hold, `--dc3_game_screen_real_goto` | Kinect-only menus; scripted A "does not navigate" (O31, O32) | It skipped each screen's own handler (title -> wait_main without `NAV_SELECT_MSG` latched "Data 0 is not String (file ui/title/title.dta, line 261)"). The native-semantics player and `flows/dc3-ymca.txt` drive every screen: e13 S2 PASS with it off, then S1 x5 (BASELINE "Flow"). The cvar `dc3_game_screen_real_goto` is deleted |
| `input.attract_force` | scripted-input adapter | 4 MiB heap scan + UIManager stomp attract -> title (O46) | The attract press goes through the game: attract -> autosave_warning -> title (e7, e13) |
| `anim.song_anim_expert` | `HamDirector::SongAnim` 0x82475578 -> `li r4,2; b SongAnimByDifficulty` | "routine-builder anim empty headless, the remixer never runs" (O34) | Self-sustaining: `MoveMgr::InsertMoveInSong` writes the remix into `TheHamDirector->SongAnim(player)`, which the patch made the authored EXPERT song.anim. DTA, patch on: SongAnim = song.anim, 85 clip keys, routine-builder 0. Off: SongAnim = player_1_routine_builder.anim, 71 keys, expert song.anim back to its authored 17. S2 2/2 PASS off (e3). **Intentional oracle change**: the dancers now evaluate the remixed routine, as on the 360 |
| decomp `UIManager::GotoFirstScreen` | decomp layout | "ChunkStream's async I/O threads fail to start" (same race) | same |

## Status legend

| status | meaning |
|---|---|
| kept (NUI) | Needed until a NUI HLE with a pose source exists (analysis L8). A Kinect title genuinely requires it |
| kept (L6) | Content/XAM enumeration, owned by a later lane |
| `--dc3_headless_autonav` only | Harness input. Off by default; the harness passes the cvar. Since lane B2 it arms only `input.attract_press` |
| see results | Measured in this lane. The outcome is in `BASELINE.md` |
