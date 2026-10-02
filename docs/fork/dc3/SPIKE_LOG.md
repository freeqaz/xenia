# dc3-oracle spike log

Running log for steps 1 and 2 of `dc3-decomp/docs/plans/XENIA_ORACLE.md`.
Newest entries at the bottom. Every "works" quotes output; every "doesn't
work" quotes the observed failure.

Run artefacts (logs, cmd lines, binaries) live under
`/home/free/tmp/dc3-oracle-run/<run>/` and are not committed. Each run dir has
`cmd.txt` (binary path + xxh3, xex sha256, exact argv, rc, wall time) and
`run.log`.

## Setup (2026-09-30)

- Worktree `/home/free/tmp/xenia-dc3-oracle`, branch `dc3-oracle`, created at
  xenia `main` `90eb07f81` with `scripts/setup_worktree.sh` (the branch was
  created first with `git branch dc3-oracle main`, so the script checked out
  the existing branch instead of making `wt-<name>`).
- **Base = `main`, as briefed.** `main` is an ancestor of `frag-alloc-trace`
  (127 commits behind it, 0 ahead). The DC3 commits that exist only on
  `frag-alloc-trace` were read before choosing: `1d92efa5b`/`a5fc2f1b6`
  (manifest load + fingerprint, decomp layout), `7bf66d32a` (XCU protect,
  decomp layout), `481cde322` (mmio: gates the read soft-fault behind a cvar
  that DEFAULTS ON = current behaviour, moves the DC3 writable range behind a
  registration that emulator.cc installs with the same values), `bd3b5f76c`
  (content wipe behind a cvar defaulting ON). None changes original-layout
  behaviour, so there was no reason to leave `main`.
- **The reflinked object cache was NOT trusted.** `setup_worktree.sh` reflinks
  the main checkout's `build/` and stamps every object newer than the sources.
  That build was made from `frag-alloc-trace` (+WIP), 160 files away from
  `main`, so stamped objects would have encoded the wrong sources. Every file
  under `src/` was `touch`ed and rebuilt (`make -C build xenia-headless
  config=checked_linux -j24`, 2 min). Third-party objects were kept (same
  pinned submodule sources).
- Baseline binary xxh3 `45bf3c063e801035` (clean `main` sources). Snapshot:
  `/home/free/tmp/dc3-oracle-run/bin/xenia-headless.baseline-45bf3c063e801035`.

## Config isolation

- `xenia_headless_main.cc:139-155`: the storage root is `--storage_root`, else
  `~/.local/share/Xenia`. `config::SetupConfig` (`config.cc:211-238`) then
  reads `<storage_root>/xenia.config.toml` **and re-saves it** — so any run
  without `--storage_root` rewrites the shared toml. With `--config=<file>`
  that exists it reads that file only and does not save.
- Every run here passes `--storage_root=<run>/storage` and
  `--config=docs/fork/dc3/xenia.dc3-oracle.defaults.toml`, which is the
  file this binary writes into an empty storage root (all compiled-in
  defaults). Command-line values beat config values (`cvar.h:186`).
- The shared toml vs this binary's defaults (keys that exist on `main`):
  `stub_nui_functions` (shared `true`, default `false`),
  `dc3_nui_enable_signature_resolver` (shared `false`, default `true`),
  `headless_thread_diagnostics` (shared `true`), `rb3dx_force_zero_commit`
  (shared `true`, title-gated to RB3DX, inert for DC3), `gpu`,
  `headless_timeout_ms`. 21 other keys in the shared toml do not exist on
  `main` at all (they are `frag-alloc-trace` cvars). The run script passes
  `--stub_nui_functions=true` explicitly and leaves the resolver at its
  default.
- Not isolated, recorded instead: the NUI resolver auto-loads
  `/home/free/code/milohax/xenia/docs/dc3-boot/dc3_nui_fingerprints.txt` (the
  MAIN checkout, hardcoded) and `dc3-decomp/config/373307D9/symbols.txt`
  (log: `Loaded fingerprint cache ...`, `Loaded NUI symbol manifest ...
  (77613 .text symbols)`, `symbol_hits=56 signature_hits=3`). Both are read
  only.

## Step 1: baseline

See `BASELINE.md`. In short: null GPU reaches `game_screen` and plays at low
host load (b1, b2, c1, c2: 4/4). Vulkan gameplay failed 2/2 (b3, b4). Under
load avg ~100-220 from other sessions, no run reached `game_screen`.
b5, b7, k1 and k2 failed in flight, and 4 of 9 boots hung (c4, k3, x1, b6).
b5, b6 and b7 had no channel.

## Step 2a: file transport (RndConsole + `{run ...}`). Measured, not built

The trigger is keyboard-only. `RndConsole::OnMsg(KeyboardKeyMsg)` returns
early unless `mShowing`, and the console is shown by `KB_ESCAPE` →
`{rnd show_console}` (`system/run/config/default.dta`). Keys come from
`KeyboardPoll` → `XInputGetKeystroke(0xFF, XINPUT_FLAG_KEYBOARD, ...)`
(`Keyboard_Xbox.cpp:50`). Xenia rejects that flag before any input driver
sees it (`xam_input.cc:138`). Logged in this branch:

```
i> F8000028 XamInputGetKeystrokeEx_entry: non-gamepad keystroke query flags=40000002 -> DEVICE_NOT_CONNECTED (call #1, guest thread 00000006)
```

Thread 6 is the main thread (see 2b). So route (a) would need four Xenia
changes, not zero:

1. A keyboard keystroke source in xam/hid.
2. A writable output location. `game:` is mounted read-only
   (`emulator.cc:3027 HostPathDevice(..., true)`), and `build_probe()` writes
   relative paths, which land on `game:`.
3. Error recovery. RndConsole's MILO_TRY becomes an uncatchable throw under
   Xenia (BASELINE.md, error semantics), so one bad probe SIGTRAPs the
   emulator.
4. A batch-of-one-file reply format instead of the `=> ` contract.

Route (b) needs only a hook and the throw trap, so (a) was not built.

## Step 2b: main-thread request queue: WORKS

`src/xenia/dc3_dta_channel.{h,cc}`, `--dc3_dta_channel=<unix socket>`
(default off: no override is installed). It overrides
`HolmesClientPollKeyboard` 0x825F0F78, which is called once per frame by
`KeyboardPoll` ← `SystemPoll` ← `App::RunWithoutDebugging`.

Evidence that this is the main thread and that the stock body is inert:

```
i> F8000028 DC3 DTA channel: first poll on guest thread 00000006 (''); guest MainThread()=1 gHolmesStream=00000000
```

(`MainThread()` is the game's own check, called through `processor->Execute`.
`gHolmesStream == 0` means `HolmesClientPollKeyboard` had nothing to do.)

Before installing, the channel checks the first 4 instruction words of every
function it calls against the target listing, and refuses on any mismatch.

Raw socket round trips (`xchan.py`), run c2/c5/c6:

```
--- {+ 1 2}
status=200 (0.006s)
=> 3
--- {+ 1 2}{symbol "hello world"}{sprint "a" 1}{* 1.5 2}{array 3}
=> 3
=> hello world
=> "a1"
=> 3.000000
=> (0 0 0)
--- {size {object_list main Object FALSE}}
=> 702
--- {do ($s "") ($n 0) {main iterate_self Object $o {if {< $n 5} {strcat $s {$o name} ":" {$o class_name} ";"}} {set $n {+ $n 1}}} $s}
=> "pause_panel:HamPanel;background_panel:UIPanel;infinite_party_mode_timeout_panel:HamPanel;playlist_restart_loading_screen:HamScreen;crew_throwdown_multiuser_screen:HamScreen;"
--- {{ui current_screen} name}
=> "wait_main_after_saveload_screen"
```

### Failure handling: two Xenia facts that had to be worked around

1. **No guest C++ EH.** `RtlRaiseException` → `HandleCppException` →
   `Break()`. The channel raises `Debug::mTry`, so a MILO_FAIL inside a probe
   becomes a guest throw (`Debug::Fail`: `if (mTry) { mTry--; throw msg; }`,
   confirmed in the target listing at 0x825CE2E0-0x825CE304). A new hook in
   `RtlRaiseException` (`g_cpp_throw_hook`) longjmps back to the channel's
   frame on the same thread. The channel then restores the PPC context and the
   globals native's `ScriptStateGuard` restores (`gCallStackPtr`,
   `gPreExecuteFunc/Level`, `gDataThis`, `gDataDir`, `gVarStackPtr`, `gFile`),
   plus `MemHeapStack` (Fail pushes "main" and never pops it before the
   throw), `Debug::mTry` and `Debug::mFailing`. When nothing is armed on the
   thread, the hook returns and stock behaviour is unchanged.
2. **`mFailing` is stuck at 1 from boot** (BASELINE.md). The first build
   therefore could not trap anything, and failures fell through:

   ```
   i> F8000028 DC3 DTA channel: poll #1 TheDebug mNoDebug=0 mFailing=1 mTry=0 mFailThreadMsg=40025B50 'BinkMovieImpl::Ready called in the wrong thread (expected 6, cur thread is 15)'
   --- {5}
   => 0
   ```

   Now the channel clears it only for the duration of each guarded call, and
   puts the stuck value back afterwards.

After both fixes (run c5):

```
--- {5}
=> !! refused: script error: 5 not function or object (file <unnamed>, line 1)
--- {no_such_func 1}
=> !! refused: script error: no_such_func not function or object (file <unnamed>, line 1)
--- {+ 1 2
!! parse error
--- {+ 1 2}{no_such_func}{+ 3 4}
=> 3
=> !! refused: script error: no_such_func not function or object (file <unnamed>, line 1)
=> 7
--- {+ 5 5}
=> 10
w> ... guest failure trapped: 5 not function or object (file <unnamed>, line 1) [repaired: gCallStackPtr, Debug::mFailing, MemHeapStack]
```

In c6 the stuck `mFailing=1` was confirmed restored after a trapped failure
(`poll #4 ... mFailing=1 ... mFailThreadMsg=40025B50`, with no "eval CHANGED
Debug state" warning). In c5 it read 0 by poll #4 and was then consumed; the
cause was not pinned. The instrumentation (a warning when an eval changes
`mFailing`/`mFailThreadMsg`) was added after c5 and has not fired since.

### Were the later crashes caused by the channel? Controls say no

c3, c5 and c6 (all with failing evals) later faulted in game code
(`SongSort::BuildItemList`, `FreestyleMotionFilter::IsActive`,
`LoadingPanel::Poll`). The controls ran at the same high load: k1 (channel
on, **no evals**) faulted in `ObjectDir::FindObject`, and b5 (**no channel**)
faulted in the same `FreestyleMotionFilter::IsActive` as c5. So the crashes
are the baseline's load-sensitive races, not the channel. The c3 site was
then reproduced with no channel at all: baseline b7 faulted in the same
`SongSort::BuildItemList` (0x829894B0), at load avg ~219. Every crash site
seen in a channel run has since appeared in a run without the channel.

### ConsoleTarget round trip (run x2, the dc3-decomp `xenia-oracle-spike` branch)

```
describe: {'target': 'xenia-orig', 'transport': 'xenia-dta-channel', 'socket': '/home/free/tmp/dc3-oracle-run/x2.sock'}
eval_dta {+ 1 2} -> EvalResult(ok=True, type='symbol', value='3', error=None)
eval_batch {size {object_list main Object FALSE}} -> EvalResult(ok=True, type='symbol', value='702', error=None)
eval_batch {do ($a {object_list main Object FALSE}) {elem $a 0}} -> EvalResult(ok=True, type='symbol', value='"[default cam]"', error=None)
eval_batch {{ui current_screen} name} -> EvalResult(ok=True, type='symbol', value='"autosave_warning_screen"', error=None)
roster main/Cam -> [('[default cam]', 'Cam'), ('[stream renderer cam]', 'Cam'), ('[tex proc cam]', 'Cam'), ('[ui.cam]', 'Cam'), ('[world cam copy]', 'Cam')]
```

(That output predates the ConsoleTarget quote/refusal fixes; refusals now
come back as `ok=False`, and quoted strings come back unquoted with
`type='string'`.)

### State Diff probe, Xenia vs native

`python3 -m state_diff.capture --target xenia:<sock> --probe screen_state`
(Xenia was on `game_screen`) against native `dc3-native` headless on :9137
(`main_screen`):

```
probe: screen_state
  A = xenia-orig  (0 objects, 10 scalars, screen=?)
  B = native  (0 objects, 10 scalars, screen=main_screen)
4 finding(s):  HIGH=4
  1. [HIGH    ] other          ui.bottom_screen: game_screen -> main_screen
  2. [HIGH    ] other          ui.current_screen: game_screen -> main_screen
  3. [HIGH    ] other          ui.focus_panel: game_panel -> main_panel
  4. [HIGH    ] other          ui.game_active: 1 -> 0
```

The `rnd.*` scalars agree. The UI scalars differ only because the two sides
were on different screens, which is expected and is exactly what the probe
exists to flag.

A DTA semantics batch run on both sides agrees on every value. The only
differences are printer formatting: Xenia's `=> ` prints floats as `%f` and
strings quoted, while native's JSON does not.

```
xenia : 3 | 3.000000 | "a12.50" | x7 | 5 | "[ui.cam]" | e | "cde" | 9 | 2
native: 3 | 3        | a12.50   | x7 | 5 | [ui.cam]   | e | cde   | 9 | 2
```

Latency: the first request waits for the next main-thread frame (~0.4-0.5 s
during boot). After that, round trips take 1-25 ms, and evaluation itself
takes 50 µs-18 ms on the guest.

## Remaining blockers and recommendation for step 3

Blockers, measured:

1. **The host flow is not reproducible, and under load it is not survivable.**
   The nav bridge calls `GotoScreen`/`FindObject` from the skeleton worker,
   which races the main thread (k1 faulted in `ObjectDir::FindObject`). The
   wall-clock script makes the screens it reaches depend on frame rate (both
   Vulkan runs failed). There are also boot hangs before `main()` at 4/9 under
   heavy load. Step 3 must not depend on reaching any particular screen.
2. **Error semantics.** Outside the channel, every MILO_FAIL is a silent
   fall-through for the whole run (`mFailing` is stuck). A golden recorded
   from game state that was built while asserts were being skipped is
   suspect. A loader golden is safer when the probe itself does the load
   inside the channel, because then its failures are trapped and reported.
3. **The longjmp recovery skips guest destructors** between `Debug::Fail` and
   the channel: DataNode temporaries, `AutoTimer` (`AutoGlitchReport::sDepth`
   leaks +1 per failure), and the abandoned `Timer::Start`. The restored set
   matches native's `ScriptStateGuard`. A failure inside an object `Load()`
   would also leave a half-built object, so a failing probe should be
   followed by a fresh boot before its neighbours are trusted.
4. **Printer conventions differ** between Xenia (`=> `: `%f`, quoted strings)
   and native (JSON). State Diff probes return strings, so this only bites
   ad-hoc evals. Normalise in the comparison, not in either engine.

Recommendation for step 3 (first loader golden):

- **Boot with a flow file that stops at `title_screen`, and have the probe do
  the load.** Load the character into a new ObjectDir with the game's own
  loader (`{new ObjectDir}` + the DirLoader path / `load_objects`, whatever
  native's HTTP server uses), then dump `(class, name, type props)` with the
  existing roster + `transforms`/`hierarchy` probes, scoped to that dir. That
  scope is the one place the nav bridge, the beat drives and LoadSong repair
  cannot reach.
- **Run on a quiet host** (load < ~30). Take the golden twice from
  independent boots, keep only the fields that agree, and record the xenia
  xxh3, the xex sha256, `cmd.txt` and this manifest with it.
- **Do the first comparison on 2-3 small character .milos**, and treat the
  first disagreements as instrument findings until each one is checked
  against the target listing.
- Before trusting state that the game built outside the channel, consider
  gating the `Debug::Fail` spin patch so it no longer leaves `mFailing` set.
  For example, restore `mFailing=0` on the worker path instead of returning
  with it set. That is a fork change with gameplay-wide effects, so it needs
  its own baseline.
