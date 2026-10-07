# fork-regress: the regression harness for the fork cleanup

Phase 0 (Lane H) of `FORK_CLEANUP_PLAN.md` §4. Every cleanup lane runs this
before and after its change, against the same pinned inputs. No file under
`src/` is touched by the harness.

```
tools/fork-regress/build_content.sh                     # once: pinned content, outside git
tools/fork-regress/run.sh <xenia-headless> <out-dir> [--scenarios S0,S1,...] [--retry K] [--wait-load SECS]
tools/fork-regress/compare.py baselines/<ref>.json <out-dir>   # exit 0 ok / 1 regression / 3 inconclusive
tools/fork-regress/load_evidence.py <out-dir>...         # outcome vs host load table
```

**Sharing the host.** Several lanes run the harness at once. Wrap each
`run.sh` call in `tools/fork-regress/slot.sh` (or pass `ab.sh --slot`): three
slots, at most one per owner (`$FR_SLOT_OWNER`, else the lane path in the
arguments, else the working directory's name), and slots 2-3 only while the
1-minute load is below 40. Hold a slot for one run, never for a whole sequence.
`/home/free/tmp/fr-slot.sh` is a symlink to this script on the dev host.

Runs need the sandbox off (`dangerouslyDisableSandbox: true`): they exec the
emulator, open unix sockets, and S1V uses GPU 1.

## Scenarios

| ID | What runs | What it measures | PASS | N (need) |
|---|---|---|---|---|
| S0 | nothing (static) | the binary's compiled-in cvar defaults (full map); source ratchets on this tree: title-ID literals outside `src/xenia/titles/`, `/home/free` in `src/`, `XELOGI(` in `src/xenia/gpu/`; the title-ID **allow-list** | measurements taken, and no title-ID line (`373307D9`/`45410914`, any case, `0x` optional, or `kTitleDc3`/`kTitleRb3`) outside `src/xenia/titles/` beyond `scenarios/S0.title-id-allowlist` (path, max lines, reason; a file below its allowance is reported as a stale entry); the comparator lists every changed default and fails a ratchet increase | 1 (1) |
| S1 | DC3 original `debug.xex`, null GPU, the shared flow `flows/dc3-ymca.txt` (native-port semantics, frame clock), 230 s (the dc3-oracle command) | milestone times title/main/choose_mode/song_select/game_screen, first `gpState=2 paused=0`, first `gpState=3`, gpState=2 sample count, max SIGSEGV, `mFailThreadMsg`/TAINTED lines | title ≤ 30 s, game_screen ≤ 60 s, ≥ 60 gpState=2 samples, gpState=3 seen, rc 0 + `TIMEOUT` line, NON_XMA 0 (SIGSEGV 0 on a binary without `DC3 FAULTS`), song (`DC3 Script: song`) == `ymca` on a binary whose adapter logs `screen ->` lines | 5 (2): the reference chan5 passes only 5 of 8 flows on a quiet host, so "2 of 3" would fail a good binary 32% of the time |
| S1V | S1 on Vulkan, `--vulkan_device=1`, capture every 300 swaps, `--headless_inline_render=true`, private pipeline cache seeded from the persistent warm cache (see "S1V pipeline cache") | frame count, capture swap indices, flow milestones, Milo fail-screen frames (first swap, screen, PNG); keeps one PNG from game_screen (else the furthest screen); per-quadrant stddev of the kept frame; `game_screen_blank_swaps` (captures failing the quadrant test, while the PPMs exist); `s1v_pcache` = seed / cold / cold-no-seed in run_meta | rc 0 + TIMEOUT, title reached, ≥ 10 frames, kept frame not a uniform fill, and **every quadrant of the kept frame has max-channel stddev ≥ 10** (a black\|blue half-and-half clear, the deferred-replay capture's usual output, has overall stddev ~74 but four flat quadrants). game_screen and the fail screen are recorded, not required | 1 (1) |
| S2 | S1 + `--dc3_dta_channel=<run>/dta.sock`; `lib/dta_driver.py` drives `dc3-decomp/tools/console/dc3_eval.py -T xenia --socket` | channel installed, first poll thread, answers + latency, `object_list main` count, plus the S1 flow on the same run | thread `00000006`; `{+ 1 2}`→`=> 3`; `{no_such_func 1}`→`=> !! refused: script error…`; `{+ 5 5}`→`=> 10`; `{size {object_list main Object FALSE}}`→`=> <int>`; `{gamedata get song}` (sent once the song plays) == `ymca` (`FR_DTA_EXPECT_SONG`); S1 criteria | 2 (1) |
| S3 | DC3 decomp-layout `default.xex` (2026-08-24 build), 120 s, pinned 2026-08-29 shared toml | count of `tw/td forced trap hit!` and the per-LR histogram (a fingerprint of how far the decomp image boots and through which assert sites) | count == 627 and histogram == `reference/trap627_s66.json`; `layout=decomp`; manifest-fingerprint-mismatch line; TIMEOUT; rc 0. LR sequence equality recorded, not required | 2 (2) |
| S4 | RB3 clean retail TU5 (`clean_tu5_nodd.xex`), autopilot, mogg key table | screen timeline, tv3 screens, game_screen entry, song-stream census after game_screen | `app-run-direct installed`; main_hub, song_select, part_difficulty, tv3_* reached; with key: game_screen transState=0 and a stream at mState=3 with 11 receivers/11 channels; no `FAULT_LIVELOCK_ABORT`; rc 0. Without key: menu-only (transition to game_screen begins) | 2 (1) |
| S5 | RB3DX `default.xex`, A every 5 s | screen timeline, game_screen entry | main_hub + song_select reached, game_screen transState=0, no livelock abort, rc 0 | 2 (1) |
| S6 | three variants: (1) DC1 TU0 60 s, no title cvars; (2) S1 with every RB3 cvar on; (3) RB3DX with every DC3 cvar on | per-pattern count of the OTHER title's hook lines | zero; the comparator ratchets the SET of leaking patterns per variant (a new leak is a regression; counts scale with run length and are only reported), because today's binaries leak (the thread-6 "present pipeline" dump) | 3 (3) |

`summary.json` also carries **passive inertness**: every S1/S1V/S2/S3 log is
scanned for RB3 hook lines and every S4/S5 log for DC3 hook lines.

## Verdicts and provenance

Each run writes `<out>/<S>/run-NN/verdict.json` (`schema: fork-regress/v1`):
`verdict` PASS / FAIL / INCONCLUSIVE / SKIPPED, `reasons`, `measurements`,
`criteria`, and `provenance`: binary path + xxh3, binary git rev (the
worktree's HEAD for a `build/bin` binary, else `--binary-rev`), harness git rev
+ dirty count, sha256 of every input (xex, flow script, symbols, fingerprint
file, manifest, mogg key, seed), the pinned config's path + sha256, full argv,
rc, wall time, host, load statistics, and the **full effective cvar list**
(the binary's own defaults dump, overlaid with the config and then argv) plus
the non-default subset with each value's source. Each scenario aggregates to
`scenario.json`; the whole run to `summary.json`.

- SKIPPED: the binary lacks a capability the scenario needs (S2 on a binary
  without `--dc3_dta_channel`; S4 without `--rb3_tu5_app_run_direct`). Probed
  from the binary's defaults dump: an unknown flag makes xenia print help and
  exit 0, which would read as a silent pass.
- INCONCLUSIVE: a flow scenario that FAILED while the 1-min load exceeded the
  gate; a process killed by an external signal; an interrupted harness; GPU 1
  busy; a run whose only failure is a **known original-game race** (reason
  `known_game_race:<name>`, see "Known original-game races"). Never counted
  as PASS or FAIL. `--retry K` re-runs it (default 1).
- A PASS under load stays a PASS (marked `loaded: true`): it is a stronger
  result, but the comparator leaves loaded runs out of timing medians.

## The DC3 flow (`flows/dc3-ymca.txt`)

The scripted-input player (`hid/nop/nop_input_driver.cc`) runs the native
port's format and semantics (`dc3-decomp native/src/platform/Joypad_Native.cpp`)
when the title supplies a guest frame clock (DC3: the main-thread hook, one tick
per `SystemPoll`, ~59 frames/s measured): `+N` is N frames after the last
satisfied `wait_screen`, `N` is absolute, a press lasts one frame,
`wait_screen` needs a settled screen (`UIManager::InTransition` false) and gives
up after 1800 frames, and `cancel`/`option`/`l1`/`r2`/... are accepted. Titles
without a clock keep the legacy timing (`+N` = N x 50 ms, 350 ms hold).

`flows/dc3-ymca.txt` is dc3-decomp `scripts/dc3-input-flows/ymca.txt`
@ `6d9785b8f` with seven timing lines moved (`docs/fork/dc3/BASELINE.md`,
"Flow"); the same file drives the native port to ymca and game_screen.
`FR_DC3_FLOW=<file>` overrides it (exploration). `FR_DTA_SCREEN_PROBE=
"<screen>|<query>|<seconds>"` makes S2's driver send one DTA query back to back
once the adapter logs that screen (answers in `driver.json`).

## Load threshold

The gate is the **mean** of the 1-min loadavg sampled every 5 s over the run,
threshold **80** (`FR_LOAD_MAX`, `FR_LOAD_METRIC=max` for the peak). The plan
guessed "max > 30". The measurements below do not support either half of that
guess.

| evidence | load (1-min) | outcome |
|---|---|---|
| BASELINE.md spike runs b1, b2, c1, c2 | ~12-20 | 4/4 reached game_screen |
| BASELINE.md x2 (chan5) | ~80-100 | reached game_screen |
| BASELINE.md b5-b7, k1-k3, c4, x1 | 100-220 | 0/6 game_screen, 4/9 boot hangs |
| chan5 S1/S2 here | mean 11, 12, 13, 23 / **72 (max 89)** | PASS ×5 |
| chan5 S1 here | mean **15** | FAIL: loading->game_screen stall, no guest fault (the nav-bridge race) |
| chan5 S1, 3 more runs | mean 37 / 24 / 28 | PASS / FAIL boot hang (thread 6 never ran: the lost-resume race fixed in integrate `6bf623353`) / FAIL stall |
| Aug-29 S1 here | mean 17, 28, 36, 36, 87 | FAIL ×5, every run with the same guest fault (a binary regression, not load) |
| Aug-29 S1, run 1 | mean 28, **max 67**, PSI 3 | FAIL like the rest: a peak-based gate at 60 called it "loaded" |
| Aug-29 S4 / S5 | mean 16-60 (max 78) / 17-71 (max 101) | PASS ×3 / ×3; the RB3 probe cadence halves under load, hence S4's 420 s timeout |
| Aug-29 S3 | mean 11-116 | 627 exact every time: S3 is not load sensitive (`flow=False`) |

So: below a mean of ~80, every failure observed here is a property of the
binary or a race that also fails on a quiet host (chan5: 5 of 8 flows at mean
11-37). Every failure
the spike attributed to load sat at 100+. 80 is the conservative edge of the
measured passes (72 here, x2 at 80-100). A short peak does not break a run; sustained
contention does. Hence the mean, not the max. PSI cpu-some is recorded
alongside (`psi_cpu_some_avg10_*`) but not gated: it tracked loadavg loosely
(PSI 3 at load 28, 34 at load 72). A loaded PASS stays a PASS (`loaded: true`). Only a loaded FAIL becomes INCONCLUSIVE.

Because one run cannot separate load from a real failure, use `ab.sh` and
`compare.py --paired`: two binaries interleaved run by run see the same host,
and a FAIL next to a PASS at comparable load is a PAIRED-FAIL.
`load_evidence.py <out-dir>...` reprints this table from any set of runs; set
`FR_LOAD_MAX` and `fr.py refinalize <out-dir>` to re-judge kept runs.

## Known original-game races

A crash that is a race in the game's own code (present in the original
`debug.xex`, not introduced by the emulator) says nothing about the binary
under test. A run whose faults match one **exactly** is INCONCLUSIVE with
reason `known_game_race:<name>` (first in `reasons`; the original FAIL
reasons follow), so `--retry` re-runs it. It is never a PASS: `scenario.json`
and `summary.json` carry `known_game_races` (every attempt, retries included,
by race; `final` = run indices still ending on one), `fr.py summary` prints a
line per race, and `compare.py` prints a `KNOWN-RACE` line with the
candidate's and the baseline's counts (not judged, exit code unchanged). A
scenario left INCONCLUSIVE by a race exits 3 like any other. The match lives
in `analyze/dc3_flow.py` (`KNOWN_RACES`) and is applied by S1, S1V and S2.
Tests: `tests/test_known_game_race.py` (recorded logs trimmed to the lines
the analyzer reads, plus one-line mutations that each must stay FAIL).

### `splash_postprocessor_uaf`: Rnd::DoWorldEnd on the splash thread

**Signature** (all required): exactly one `MMIO soft-fault read from
unmapped guest 00000008 (... guest lr 82662B24 ...)` line in the log, logged
before title_screen; every `crash_guest=` on the status reports is
`0x82662B18`; exactly one non-XMA fault (one SIGSEGV on a binary without
`DC3 FAULTS`). Anything else (a second fault, another PC/lr/address, the same
read after title_screen, XMA faults that move `crash_guest`) stays a FAIL: a
miss errs toward FAIL.

**Mechanism** (dc3-decomp `066d55163`):

- `Splash::BeginSplasher` starts a real render thread on the Xbox path
  (`src/system/movie/Splash.cpp:226-232`: `CreateThread` suspended,
  `XSetThreadProcessor(thread, 5)`, `SetThreadPriority(thread, 1)`); the main
  thread goes on booting (`src/App.cpp:634` BeginSplasher ... `:703`
  `WorldInit()`).
- Each splash frame ends the world: `Rnd::DoWorldEnd`
  (`src/system/rndobj/Rnd.cpp:737-756`) walks `TheRnd.mPostProcessors`
  (`std::list<PostProcessor *>`, `src/system/rndobj/Rnd.h:314`, at
  TheDxRnd+0x15C) **without a lock**, calling `EndWorld` on each.
- On the main thread, `WorldInit` (`src/system/world/World.cpp:37`) calls
  `NgSpotlightDrawer::Init` (`src/system/world/SpotlightDrawer_NG.cpp:89-96`),
  whose `RELEASE(sDefault)` (`:92`) destroys the default `SpotlightDrawer`:
  `~SpotlightDrawer` (`src/system/world/SpotlightDrawer.cpp:34-41`) ->
  `DeSelect` (`:420-430`) -> `Rnd::UnregisterPostProcessor`
  (`src/system/rndobj/Rnd.cpp:885`, `list::remove`), which unlinks the node
  and frees it at once; `FixedSizeAlloc::Free`
  (`src/system/utl/PoolAlloc.cpp:99-101`) writes the pool's free-list link
  into the node's first word, which is the list node's `_M_next`.
- If the splash thread is between `EndWorld` and `++it` on that node
  (`0x82662B24`, `lwz r30,0(r30)`), it follows the free-list link into pool
  memory, loads a data word that is not a `PostProcessor`, and faults on the
  vtable+8 load at `0x82662B18` (`lwz r11,8(r11)` with r11 = 0: a read of
  guest `0x00000008`; lr is still `0x82662B24`).

**Our source matches the image**: `Rnd::DoWorldEnd`,
`Rnd::RegisterPostProcessor`, `Rnd::UnregisterPostProcessor`,
`NgSpotlightDrawer::Init`, `SpotlightDrawer::~SpotlightDrawer` (and its
deleting dtor), `SpotlightDrawer::Select`, `SpotlightDrawer::DeSelect`,
`FixedSizeAlloc::Free`, `Splash::BeginSplasher` and `Splash::ThreadStart` are
all 100.0% (normalized and fuzzy, `name_check` ruler) in dc3-decomp's
`report.json`. The race is in the game.

**Evidence**:

- The production signature, identical in every instance (read of guest 8,
  crash_guest `0x82662B18`, guest lr `82662B24`, r1 `7330FA80`, splash thread,
  3-9 s, before title): flow-wake `rate` S1 run-04 (1 of 10), lane B2
  `r1-cand` S2 run-01 (1 of 43 lane-B2 S1/S2 runs, `docs/fork/dc3/BASELINE.md`
  "Other findings"), core-d2 `gdb-cand3` S1 run-01.
- Caught in the act under the GDB-RSP stub (2026-10-07, main `29dbc4380`,
  scripts in `/home/free/tmp/splash-race/`, run-06): main stopped at
  `NgSpotlightDrawer::Init` with the default drawer's node `0x4029FCE0`
  first in the list; the splash thread (guest tid 0x12) then stopped at
  `0x82662B24` with r30 = `0x4029FCE0`, r31 = the list sentinel
  `0x830A13CC`, while main was already inside `~SpotlightDrawer` past
  `DeSelect` (lr `0x82827290`). Released, the splash thread faulted with the
  production signature. The stub is all-stop (no per-thread hold), so a
  deterministic forced interleaving was not possible; this was one natural
  catch.
- Specificity (2026-10-07): the matcher over every recorded S1/S1V/S2 run
  under `/home/free/tmp` (342 runs: 251 PASS, 77 FAIL, 14 INCONCLUSIVE by the
  verdicts on disk) matches exactly the three runs above and nothing else; no
  other run carries even part of the signature (crash_guest `0x82662B18` or
  an unmapped read with lr `82662B24`).

**Why the native port is immune**: it has no splash thread.
`src/system/movie/Splash.cpp:29-34` sets `mThreaded = false` under
`HX_NATIVE`, and the `CreateThread` path (`:226`) is `#ifndef HX_NATIVE`; the
splash is drawn from `TheSplasher->Poll()` on the main thread (`src/App.cpp`
native path, Poll around `WorldInit()` at `:414`), so the walk and the free
never overlap.

**Why Xenia likely amplifies it** (not measured on hardware): on a console the
splash thread runs on hardware thread 5 at raised priority, and the window
(from loading the node to `++it`, i.e. one `EndWorld` call on the default
drawer) has to overlap a short RELEASE on the main thread.
Xenia ignores both (`ignore_thread_affinities` / `ignore_thread_priorities`
are true in the pinned config), so every guest thread is an ordinary host
thread scheduled by Linux next to the JIT, the XMA workers and whatever else
the host runs; one preemption of the splash thread inside the window is
enough. Measured rate here: 1 of 10 (`rate`) and 1 of 43 (lane B2).

## Baselines (`baselines/`)

Condensed summaries (`compare.py --make-baseline`), recorded 2026-10-01.
**A** = `aug29-checked-783a0830c92e9cbc` (pinned Aug-29 Checked binary,
frag-alloc-trace, ~f137bcedb). **C5** = `chan5-spike-3c48915b822b51d5` (the
dc3-oracle spike c59ced098 on main 90eb07f81; DC3 scenarios only; S1 recorded
at N=3). **I** = `integrate-6bf623353-33dc9e5a402fea0a` (Checked, built by
this lane from integrate-2026-10 @ 6bf623353, which carries the bisect fix
82acbded7 `--nop_audio_driver` and the lost-resume fix).

| | A | C5 | I |
|---|---|---|---|
| S0 | PASS (210 cvars; ratchets 65 / 3 / 53) | PASS | PASS |
| S1 | **FAIL 0/3**: guest fault in XMAHALWriteAndUnlockContexts+0x7C by 6 s, then FreestyleMotionFilter::IsActive faults, stall loading->game_screen | PASS 2/3: title 9 s, game_screen 33-36 s, 75 gpState=2 samples, 0 SIGSEGV | PASS 2/5: same timings when it passes; 3 loading->game_screen stalls, no guest faults, mean load 15-42 |
| S1V | FAIL: GPU-side swaps stop at ~100, 0 frames | PASS: 44 frames; Milo fail screen from swap 1200 at song_select (`preview.tmov`) | PASS: 43 frames; same fail screen at 1200 |
| S2 | SKIPPED (no channel) | PASS 2/2: `3`, refused, `10`, `object_list main` = 702 | PASS 2/2, identical answers; first poll after 9 s |
| S3 | **PASS 2/2: 627**, histogram + LR sequence identical to s66 | FAIL: 3009 traps (main lineage lacks the frag-alloc-trace manifest load) | **PASS 2/2: 627**, identical sequence |
| S4 | PASS 2/2: main_hub 36-39 s, game_screen 131-133 s, song 11/11 (tv3_a), 13/13 (tv3_c) | n/a | PASS 2/2 (+1 retry): main_hub 42 s, game_screen 160-167 s, song 14/14, 15/15 |
| S5 | PASS 2/2: main_hub 27 s, game_screen 100-109 s | n/a | PASS 2/2: main_hub 27-42 s, game_screen 91-100 s |
| S6 | FAIL: v1 (DC1) and v3 (RB3DX + DC3 cvars) leak the DC3 thread-6 dump; v2 clean | same | same |
| passive | RB3 logs carry 472-528 DC3 dump lines each; DC3 logs 0 RB3 lines | DC3 logs 0 RB3 lines | as A |

**FC** = `fork-cleanup-2026-10-d7e23d1a2621b7fe` (2026-10-02): the fork after
the cleanup, tag `fork-cleanup-2026-10`. **Compare new work against this one.**
It was recorded as an interleaved A/B against main `1f309687c`, and all 18 pairs
were PASS/PASS:

- S0 PASS;
- S1 3/3 (title 12-15 s, game_screen 36-39 s);
- S1V 40 frames;
- S2 2/2;
- S3 627 exact;
- S4 3/3;
- S5 3/3;
- S6 3/3, with **no** leaking pattern in any variant. The thread-6 dump that A, C5 and I carry is gone.

Which song the RB3 autopilot lands on varies run to run (tv3_a..e, 11-15
channels), so the song is a watched measurement, not a criterion. Three of
~16 RB3 runs (A S5 seeded, A S6 v3, I S4 under load) aborted with the Checked
assert `cs->owning_thread == 0` in RtlEnterCriticalSection
(`xboxkrnl_rtl.cc:638`), the fork's CS fast path.

## S1V pipeline cache

Why inline render: measured 2026-10-03 on main `b7569fe3b`'s Checked binary,
every game_screen capture of five S1V runs. The deferred replay (S1V before
this change) kept 2 correct frames of 59; 51 were black|blue clears, bare or
with a stray polygon, and 6 partial (dark fragments, a scene under a black
blob). `--headless_inline_render=true`
kept 47 of 47 correct (venue and dancers, intro shot, the tunnel transition,
the Kinect camera panel). `--headless_capture_only_draws=false` on the deferred
path gave 16 of 19 correct plus 3 with thin stray lines, so most of the damage
is that the deferred path drops every non-capture frame's draws and resolves.

The kept-frame quadrant criterion was checked against these runs with the new
analyzer, re-run over their kept frames. The deferred baselines FAIL: main-keepfix
2400 has quadrants `[29.2, 1.0, 29.0, 0.0]` and the second baseline's 2400 has
`[18.5, 19.5, 0.0, 0.0]`. Inline render (two runs) and capture_only_draws=false
PASS, with every quadrant at 40.9 or above. The previous analyzer PASSed all
five.

Inline render compiles pipelines on the CP thread, so with async pipelines a
cold start skips draws until they compile. S1V therefore copies a persistent
warm cache, `$FORK_REGRESS_CONTENT/s1v-pcache/xenia_vulkan_pipeline_cache.bin`,
into the run's private `--vulkan_pipeline_cache_path` before the run.
`scenarios/S1V.post.sh` (run.sh calls `<scenario>.post.sh <run-dir> <verdict>`
after finalize when one exists) copies the run's cache back only after a
**PASS**, only when it is at least as large as the seed (the cache is
load + append), through a temp file and a rename. Concurrent runs read the old
file or the new one and never write the shared copy in place.

**Cold start, measured.** Both runs used inline render, no xenia cache, and
a fresh empty NVIDIA driver cache (`__GL_SHADER_DISK_CACHE_PATH`; it filled to
5.8 MB, so the driver did use it). Neither deadlocked:

| run | async pipelines | load mean | verdict | game_screen / gpState=3 | frames | blank game_screen captures |
|---|---|---|---|---|---|---|
| cold3 | on (as S1V passes it) | 19 | PASS | 36 s / 192 s | 37 | 0 of 26 |
| cold4 | off (`--extra-arg --headless_async_pipelines=false`) | 124 | PASS (loaded) | 39 s / 195 s | 30 | 0 of 20 |

The 2026-06 doc's cold-compile deadlock does not reproduce here. Both runs
kept `--headless_skip_submission_wait=true`, whose own description names the
same frame-12 deadlock. cold3's PASS wrote a 4,780,175 B seed. cold4's
lower frame count is not attributable to the cold cache, because that run was
loaded.

So the seed needs no separate step. With no seed, S1V starts cold
(`s1v_pcache: cold-no-seed`), and its first PASS writes the seed.
`FR_S1V_COLD_CACHE=1` ignores an existing seed to measure a cold start on
purpose.
To re-seed, delete `s1v-pcache/` and run S1V once. The cache depends on the
binary's shader translation and the driver, so a mismatched file costs compile
time and is not a correctness problem.

**S1V x2 with this scenario** (main `b7569fe3b`'s Checked binary, seeded,
2026-10-03): both runs PASS.

| run | load mean | game_screen / gpState=3 | frames | blank game_screen captures | kept-frame quadrants |
|---|---|---|---|---|---|
| run-01 | 49 | 36 s / 192 s | 26 | 0 of 17 | 39.7 and above |
| run-02 | 38 | 33 s / 189 s | 34 | 0 of 23 | 40.8 and above |

An earlier pair from the same binary hit two different splits of the stdout
TIMEOUT line, which the dc3_flow fixes on this branch now handle. Re-judged,
that pair also PASSes.

## Pinned inputs

Content: `build_content.sh` → `$FORK_REGRESS_CONTENT` (default
`/home/free/tmp/fork-regress-content`, not `/tmp`, never in git), with
`MANIFEST.sha256` (sha256 for files, target + size + mtime for symlinks).
Every copied input is verified against an expected sha256; an existing file is
never overwritten with different bytes.

| dir | what | used by |
|---|---|---|
| `dc3-original/` | `debug.xex` (sha256 `2d5e4a32…`) + symlinks to the 15 data entries that sit next to it in `dc3-decomp/orig/373307D9` (`gen`, `grammar`, `nuisp*`, `nxeart`, …). The xex's directory is `game:\`; without the data the boot hangs | S1 S1V S2 S6.2 |
| `dc3-inputs/` | `xenia-ymca.txt` (`ea733eee…`), `symbols.txt` @ dc3-decomp `c362ede1c`, fingerprints @ xenia `a5fc2f1b6` | S1 family |
| `dc3-decomp-2026-08-24/` | decomp `default.xex` (`b4af75f8…`, written 2026-08-24 10:19), its patch manifest, `symbols.txt` @ `3504a8a58` (in force on 2026-08-29), fingerprints, `gen` link | S3; the manifest also for S1 (else it is auto-probed) |
| `rb3/tu5-clean-nodd/` | `clean_tu5_nodd.xex` (`6d73992c…`) + `gen/` of per-file links to the torrent's 10 main arks + `main_xbox.hdr` (no `patch_xbox*`: that is RB3DX's LOLZ-encrypted ark, which retail cannot read; per-file links also avoid the torrent's `gen/gen` self-loop) + `AvatarAwards`, `nxeart`, `charnames.zbm` | S4 |
| `rb3/rb3dx/` | RB3DX `default.xex` (`6639ce25…`) + the same links, patch ark included | S5 S6.3 |
| `rb3/mogg_key_table.hex` | 64 bytes at VA `0x82C76258` of `rb3-xenon/orig/45410914/band.exe` (the deobscured RB3DX-lineage image; `dx_vs_retail_diff.txt`). Mode 0400, refused unless its sha256 is `4321690f…` | S4 |
| `rb3/seed-post-s66-2026-08-29/` | the RB3 profile content s66 ran on (`globaloptions`, `songcache`, `rbdxcache`, `band3/save.dat`), copied into the private content root before S4. Fresh: the boot reaches splash but the A-press join never fires; globaloptions without band3: parks in the startup autosave | S4 |
| `s1v-pcache/` | `xenia_vulkan_pipeline_cache.bin`, S1V's warm Vulkan pipeline cache. NOT pinned: written by S1V itself after a PASS, excluded from `MANIFEST.sha256`; each run records the sha256 it started from (`s1v_pcache_seed_sha256`) | S1V |
| `dc1/` | DC1 TU0 `default.xex` alone (no disc data: it only has to boot far enough for title hooks to fire or not) | S6.1 |

Configs (in git, small text): `config/dc3-oracle.defaults.toml` (the
dc3-oracle all-defaults toml) for the DC3 original runs and
`config/shared-2026-08-29.toml` (the shared toml every 627 and RB3 §8v-8x
result was measured under) for S3/S4/S5/S6.3. Each run gets a private copy.
A pinned toml FREEZES every cvar it names, so a cleanup lane that flips a
compiled-in default is not seen by those runs: S0's defaults diff is what
catches it.

`reference/trap627_s66.json`: count, LR histogram and LR-sequence hash of
`dc3_nonreg_s66.log`.

## Rules the harness keeps

- Private `--storage_root` per run (deleted after analysis; `FR_KEEP_STORAGE=1`
  keeps it). Never touches `~/.local/share/Xenia`.
- Every run bounded by `timeout -k 10`; on Ctrl-C only the PIDs this harness
  launched are signalled. `ulimit -c 0`: a Checked assert leaves no core.
- Sequential runs. Parallel runs load the host and the flows are load sensitive.
- S1V refuses GPU 1 while another process computes on it
  (`FR_GPU_SHARE_OK=1` overrides).

## Harness contracts (log lines the analyzers read)

A cleanup lane that changes one of these must update the analyzer in the same
commit: `Thread Status Report (<ms>ms)… SIGSEGV=<n>`, `TIMEOUT: <ms>ms reached`,
`DC3 Script: wait_screen '<x>' SATISFIED`, `DC3 Script: screen -> '<x>'`,
`DC3 Script: song '<sym>'`, `gpState=… paused=…`,
`tw/td forced trap hit! … LR=<hex>`, `DC3: NUI patch layout=<x>`,
`Disabling patch manifest target resolution due fingerprint mismatch`,
`DC3 DTA channel: installed on`, `DTA channel: first poll on guest thread <tid>`,
`RB3DX UI PROBE[n]: transState=… curScreen=…'<x>' transScreen=…'<x>'`,
`STREAM-CENSUS 0x… mState=… (n=…) … (n=…)`, `RB3: app-run-direct installed`,
`RB3: mogg-key-table installed`, `FAULT_LIVELOCK_ABORT`, `VdSwap #<n>: … [CAPTURE]`,
`MMIO soft-fault read from unmapped guest <addr> (… guest lr <lr> …)`
(cpu/mmio_handler.cc) and `crash_guest=0x<pc>` on the status report (both
read only to match a known original-game race).
