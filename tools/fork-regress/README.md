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
| S0 | nothing (static) | the binary's compiled-in cvar defaults (full map); source ratchets on this tree: title-ID literals outside `src/xenia/titles/`, `/home/free` in `src/`, `XELOGI(` in `src/xenia/gpu/` | measurements taken; the comparator lists every changed default and fails a ratchet increase | 1 (1) |
| S1 | DC3 original `debug.xex`, null GPU, ymca flow, 230 s (the dc3-oracle command) | milestone times title/main/choose_mode/song_select/game_screen, first `gpState=2 paused=0`, first `gpState=3`, gpState=2 sample count, max SIGSEGV, `mFailThreadMsg`/TAINTED lines | title ≤ 30 s, game_screen ≤ 60 s, ≥ 60 gpState=2 samples, gpState=3 seen, rc 0 + `TIMEOUT` line, SIGSEGV 0 | 3 (2) |
| S1V | S1 on Vulkan, `--vulkan_device=1`, capture every 300 swaps | frame count, capture swap indices, flow milestones; keeps one PNG from game_screen (else the furthest screen) | rc 0 + TIMEOUT, title reached, ≥ 10 frames, kept frame not a uniform fill. game_screen is recorded, not required (BASELINE.md: Vulkan gameplay 0/2) | 1 (1) |
| S2 | S1 + `--dc3_dta_channel=<run>/dta.sock`; `lib/dta_driver.py` drives `dc3-decomp/tools/console/dc3_eval.py -T xenia --socket` | channel installed, first poll thread, answers + latency, `object_list main` count, plus the S1 flow on the same run | thread `00000006`; `{+ 1 2}`→`=> 3`; `{no_such_func 1}`→`=> !! refused: script error…`; `{+ 5 5}`→`=> 10`; `{size {object_list main Object FALSE}}`→`=> <int>`; S1 criteria | 2 (1) |
| S3 | DC3 decomp-layout `default.xex` (2026-08-24 build), 120 s, pinned 2026-08-29 shared toml | count of `tw/td forced trap hit!` and the per-LR histogram (a fingerprint of how far the decomp image boots and through which assert sites) | count == 627 and histogram == `reference/trap627_s66.json`; `layout=decomp`; manifest-fingerprint-mismatch line; TIMEOUT; rc 0. LR sequence equality recorded, not required | 2 (2) |
| S4 | RB3 clean retail TU5 (`clean_tu5_nodd.xex`), autopilot, mogg key table | screen timeline, tv3 screens, game_screen entry, song-stream census after game_screen | `app-run-direct installed`; main_hub, song_select, part_difficulty, tv3_* reached; with key: game_screen transState=0 and a stream at mState=3 with 11 receivers/11 channels; no `FAULT_LIVELOCK_ABORT`; rc 0. Without key: menu-only (transition to game_screen begins) | 2 (1) |
| S5 | RB3DX `default.xex`, A every 5 s | screen timeline, game_screen entry | main_hub + song_select reached, game_screen transState=0, no livelock abort, rc 0 | 2 (1) |
| S6 | three variants: (1) DC1 TU0 60 s, no title cvars; (2) S1 with every RB3 cvar on; (3) RB3DX with every DC3 cvar on | per-pattern count of the OTHER title's hook lines | zero; the comparator treats each count as a ratchet (candidate ≤ baseline), because today's binaries leak (thread-6 dump, nop_input DC3 paths) | 3 (3) |

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

See the measured table in the baseline commit message and
`load_evidence.py`. Default `FR_LOAD_MAX` is set in `lib/fr.py`
(`DEFAULT_LOAD_MAX`).

## Pinned inputs

- Content: `build_content.sh` → `$FORK_REGRESS_CONTENT` (default
  `/home/free/tmp/fork-regress-content`, not `/tmp`, never in git), with
  `MANIFEST.sha256`. Every copied input is verified against an expected
  sha256; an existing file is never overwritten with different bytes.
- Configs (in git, small text): `config/dc3-oracle.defaults.toml` (= the
  dc3-oracle all-defaults toml, for S1/S1V/S2/S6.1-2) and
  `config/shared-2026-08-29.toml` (the shared toml every 627 and RB3 §8v-8x
  result was measured under, for S3/S4/S5/S6.3). Each run gets a private copy.
- Resolver inputs passed explicitly so no host path is auto-probed:
  `symbols.txt` @ dc3-decomp `c362ede1c` (S1 family) / `3504a8a58` (S3, the
  copy in force on 2026-08-29), fingerprints @ xenia `a5fc2f1b6`, the
  2026-08-24 patch manifest.
- `reference/trap627_s66.json`: count, LR histogram and sequence hash of
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
