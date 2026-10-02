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

Runs need the sandbox off (`dangerouslyDisableSandbox: true`): they exec the
emulator, open unix sockets, and S1V uses GPU 1.

## Scenarios

| ID | What runs | What it measures | PASS | N (need) |
|---|---|---|---|---|
| S0 | nothing (static) | the binary's compiled-in cvar defaults (full map); source ratchets on this tree: title-ID literals outside `src/xenia/titles/`, `/home/free` in `src/`, `XELOGI(` in `src/xenia/gpu/`; the title-ID **allow-list** | measurements taken, and no title-ID line (`373307D9`/`45410914`, any case, `0x` optional, or `kTitleDc3`/`kTitleRb3`) outside `src/xenia/titles/` beyond `scenarios/S0.title-id-allowlist` (path, max lines, reason; a file below its allowance is reported as a stale entry); the comparator lists every changed default and fails a ratchet increase | 1 (1) |
| S1 | DC3 original `debug.xex`, null GPU, ymca flow, 230 s (the dc3-oracle command) | milestone times title/main/choose_mode/song_select/game_screen, first `gpState=2 paused=0`, first `gpState=3`, gpState=2 sample count, max SIGSEGV, `mFailThreadMsg`/TAINTED lines | title ≤ 30 s, game_screen ≤ 60 s, ≥ 60 gpState=2 samples, gpState=3 seen, rc 0 + `TIMEOUT` line, SIGSEGV 0 | 5 (2): the reference chan5 passes only 5 of 8 flows on a quiet host, so "2 of 3" would fail a good binary 32% of the time |
| S1V | S1 on Vulkan, `--vulkan_device=1`, capture every 300 swaps, private pipeline cache | frame count, capture swap indices, flow milestones, Milo fail-screen frames (first swap, screen, PNG); keeps one PNG from game_screen (else the furthest screen) | rc 0 + TIMEOUT, title reached, ≥ 10 frames, kept frame not a uniform fill. game_screen and the fail screen are recorded, not required: on chan5 and both BASELINE.md runs the Vulkan run hits a MILO_FAIL at song_select (`Could not find preview.tmov in dir song_info`) from swap 1200 | 1 (1) |
| S2 | S1 + `--dc3_dta_channel=<run>/dta.sock`; `lib/dta_driver.py` drives `dc3-decomp/tools/console/dc3_eval.py -T xenia --socket` | channel installed, first poll thread, answers + latency, `object_list main` count, plus the S1 flow on the same run | thread `00000006`; `{+ 1 2}`→`=> 3`; `{no_such_func 1}`→`=> !! refused: script error…`; `{+ 5 5}`→`=> 10`; `{size {object_list main Object FALSE}}`→`=> <int>`; S1 criteria | 2 (1) |
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
  busy. Never counted as PASS or FAIL. `--retry K` re-runs it (default 1).
- A PASS under load stays a PASS (marked `loaded: true`): it is a stronger
  result, but the comparator leaves loaded runs out of timing medians.

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
`DC3 Script: wait_screen '<x>' SATISFIED`, `gpState=… paused=…`,
`tw/td forced trap hit! … LR=<hex>`, `DC3: NUI patch layout=<x>`,
`Disabling patch manifest target resolution due fingerprint mismatch`,
`DC3 DTA channel: installed on`, `DTA channel: first poll on guest thread <tid>`,
`RB3DX UI PROBE[n]: transState=… curScreen=…'<x>' transScreen=…'<x>'`,
`STREAM-CENSUS 0x… mState=… (n=…) … (n=…)`, `RB3: app-run-direct installed`,
`RB3: mogg-key-table installed`, `FAULT_LIVELOCK_ABORT`, `VdSwap #<n>: … [CAPTURE]`.
