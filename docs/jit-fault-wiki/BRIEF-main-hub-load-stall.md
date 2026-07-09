# BRIEF — RB3DX main_hub load stall (post-OOM-fix)

**Status:** SPLASH GATE FIXED (2026-07-09, commit `9096dd4d0`) — see
"Fable investigation #3" + "Fix landed" below. RB3DX now advances
`splash_screen → first_time_calibration`; the residual blocker moved one
first-boot flow downstream (interactive `cal_audio_screen` A/V-latency
calibration, unreachable headless with null audio + fixed-time scripted input).
Prior status: OPEN, downstream of the now-fixed heap-OOM.
**For:** a fresh investigator. No prior context assumed.
**Sources:** the stalled boot log `/tmp/rb3dxnav/hub2/boot.log` (18,540 captured
frames / 19,413 VdSwaps, cited by line inline), the shorter `/tmp/rb3dxnav/hub/boot.log`,
frames in `/tmp/rb3dxnav/hub2/*.ppm`, `CRASH-REPORT-main-hub-oom.md` (OOM context +
repro), the memory log `project_xenia_seh_fault_wiki.md` (line 65 = this frontier),
and two decomp reads: the RB3 native port (`/home/free/code/milohax/rb3`) and the
rb3-xenon decomp (`/home/free/code/milohax/rb3-xenon`). No emulator was run for this
brief; it synthesizes existing evidence + decomp.

---

## 1. Executive summary

The heap-OOM wall (crash report §4; fixed by `e248d624c`, posix `GetInfo::total_size`)
is gone: RB3DX now boots **deterministically** past its animated title **into
`main_hub` load** — then **stalls in a loading-wipe transition that never completes**.
It is **not a crash** (no host/guest fault storm; SIGSEGV=168 total over 357 s = benign,
not a livelock). The game does **zero file I/O after ~frame 600** and idle-loops the
present thread rendering a blue-wipe/black cycle for the remaining ~350 s.
**Leading hypothesis (revised):** *not* the Bink background video — both decomp reads
show `main_background.bik` is a **decorative `TexMovie`, not a load-gate**. The stall
is a **non-movie completion gate** in the main_hub/boot-chain Poll: most likely a
scene `DirLoader`/`UIPanel` "loaded" signal or a **NetSync lock-step transition
permission** that never becomes true headless.

---

## 2. Reproduction

Emulator: `/home/free/code/milohax/xenia`, branch `headless-vulkan-linux`, at/after
`e248d624c` (the GetInfo fix; builds on `b803faab1` circuit-breaker and `fb864e3e`
zero-page). Binary `build/bin/Linux/Checked/xenia-headless`. Boot dir `/tmp/rb3dxboot`
(`default.xex` = RB3DX TitleID `45410914` + symlinks `gen`/`charnames`/`AvatarAwards`/
`nxeart` → `/srv/torrents/games/arbys/rb3/`). Actual flags used for the `hub2` capture
(`hub2/boot.log:1-20`): `--protect_zero=false --gpu=vulkan` (NVIDIA RTX 3090),
scripted START/A presses at 8–56 s, `--headless_timeout_ms=360000`, frame dump interval.

**Recognizing the stall vs the old crash:**
- **Old OOM (fixed):** `FAULT LIVELOCK … guest EA FFFFFFFC`, SIGSEGV climbing ~11,640/s,
  VdSwaps stop, `_Exit(70)`.
- **This stall:** `TIMEOUT: 360000ms reached` (`hub2/boot.log:679861`); VdSwaps run to
  the end (19,413); SIGSEGV=168 total (`:673263`); frames cycle blue-wipe/black forever.
Deterministic now: memory log line 65 records 18,553 VdSwaps, 0 wedge/OOM.

---

## 3. Symptom & evidence

**Frames (`/tmp/rb3dxnav/hub2/`, PPM):** early `frame_0060` is 100 %-nonzero dark
magenta = the animated title/attract (the magenta scanline = known headless
tiled-readback artifact, non-blocking; crash report §6). From ~`frame_3840` on it locks
into a two-state cycle: **50 %-nonzero pure blue** (`mean RGB [0,0,74]`, e.g.
`frame_18540`) = the loading-wipe half-covering the frame, and **0 % black**
(`frame_16800`). The `RSTAB: CAPTURE` verdicts confirm: 175× `nonzero_pct=50 SCENE`,
87× `nonzero_pct=0 HUD`, last capture `frame=18540 … nonzero_pct=50 verdict=SCENE`
(`:679835`). **Frontbuffer is pinned at `0x1D1C8000` for the entire run** (title *and*
wipe) — no screen swap ever completes.

**VdSwap/present behavior:** the render/present thread `F8000004`/`F8000028` keeps
issuing `VdSwap` + `XE_SWAP` + `FlushDeferredDraws` to the end (tail `:679835-679860`)
— the loop is alive, just producing the same wipe.

**File-load list (all `NtCreateFile`, `hub2/boot.log`):**
| File | Result | What it is |
|---|---|---|
| `d:\gen\main_xbox_0..9.ark`, `patch_xbox_0.ark`, `*.hdr` | OK (`:911-968`) | base + Deluxe (`LOLZ`) arks |
| `d:\charnames.zbm` | OK (`:3520`) | character name table |
| `rbdxcache:\rbdxcache` | **OK** (`:5624,5632`) | Deluxe cache (the old #600 wall) |
| `globaloptions:\globaloptions` | **OK** (`:5697,5708`) | global options blob |
| `game:\main_background.bik` ×3 | **FAIL 0xC000000F** (`:3078-3088`) | Deluxe menu bg **video** — absent |
| `game:\dx_{settings,playlist,track_theme_default,modifiers,values,event_config}.dta`, `log.dta` | **FAIL 0xC000000F/22** (`:3022-3070`) | RB3DX Deluxe configs — absent |
| `gd:\dev_hdd0\game\blus30463\usrdir\dx_high_memory.dta` | FAIL device-not-found (`:2962,3065`) | RB3DX probing a **PS3** path (tolerated) |

> **Correction to the task premise:** the game does **not successfully read** the
> `dx_*.dta` configs or `main_background.bik` — it *attempts* them and every RB3DX
> **loose file** is **absent** (not deployed in the boot dir or `/srv`). Only the arks +
> `rbdxcache`/`globaloptions` succeed. The bik is tried 3× at ~frame 240 and **never
> retried**. So this is *not* "a Bink that loaded and won't advance" — the file never
> opens.

**Last I/O:** last `NtCreateFile` is `globaloptions` at `:5697`; last `XamContent*` at
`:5708`. After that, **no file I/O and no XamContent for the entire remaining
~18,000 frames.**

---

## 4. What the game is doing at the stall

**Idle-waiting on an in-memory completion event — not polling a file, not spinning.**
Evidence: (a) zero I/O after frame ~600; (b) the final Thread Status Report
(`:673263`, 26 threads) shows the **main/present thread** (Thread 6, SP `0x7018F840`,
the `F8000028` frame loop) live in graphics code (`LR=0x8285B000`, `r4=0x433D8600` =
the `VdSetGraphicsInterruptCallback user_data` from `:1019`), while **almost every
worker thread is parked in a kernel wait** — `LR=0x82844CF8` (threads 8-class, 11, 12,
16, 19, 24, 27, 28) and `LR=0x82845144` (7, 18, 20, 21, 22, 23), i.e. `Ke*Wait`
returning to idle. Parked workers = the load **work** is done; the gate the main-thread
Poll checks each frame simply never flips. Three threads have deeper live chains and
are the load-worker candidates worth naming: **Thread 15** (`0x8252AB34` ← `0x827ABA8C`
← `0x8252793C`, its start func `0x8252A3B0`), **Thread 17** (`0x8283FB04` ← `0x82AF2C2C`
← `0x82AC6BB8` ← `0x82AC6E00` ← `0x82AC5C04` ← `0x82B0074C`, with a timeout-looking
`lr_sp4=0xFFFFFE4A`), **Thread 10** (`0x82BF3B2C` ← `0x82BF4C1C` ← `0x82BF3BAC`).
The steady blue-wipe is RB3's generic **loading-transition overlay**; it cycles
because `UIManager`'s transition never sees its target screen report loaded (§5).

Audio note: `XAudioRegisterRenderDriverClient: CreateDriver failed (C0000002),
returning dummy handle` (`:1065`); `XmaContext: reset context 0..3` (`:10924,15532`).
Audio is stubbed — relevant only if a gate waits on audio/XMA progress (H1 sub-case).

---

## 5. Ranked hypotheses

### H1 — Bink `main_background` video gating main_hub — **REFUTED as the gate (LOW, ~10-15%)**
Both decomp reads independently show the animated menu background is a **decorative
`TexMovie`**, not a load-gating panel:
- rb3-xenon: `main_background.bik` is a `TexMovie` `bink_movie_file` prop
  (`rb3-xenon/src/system/movie/TexMovie.cpp:56-71,236-263`); root-relative `game:\` path
  matches a `TexMovie`, **not** a `MoviePanel` (which prefixes `videos/`,
  `rb3-xenon/src/system/meta/MoviePanel.cpp:313`). `TexMovie` has **no `IsLoaded`
  override** and is not consulted by the scene `DirLoader` completion; a failed movie is
  just ended (`TexMovie.cpp:147-158`). The screen-load gate `UIManager::Poll →
  UIScreen::CheckIsLoaded → UIPanel::CheckIsLoaded → DirLoader::IsLoaded`
  (`rb3-xenon/src/system/ui/UI.cpp:710,731`, `UIScreen.cpp:177-191`,
  `UIPanel.cpp:57-80,246-257`, `DirLoader.cpp:109,817,832`) contains **no movie check**,
  and `MainHubPanel` has **zero** movie references
  (`rb3-xenon/src/band3/meta_band/MainHubPanel.cpp:108-125`).
- native port: **no `main_background.bik` exists in vanilla RB3 at all** — it is an
  RB3DX addition; the vanilla menu is a 3D shell scene. Any menu-adjacent `TexMovie` is
  silently no-op'd on native and never gates the menu; a failed/absent bik is graceful
  (`rb3/native/src/rb3_movie_native.cpp:146-162,205-217,244`, `rb3/src/system/movie/
  Movie.cpp:259-266,275,598`, `TexMovie.cpp:66,116-124`).

*Against fully dismissing:* RB3DX is a **mod**; it *added* `main_background.bik` and may
have modified `main_hub`/its shell in ways not present in the vanilla decomp. But the
engine load-gate architecture is shared, and the observed behavior (open fails once,
never retried, no movie-ready poll possible for a `TexMovie`) fits "decorative, ignored"
far better than "hard-wait." The headless-decode / XMA-audio-track sub-case is **moot**
here because the file never opens — there is nothing decoding to stall. Confirm/kill:
deploy a real `main_background.bik` (§6) and see if the stall persists.

### H2 — a non-movie completion gate the boot-chain Poll waits on — **LEADING (~55%)**
The wipe cycles because `UIManager`'s transition never sees the target screen loaded.
Two concrete sub-gates:
- **H2a — scene `DirLoader`/`UIPanel` "loaded" never fires.** `UIPanel::IsLoaded` gates
  on `mLoader->IsLoaded()` **plus** an `"is_loaded"` message
  (`rb3-xenon/src/system/ui/UIPanel.cpp:57-80,246-257`). If a main_hub panel's scene
  loader never reaches `DoneLoading` (`DirLoader.cpp:817,832`) or the `"is_loaded"`
  message never arrives, the screen never loads. *For:* parked workers + no I/O =
  work-done-but-gate-false; the loading overlay is exactly this state. *Against:* all
  ark/cache reads succeeded, so raw asset streaming looks complete.
- **H2b — NetSync lock-step transition permission never granted.** The boot flow gates
  screen transitions on `NetSync::…IsTransitionAllowed`
  (`rb3-xenon/src/band3/meta_band/NetSync.cpp:240-256`, flow comment mentions
  `intro_movie_screen` at `:248`). Headless with **stubbed networking**, a lock-step /
  session-sync permission can stay false forever = a perfect silent idle-loop. *For:*
  fits "past title, never finishes," no fault, no I/O, threads parked; the RB3 native
  port already needed NetSession shims elsewhere (song-end). *Against:* not yet observed
  in this log — needs a trace to confirm which gate is consulted.
Confirm: entry-hook `UIManager::Poll` / `UIScreen::CheckIsLoaded` (or the specific
panel's `IsLoaded`) and log its return + which sub-condition is false each frame.

### H3 — `UIManager::GotoFirstScreen` / milo scene step failing silently — **MEDIUM-LOW (~15%)**
The first-screen bring-up could be mid-flight (a milo objectdir failing to instantiate,
a factory not registered, a Deluxe object type missing because the `dx_*.dta` configs
are absent). Overlaps H2a. *For:* the entire RB3DX Deluxe **config set is missing**
(§3) — `dx_settings/values/event_config` define the Deluxe main_hub; their absence could
leave the screen definition incomplete. *Against:* no guest assert/`MILO_FAIL`/error
print anywhere in the log (only benign `ResolvePath device-not-found`).
Confirm: deploy the `dx_*.dta` set (§6) and re-run; trace `GotoFirstScreen`.

### H4 — deployment confound: the RB3DX loose-file set is simply not staged — **CROSS-CUTTING (~15%)**
Every RB3DX loose `game:\` file (bik + 7 `dx_*.dta`) is **absent** from `/tmp/rb3dxboot`
and `/srv`. If RB3DX expects these as loose files (vs packed in the `LOLZ`
`patch_xbox_0.ark` with the loose paths as override probes), their absence is a
**content/deployment** root, not an emulator bug. This is the **cheapest first test** and
de-risks H1/H2a/H3 at once. *Unknown:* whether the Deluxe assets also live inside the
patch ark (then the loose failures are expected red herrings). Confirm: locate the RB3DX
loose files (mod distribution) and stage them; or dump `patch_xbox_0.ark`'s TOC for
`main_background`/`dx_*`.

---

## 6. Investigation levers (concrete, cheapest-first)

1. **Deploy the RB3DX loose files** (`main_background.bik` + `dx_*.dta`) into
   `/tmp/rb3dxboot` and re-run. Removes the H4 confound and directly tests whether a
   present bik / present config set clears the stall. If it still stalls → the bik and
   configs are red herrings, focus H2b/H2a.
2. **Entry-hook the load-gate, not the movie.** Using the fork's proven guest-entry-hook
   precedent — the `__savegprlr_23` override at `0x82829244` (memory log line 64; crash
   report §7.4) and the `ArkFile::Read` PPC trampoline (`31883d689`), *not* synthetic PPC
   caves — trap `UIManager::Poll` / `UIScreen::CheckIsLoaded` and log the returned
   loaded-flag + the failing sub-condition each frame. Map guest addresses via
   rb3-xenon symbols `/home/free/code/milohax/rb3-xenon/config/45410914/symbols.txt` and
   the Ghidra service `http://ghidra.local:8001/mcp`.
3. **Trace `NetSync::IsTransitionAllowed`** (`NetSync.cpp:240-256`) — if it is on the
   main_hub entry path and returns false headless, that is the stall; a title-gated
   force-allow is a candidate mitigation (§7). Check whether a session/lock-step object
   is waiting on a peer that never appears.
4. **Name Threads 10/15/17** (§4 addresses) via Ghidra — identify whether any is a
   loader/decode thread parked on a job that never arrives, vs a benign idle worker.
5. **Compare to the native port's satisfied path:** the native port renders main_hub
   fine with **no** background video (agent-confirmed), so the *content* main_hub needs
   is only the shell scene + Deluxe config — reinforcing that the gate is a
   load/transition condition, not the video.
6. **Only if 1 shows a present bik still stalls:** trace the Bink `fn_82C1xxxx` SDK
   functions + the un-decompiled `BinkMovieImpl::BeginFromFile/Ready/Poll`
   (`rb3-xenon/src/system/moviebink/BinkMovieImpl.cpp:117-143` are HX_NATIVE stubs; real
   bodies only in the binary) and check the XMA/`XAudio` dummy-handle path (`:1065`).

---

## 7. Constraints on any fix

- **DC3-safe, mandatory.** DC3 (title `0x373307D9`) boots to gameplay on this fork and
  must not regress. Any change must be cvar- and/or title-id-gated to `0x45410914`, or
  proven inert for DC3 (the GetInfo fix and circuit-breaker already are).
- **Prefer a general emulator-correctness fix** over a per-title hack (project
  direction). But: if the native port confirms main_hub does not truly need the video /
  a net peer, a **headless-only or title-gated skip** (of a bik gate or a NetSync
  transition wait) is an **acceptable outcome** — the goal is reaching an interactive
  main_hub for the same-instrument work, not byte-faithfulness of a menu video.
- Do **not** paper over a wait by mapping/zeroing memory (crash report §8): a benign
  return perpetuates the idle-loop instead of surfacing the real gate.

---

## 8. Key files / addresses appendix

| Item | Value |
|---|---|
| Stalled log (definitive) | `/tmp/rb3dxnav/hub2/boot.log` (19,413 VdSwaps; `TIMEOUT` `:679861`) |
| Shorter transition log | `/tmp/rb3dxnav/hub/boot.log` (same wipe, `TIMEOUT 220000ms`) |
| Frames | `/tmp/rb3dxnav/hub2/frame_*.ppm` (blue wipe = `frame_18540`; black = `frame_16800`; title = `frame_0060`) |
| OOM fix commit (unblocked this) | `e248d624c` (posix `GetInfo::total_size`) |
| Frontbuffer (pinned) | `0x1D1C8000` |
| bik open fails | `hub2/boot.log:3078-3088` (`0xC000000F`) |
| dx_*.dta / log.dta fail | `:3022-3070` |
| rbdxcache / globaloptions OK | `:5624`, `:5697` (last I/O) |
| Thread Status Report | `:673263` (SIGSEGV=168, last_fault=`0x17FEA1A90`→guest `0x7FEA1A90`, last_rip=`0xA0A395FF`, crash_guest=`0x82BCEFE4`) |
| Parked-worker LRs (`Ke*Wait`) | `0x82844CF8`, `0x82845144` |
| Load-worker candidate threads | T15 `0x8252AB34`(start `0x8252A3B0`), T17 `0x8283FB04`(→`0x82AC*`/`0x82AF*`/`0x82B0074C`), T10 `0x82BF3B2C` |
| Load-gate chain (decomp) | `rb3-xenon` `ui/UI.cpp:710,731`, `UIScreen.cpp:177-191`, `UIPanel.cpp:57-80,246-257`, `obj/DirLoader.cpp:109,817,832` |
| Movie is decorative | `rb3-xenon` `movie/TexMovie.cpp:56-71,147-158,236-263`; only gate = `meta/MoviePanel.cpp:230-243,313` (not main_hub) |
| NetSync transition gate | `rb3-xenon` `band3/meta_band/NetSync.cpp:240-256` (`:248` intro_movie_screen) |
| Native-port movie shim | `rb3/native/src/rb3_movie_native.cpp:146-162,205-217`; `rb3/src/system/movie/Movie.cpp:259-266,275,598` |
| Guest-entry-hook precedent | `__savegprlr_23` override `0x82829244`; `ArkFile::Read` trampoline `31883d689`; milo-trace |
| RB3DX symbols | `/home/free/code/milohax/rb3-xenon/config/45410914/symbols.txt` (address-only `fn_XXXXXXXX`) |
| Ghidra service | `http://ghidra.local:8001/mcp` |

---

## 9. Known unknowns

- **Which gate the per-frame Poll is stuck on** is not yet traced (H2a scene-loader vs
  H2b NetSync). This is the linchpin — resolve it first (lever §6.2/§6.3).
- **Are the RB3DX loose files (bik + `dx_*.dta`) supposed to be loose, or packed in the
  `LOLZ` `patch_xbox_0.ark`?** If packed, the loose-open failures are expected red
  herrings and the assets load via ArkFile (invisible in `NtCreateFile`). Undetermined.
- **RB3DX main_hub modifications** aren't in the vanilla rb3/rb3-xenon decomp — the mod
  may add a video-background gate the vanilla engine lacks. The decomp evidence is
  architectural, not from RB3DX's own modified bodies.
- **Real Xbox `BinkMovieImpl` bodies are not decompiled** (only HX_NATIVE stubs +
  header field semantics); missing-file Bink behavior on hardware/Xenia is inferred.
- **SIGSEGV=168** (not 0): a small, steady count of benign recovered soft-faults
  (guest EA in the `0x7F…` hole, guest PC `0x82BCEFE4`) — unrelated to the stall and
  nothing like the OOM's 11,640/s livelock, but not independently root-caused here.
- **Silent-stall vs stuck-title** (crash report §10) are now the *same* deterministic
  outcome post-fix; whether the intermittent 100 %-nonzero frames up to frame 15660 are
  the attract scene still compositing under the wipe was not step-traced.

---

## Fable investigation (2026-07-08)

**Verdict: the brief's framing was wrong in one load-bearing way — the game never
reaches `main_hub` load at all. It is stuck one screen *earlier*, on
`splash_screen`, at the overshell/session local-user-join gate. Both H1 (bik) and
the loose-file confound (H4) are RED HERRINGS. H2b (NetSync transition lock-step)
is REFUTED. The true gate is the splash panel's DTA state machine waiting on an
`overshell_allowing_input(TRUE)` message that never fires because the local-user
NetSession join round-trip never completes headless — the *exact* gate the RB3
native port hit and fixed.**

Method: added a title-gated (`0x45410914`), default-off, read-only guest-memory
sampler (`--rb3dx_ui_probe`, `emulator.cc`) that every ~2 s reads `TheBandUI`
(`0x82DFD2B0`) transition state + current/transition screen names + per-panel load
states, and walks `ObjectDir::sMainDir` (`0x82E054B8`) to name every main-dir
object; plus a temp `XamInputGetState` diagnostic proving scripted input reaches
the guest. No guest patches; DC3-inert. Logs: `/tmp/rb3dx-hub-investigate/`
(`probe6-boot.log` = definitive; `probe4-boot.log` = input-delivery proof).

### (a) H4 — RESOLVED: loose files are RED HERRINGS

Extracted `gen/patch_xbox.hdr` (the `LOLZ` Deluxe ark) with `arkhelper ark2dir`
→ 6136 files. The RB3DX Deluxe payload **is packed in the ark**:
`gen/dx_settings.dtb`, `videos/dx_intro_movie.bik`, and the entire `dx/` tree
(`dx/ui/gen/dx_ui_{screens,macros,init}.dtb`, `dx/read_write/gen/*`, …) are all
present. So the game **does** get its Deluxe config — via `ArkFile`, invisible to
`NtCreateFile`. The failing loose `game:\` opens are decoded from
`dx/read_write/gen/dx_paths.dtb` (`DX_*_FILE_PATH` = `GAME:/…` then `sd:/…` then
`GD:/…` PS3 probes): they are the platform-path probe cascade, not a hard dep.

- **`main_background.bik`** is **not** in the ark and **not** loose — but every
  consumer guards it with `{file_exists DX_MENU_BACKGROUND_BIK_PATH}`
  (`dx/ui/gen/dx_ui_macros.dtb` `DX_SV_PANEL`/`DX_SV4_PANEL`;
  `dx/ui/gen/dx_ui_screens.dtb` `splash.tmov`; `ui/main/…/main_hub.dtb:1007`
  picks `song_movie_panel` **else** `sv3_panel`). Absent → falls back to the
  static `sv3/sv4/sv8_panel` with no error. Decorative, exactly as the brief's
  H1 decomp read concluded. **RED HERRING.**
- **`dx_playlist / dx_modifiers / dx_values / dx_event_config / log.dta`** are
  *runtime user-write* files (written by the `#ifdef _SHIP` `dx_*_dta_writer`
  funcs in `dx_read_write_funcs`), read via `{read_file …}` whose engine impl
  `DataReadFile` (`obj/DataFile.cpp:683`) returns `nullptr` gracefully on a
  missing file (`FileStream::Fail` → `MILO_WARN` → null; `OnReadFile`
  `DataFunc.cpp:1053` returns `0`). Absent in a fresh install by design.
  **RED HERRING.**

### (b) The EXACT gate

Probe result, held for the entire run (34/36 samples; 2 early on
`intro_movie_screen`), never anything else:

```
curScreen = 0x44EFA3F0 'splash_screen'   (== maindir['splash_screen'])
mTransitionState = 0 (kTransitionNone)   transScreen = 0x0 (null)
```

- **`mTransitionState == kTransitionNone` forever with a null transition screen**
  ⇒ there is **no pending screen transition**. `NetSync`/`UIManager` transition
  lock-step (`IsBlockingTransition`, `NetSync.cpp:240-256`) only runs *during*
  `InTransition()`. **H2b REFUTED.** `main_hub`'s `DirLoader`/`UIPanel`
  `is_loaded` (`is_initial_load_done`) is never consulted — `main_hub` is never
  entered. **H2a is moot** (not reached).
- Users 0 & 1 report **signed-in** (`XamUserGetSigninState = 1`,
  `local_user_count=2`), and scripted **input is delivered to the guest**
  (`XamInputGetState DIAG: user=0 buttons 0x0010/0x1000` — 24 A/START edges). So
  it is neither a missing-profile nor a dead-input problem.

The stall is inside `splash_screen`'s `splash_panel` DTA state machine
(`ui/splash/splash.dta`, in the base ark):

```
Entered --(SELECT/Confirm)--> ActivateSaveLoad --(poll: saveload_mgr is_idle)-->
StartOvershell {overshell attempt_to_add_user last_user} --> WaitOvershell
  --> [advance REQUIRES message]  overshell_allowing_input($is_allowed==TRUE)   (splash.dta:404-417)
  --> CheckFirstInstrument --> EndOvershell --> … --> main
```

`overshell_allowing_input(TRUE)` is emitted by an `OvershellSlot` only once its
local user has *joined the session*:

```
OvershellSlot::AddUser (OvershellSlot.cpp:1717)
  -> mSessionMgr->AddLocalUser (SessionMgr::AddLocalUser -> AddLocalUserImpl, SessionMgr.cpp:105/122)
    -> mSession->AddLocalUser         (the ONLINE NetSession::AddLocalUser)
       ... asynchronous session-join request/response ...
  -> OvershellSlot::OnMsg(AddLocalUserResultMsg)  (OvershellSlot.cpp:1531)   [fires only on join completion]
     -> slot reaches allowing-input -> OvershellAllowingInputChangedMsg
        -> splash overshell_allowing_input(TRUE) -> advance
```

The join is a **request/response protocol** — the RB3-360 `NetSession` has
`OnMsg(AddUserRequestMsg)` / `OnMsg(AddUserResponseMsg)` (rb3-360 addrs
`0x823045d8` / `0x823e7380`). Headless, with XNet/XSession/Quazal networking
stubbed, the response is never delivered → `AddLocalUserResultMsg` never fires →
the slot never reaches allowing-input → `overshell_allowing_input(TRUE)` never
fires → `kSplashScreen_WaitOvershell` never advances → `splash_screen` is never
left → `main_hub` is never loaded.

Main-dir objects located for follow-up byte-proof: `session` (NetSession)
`0x43632424`, `session_mgr` `0x44328E04`, `overshell` `0x44EFCA6C`,
`splash_panel` `0x44EFA3C0`.

### (c) Why it never flips headless — confirmed by the native-port oracle

The RB3 **native port hit this identical gate** and documents it verbatim
(`rb3/native/src/rb3_netsession_native.cpp:80-97`): their `NetSession::AddLocalUser`
was a weak no-op →

> "the local-user join never completed → SessionMgr never fired
> `AddLocalUserResultMsg` → the overshell slot never reached an input-allowing
> 'joined' state → the splash `kSplashScreen_WaitOvershell` gate
> (`overshell_allowing_input` TRUE) never fired → splash never advanced to
> main_hub."

Their fix: mirror the offline single-host `IsHost()` path — add the user to the
session's local list and **synchronously fire `AddUserResultMsg(1)`**. On Xenia
the *real* online `NetSession::AddLocalUser` runs, but the join round-trip cannot
complete without a live session, so it stalls at exactly the same point. (This is
why the brief saw `rbdxcache`/`globaloptions` `XamContentCreate` succeed at
~frame 600 and then all I/O stop: those are the splash `saveload_mgr activate`
cache mounts — the last thing that happens before the WaitOvershell idle-loop.
The blue-wipe is the splash transition/abstract-wipe overlay, not a `main_hub`
load.)

### (d) DC3-safe fix recommendation

**Primary (precise, DC3-inert, mirrors the proven native fix):** a title-gated
(`0x45410914`), default-off guest-function override — same mechanism as the
existing `--rb3dx_alloc_probe` `__savegprlr_23` override / `RegisterGuestFunctionOverride`
(commit `0c0f291b4`) — installed on the **online `NetSession::AddLocalUser`** (or
one level up at `SessionMgr::AddLocalUserImpl`) that, for the offline single-local-host
case, immediately queues/`Handle`s `AddUserResultMsg(success=1)` for the added
user, byte-for-byte the native port's offline branch. This unblocks the overshell
join → splash advance → `main_hub` with zero effect on DC3 (title-gated) and no
change to the online path when a real session exists. Resolve the target address
via the Ghidra service (`http://ghidra.local:8001/mcp`) / `bin/analyze-function`
against `45410914` (the mangled symbol is stripped from the xex; use the
`AddUserRequestMsg`/`AddUserResponseMsg` `OnMsg` neighbours `0x823045d8`/`0x823e7380`
and the `session`-object vtable to pin `AddLocalUser`).

**Alternative (more general, higher-risk for DC3):** make the XNet/XSession
"create/join local session" kernel path the online `NetSession` awaits complete
synchronously in headless mode, so `AddUserResponseMsg` is delivered. This is a
broader emulator-correctness change; it must be proven inert for DC3 before use.
Prefer the title-gated shim.

Do **not** paper over it by zeroing/mapping memory (crash report §8): a benign
return perpetuates the idle-loop. The fix must actually deliver the join result.

### (e) Unresolved / not byte-proven

- The precise splash sub-state (`kSplashScreen_Entered` vs `WaitOvershell`) and
  `NetSession::mUsers.size()` (0 = press never bound / 1 = join issued, no
  response) were not byte-decoded — `splash_state` is a DTA UIProperty and the
  session objects are multiple-inheritance vbases (offset math from the stored
  `Hmx::Object*` is fiddly). Signed-in users + delivered input + the native
  oracle pinning the fix at `AddLocalUser` (not at the button/focus path) point
  firmly at the WaitOvershell/join sub-state. To byte-confirm: extend the probe
  to decode `session`@`0x43632424` `NetSession::mUsers` and `overshell`'s slot
  states, or override `AddLocalUser` and observe the advance.
- Exact RB3-360 guest address of `NetSession::AddLocalUser` is not hand-resolved
  here (recommendation cites the resolution path).
- The brief's "18,540 captured frames of main_hub load" are actually
  splash_screen frames; the frame captures were not re-classified.

**Diagnostic instrumentation** (uncommitted, per instructions): `emulator.cc`
`Rb3dxUiProbeThread` + `--rb3dx_ui_probe` cvar (title-gated, default-off,
read-only); a temp `XamInputGetState` button-transition log in
`xam_input.cc`. Both are diagnostic-only; remove after fix. Harness files
untouched.

---

## Fix implementation (2026-07-08)

**Result: NO — splash does NOT advance to main_hub.** The offline-join fix was
implemented exactly as §(d) recommended (title-gated, default-off, DC3-inert,
guest-function override mirroring the native port's `IsHost()==true`), and it
installs correctly at runtime — but direct instrumentation **refutes the Fable
root-cause for this build/boot**: the boot never reaches the NetSession
local-user join, so completing the join cannot advance the splash. The true
blocker is *upstream* of the join.

### What was hooked, and how it self-locates

`--rb3dx_offline_join` (default off, title-gated `0x45410914`) installs a
guest-function override on **`NetSession::IsHost()`** via the existing
`RegisterGuestFunctionOverride`/extern mechanism (same path as
`--rb3dx_alloc_probe`). For the offline case the override returns the host value
so `AddLocalUser` would take its host branch and fire `AddUserResultMsg(1)`
using the game's *own* code (avoiding fragile guest-message synthesis).

Pinning had to change mid-task. The rb3-xenon decomp (`band.exe`) resolves
`NetSession::AddLocalUser`=`0x823D2468` → `bl IsHost` at `0x823D2490` →
`IsHost`=`0x823CECE0` (body reads `mState`@`this+0x68`, `mQNet`@`this+0x70`;
host iff not requesting/joining and (`mQNet==0` **or** Quazal
`IsADuplicationMaster()`)). **But the running `default.xex` (retail RB3 + TU5 +
RB3DX) is a *different build* than `band.exe`** — code is relocated (e.g.
`__savegprlr_23` region and everything else differ), so the decomp address is
wrong for the live image. The override therefore **signature-scans** for
`IsHost`'s relocation-independent body (the `lwz r11,0x68(r3)` mState read +
the 7/3/4/5/6 `cmpwi` state ladder; branch words wildcarded) and installs at the
match. Runtime: found at **`0x823E1700`** (retail), signature unique in
`band.exe`, prologue-verified, **20 `bl`-callers** in the live image (so `IsHost`
is a real, non-inlined call target — same as `band.exe`).

### Why it does not advance (evidence, `--rb3dx_ui_probe` extended to dump `session`)

Across an entire ~160 s boot with scripted A/START presses that **do** reach the
guest (`XamInputGetState` shows user-0 `buttons 0x1000/0x0010`; users 0/1
`SigninState=1`):

- `session` (NetSession) **`mState == 0` (kIdle)** and **`mQNet == 0` (null)**
  for every one of ~40 samples — never `kRequestingNewUser(7)`.
- **`NetSession::IsHost()` is called 0 times** the whole boot (override hit
  counter never increments) despite 20 real `bl`-callers → **`AddLocalUser` is
  never reached**, and neither is any other NetSession `IsHost` path.
- The boot *does* reach the cache-mount milestone (`XamContentCreate` for
  `rbdxcache` + `globaloptions`) and the splash panels are fully up
  (`splash_panel`/`sv8_panel`/`meta` all `active=1 mState=1`), then idle-loops on
  `splash_screen`.

Two consequences: (1) with `mQNet==0`, the *real* `IsHost()` **already returns
true**, so even if the join were reached it would take the host-success path
*without* any fix — the online request/response stall in §(b)/§(c) is **not**
what is happening here. (2) `AddLocalUser` never runs, so the gate is **before**
the join — in the splash `Entered → (Confirm) → ActivateSaveLoad → StartOvershell
→ attempt_to_add_user → AddUser` chain. The scripted confirm reaches the guest
input state but does not advance the splash sub-state. (The §(c) inference that
the original stall sat at `WaitOvershell` was never byte-confirmed, per §(e);
this direct read of `session.mState`/`mQNet` supersedes it.)

Frame proof: `/tmp/rb3dx-join/frames/frame_5880.ppm(.png)` — full-frame green
vertical-scanline tiled-readback artifact (same class as the §3 magenta
artifact), content not legible; `--rb3dx_ui_probe` confirms `curScreen` stays
`splash_screen`. Logs under `/tmp/rb3dx-join/`.

### DC3-safety

Fully inert for DC3 (`0x373307D9`): the cvar is default-off **and** the whole
block is gated on `title_id == 0x45410914`, so no code path exists for DC3
(argued by construction; same guard style as `--rb3dx_alloc_probe`). The
signature scan only runs when the cvar is enabled under the RB3DX title.

### New frontier

The join-completion fix is correct-in-principle and ready if the boot is ever
driven to `AddLocalUser` *with a live online session* (`mQNet!=0`), but that is
not the current blocker. Next investigator should trace the **pre-join** splash
step: instrument the splash DTA `splash_state` / `OvershellSlot::AddUser` entry
and the `saveload_mgr` `is_idle` poll to learn why `Confirm` (input reaches the
guest but the splash never leaves its entry state) does not drive
`StartOvershell → AddUser`. Candidates: (a) the RB3DX splash confirm requires a
user *actively signing into a profile* (note `dialog_need_signin_screen` +
`XamShowSigninUI` in the main dir) rather than the auto `SigninState=1`; (b) a
`saveload_mgr` state-machine poll that never reaches idle headless after the
cache mounts; (c) the confirm being consumed by a different pad/user-slot than
the one the overshell watches. `IsHost` override + `session` dump instrumentation
(both title-gated/default-off) are left in place to help.

**Files changed (uncommitted):** `src/xenia/emulator.cc` only — the
`--rb3dx_offline_join` cvar + `Rb3dxIsHostOfflineExtern` + the signature-scan
install block, plus a `session mState/mQNet` dump added to the existing
`--rb3dx_ui_probe` sampler (diagnostic, default-off). Harness files
(`command_processor.cc`, `nop_input_driver.*`) and `xam_input.cc` were **not**
modified by this task (their working-tree edits pre-date it).

---

## Fable investigation #3 (2026-07-09) — splash gate ROOT-CAUSED → FIXED (`9096dd4d0`): fork regression in `XamEnumerate`'s synchronous path

> **FIXED 2026-07-09, commit `9096dd4d0`** — verdict below CONFIRMED and the fix
> landed + DC3-verified. See "Fix landed (2026-07-09)" at the bottom of this file
> for the verification matrix and the new downstream first-boot calibration gate.

**VERDICT: candidate (b) — the `saveload_mgr is_idle` poll — is CONFIRMED as the
gate, and the mechanism is a named, byte-verified FORK BUG, not a game or
content problem. Candidates (a) sign-in and (c) pad/user-slot are REFUTED.**

The splash never leaves `kSplashScreen_ActivateSaveLoad`'s `{saveload_mgr
is_idle}` poll because `SaveLoadManager`'s auto-load parks a Memcard worker
thread in an **infinite guest loop**: `MemcardXbox::FindValidUnit` (the save-
container search, **100% byte-verified vs retail** in rb3-xenon
`build/45410914/report.json`) loops `while (XEnumerate(h, &buffer, 0x134,
&dw1bc, nullptr) == 0)` — a **synchronous** enumerate — and this fork's
`xeXamEnumerate` (commit **`a224a6846`**, "progress", 2026-02-21 DC3-headless
work) converts `X_ERROR_NO_MORE_FILES` → `X_ERROR_SUCCESS` + 0 items inside the
shared `run` lambda (`src/xenia/kernel/xam/xam_enum.cc:50-58`). The conversion
was written for the **overlapped** path (comment: real-360 `XGetOverlappedResult`
semantics; "Games like Dance Central 3 only handle result codes 0 and 0x65B")
but it also rewrites the **synchronous** return, so an exhausted enumerator
returns `0` forever and the guest loop **never exits**. Stock upstream Xenia
returns `ERROR_NO_MORE_FILES` (0x12) synchronously — which this exact guest code
handles (`FindValidUnit` checks `enumRes == 0x12` on the create; the while-loop
exits on any non-zero).

### The full causal chain (each link cited)

1. **RB3DX activates saveload at splash entry, before any confirm.** The
   RB3DX ark **overrides the base splash script**
   (`patch-extract/ui/splash/gen/splash.dtb`, decoded at
   `/tmp/rb3dx-hub-investigate/dta/ui_splash_gen_splash.dtb.dta`). Its
   `dx_splash_screen_enter` handler (dx handles `:4-14`) runs `{ui goto_screen
   splash_screen}` + `{saveload_mgr activate}` on intro-movie end/skip. The
   observed splash-entry content ops (probe6 `:6097-6234`) are this activate.
2. **The auto-load's cache searches complete fine.** The two
   `XamContentCreateEnumerator user=254 flags=0` calls are
   `CacheMgrXbox::SearchAsync` (`rb3-xenon src/system/utl/CacheMgr_Xbox.cpp:67`,
   `XContentCreateEnumerator(0xFE,0,1,0,1,…)`), and each is followed by a
   successful `xeXamContentCreate` mount (`rbdxcache`, `globaloptions`) — the
   **overlapped** enumerate path works.
3. **The save-container search then hangs.** The LAST kernel op of every boot is
   `XamContentCreateEnumerator user=0 device=0 type=1 flags=4096 items=1
   added=2` (probe6 `:6234`; no-input control `:9026`) = `MemcardXbox::
   FindValidUnit` (`rb3-xenon src/system/os/Memcard_Xbox.cpp:482`, flags
   `0x1000`), called from `MemcardMgr::ThreadCall_SearchForDevice`
   (`src/system/meta/MemcardMgr_Xbox.cpp:295`) on a Memcard worker thread. The
   enumerator holds exactly 2 items — `rbdxcache` + `globaloptions`, the
   packages the game itself created (`added=2`; `xam_content.cc:111`) — neither
   matches the profile-save container name, so after 2 successful iterations
   `WriteItems` yields `NO_MORE_FILES` (`src/xenia/kernel/xenumerator.cc:63-66`;
   `current_item_` is never reset, so exhaustion is permanent) → fork converts
   to SUCCESS/0 items → **infinite loop**. This is why there is ZERO further
   I/O, no
   `XamShowDeviceSelectorUI`, no `XamShowMessageBoxUIEx`, no
   `XamUserReadProfileSettings` for the rest of the boot (all confirmed
   never-invoked at runtime), and why the earlier Thread Status Report showed
   worker threads with live deep chains while VdSwap kept presenting.
4. **SaveLoadManager therefore never reaches idle** (`IsIdle()` = `mState==
   kS_Idle && mRequestFlags==0`, rb3 Wii `src/band3/meta_band/SaveLoadManager.cpp:76`),
   so the splash `poll` gate (`splash.dtb.dta:256-262`: state==ActivateSaveLoad
   && `{saveload_mgr is_idle}` → StartOvershell) never passes →
   `{overshell attempt_to_add_user}` never runs → `OvershellSlot::AddUser` /
   `SessionMgr::AddLocalUserImpl` / `NetSession::AddLocalUser` never reached —
   exactly matching the #2 investigation's "IsHost called 0 times / 20 callers".
   `main_hub` is never loaded.

### Why (a) and (c) are refuted

- **(a) sign-in:** `XamShowSigninUI` is **never invoked at runtime** in any boot
  log (import-table entry only); the decomp shows **no signin gate anywhere** in
  the Entered→StartOvershell chain (no `dialog_need_signin_screen` in game
  code; `XShowSigninUI` unreferenced); the native-port oracle joins profileless
  users via `kState_JoinedDefault`/`kState_ChooseProfile` where signin is an
  *optional menu choice* (`slot_states.dta`), and `OvershellSlot::AddUser`
  has no signin predicate (`rb3 src/band3/meta_band/OvershellSlot.cpp:1740`).
- **(c) wrong pad/user-slot:** two-part refutation. (i) The "pad rejected"
  variant: real retail bytes of `ReadSingleXinputJoypad` (@`0x8251EB24` in
  band.exe, disassembled this session) show an unknown caps SubType — including
  the `0x01` Xenia's nop HID reports — **falls through to `kJoypadAnalog`**, and
  even a caps-read failure continues as kJoypadAnalog; only
  `dwPacketNumber==-1` yields kJoypadNone. (The rb3-xenon decomp source of this
  function is only 81.8% matched and wrongly returns kJoypadNone on those
  paths — do not trust it here.) (ii) The "input never dispatched" variant: a
  **no-input control boot** (`/tmp/rb3dx-splash-gate/noinput-boot.log`) shows
  the intro movie playing ~2.5× longer (probe samples [5]-[9] vs [5]-[6])
  than the scripted boot, where the transition follows the 6071-line START
  within ~70 log lines — i.e. the guest UI **did consume the START as a movie
  skip**, so ButtonDownMsg dispatch works end-to-end. And decisively: the
  control boot stalls **identically with zero input** and the identical
  content-op signature (`:8891-9026`) — the parked flow is input-independent,
  so no input/slot theory can be the gate.

### Supporting facts

- The `Keystroke KEYDOWN VK=0x58xx` log lines are consumed by
  `Keyboard_Xbox.cpp:50` (`XInputGetKeystroke(0xFF,…)`, the chatpad layer), not
  the joypad path; the joypad path is the poll thread reading `XamInputGetState`
  (the DIAG lines). Both deliver.
- `XamShowDeviceSelectorUI` headless already completes immediately with dummy
  device `0x00000001` (`src/xenia/kernel/xam/xam_ui.cc:509-521`), so the
  post-fix no-save path will not hang on a device picker.
- The splash's two gates are SERIAL: (G1) BUTTON_DOWN→`SELECT_MSG` flips
  Entered→ActivateSaveLoad (this works, or will — input is proven consumed);
  (G2) `is_idle` (the broken one). Fixing G2 is necessary; G1 needs no fix.

### The fix (implemented by the follow-on workflow; see PLAN)

Restrict the `NO_MORE_FILES → SUCCESS` rewrite in `xeXamEnumerate` to the
**overlapped** completion path only, restoring stock synchronous semantics
(`X_ERROR_NO_MORE_FILES` returned to the caller). DC3-safe by construction AND
by parity: DC3's consumers are the overlapped/`XGetOverlappedResult` path the
conversion was written for — preserved bit-for-bit (the deferred-completion
machinery `kernel_state.cc:748-800` is untouched and proven working by the
successful rbdxcache/globaloptions mounts) — and DC3 ships the **identical**
synchronous `FindValidUnit` loop (`dc3-decomp/src/system/os/Memcard_Xbox.cpp:496`),
so the sync restoration is what DC3's own code wants too. Independent Opus
verification pass concurred at ~90% confidence, ranking (b)≫(c)≫(a); its one
open question — whether the spinning thread `F8000100` is a worker or the game
logic thread (in which case the spin also starved input processing) — does not
change the fix, only whether G1 needs any follow-up. Full task breakdown,
verification matrix, and residual watch-items (overshell join eligibility,
saveload no-save dialogs, first-time-calibration dialog) in
**[PLAN-splash-confirm-gate.md](PLAN-splash-confirm-gate.md)**.

**Logs (this investigation):** `/tmp/rb3dx-splash-gate/noinput-boot.log`
(no-input control, 74 probe samples, same stall); prior logs
`/tmp/rb3dx-hub-investigate/probe6-boot.log`, `/tmp/rb3dx-join/boot.log`.
No emulator source was modified in this pass (evidence gathered from existing
logs, one flag-only control boot, decomp reads, and band.exe disassembly).

---

## Fix landed (2026-07-09) — investigation #3 verdict CONFIRMED, splash gate CLEARED (commit `9096dd4d0`)

**The investigation #3 root cause is FIXED and VERIFIED.** `src/xenia/kernel/xam/
xam_enum.cc` now applies the `X_ERROR_NO_MORE_FILES → SUCCESS/0` conversion
**only on the overlapped completion path** (a `run_overlapped` wrapper); the
**synchronous** path returns `WriteItems`' result verbatim, restoring stock
upstream Xenia semantics. This is the exact fix specified in
[PLAN-splash-confirm-gate.md](PLAN-splash-confirm-gate.md) §2. Regressing commit:
`a224a6846`.

**Verification (full matrix in the PLAN's "Results (2026-07-09)"):**
- **Sync contract (no input, 150 s):** the `user=0 flags=4096`
  `MemcardXbox::FindValidUnit` enumerator now fires **exactly once** and its
  handle is Removed — no spin; 16,595 VdSwap presents continue after it; sync
  exhaustion returns `NO_MORE_FILES(0x12)`. Pre-fix it returned SUCCESS/0 forever
  (infinite Memcard-worker loop → the `saveload_mgr is_idle` stall).
- **Splash advance (scripted START/A):** UI probe shows
  `splash_screen → first_time_calibration` — the `{saveload_mgr is_idle}` gate
  (candidate (b)) is cleared, as predicted.
- **DC3-SAFE:** retail DC3 boots identically at `--fault_spin_limit` 4096 and 0
  (same milestone sequence, SIGSEGV plateau unchanged at 4 = pre-existing benign
  recovered fault, byte-identical to the `b803faab1` baseline); DC3's consumers
  never exercise the restored sync path. Non-negotiable bar MET.

**Residual blocker (moved one flow downstream — NOT the enumerate fix):** the
first-boot `first_time_calibration → cal_audio_screen` interactive **A/V-latency
calibration** (`cal_audio_panel`). Headless with null audio and fixed-time
scripted `A` presses, the latency measurement cannot complete, so the screen
never advances (stalls ~74 s to timeout at UI-probe `sample[35]`); input is
proven still live on that screen (`XamInputGetState` user=0 `0x1000` during the
stall). `main_hub_screen`/`instrument_screen` appear only in the maindir
screen-registry, never as `curScreen`. An attempted opt2-skip (`DOWN`+`A` on the
`first_time_calibration` dialog, §"Fable investigation" (b)/PLAN §3.5 watch-item
5) regressed to staying on `first_time_calibration` — not trivially scriptable.
Next-pass leads (cheapest first): prime `get_has_seen_first_time_calibration`
true (skip calibration on first boot), drive the exact `cal_audio` skip-button
focus sequence, or feed a headless calibration-complete signal. Logs:
`/tmp/wf-splash-confirm-gate/`.
