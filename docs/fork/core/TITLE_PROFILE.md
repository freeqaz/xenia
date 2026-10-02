# Per-title cvar profile

`src/xenia/titles/title_profile.cc` sets the fork's title mitigations at
launch:

- **When:** in `titles::ApplyLaunchHooks`, before `LaunchModule`, so before any
  guest code is compiled or run.
- **Which titles:** Dance Central 3 (`373307D9`) and Rock Band 3 (`45410914`,
  which also covers RB3DX/RB3E).
- **Logging:** one `TITLE-PROFILE <TID>: <cvar>=<value>` line per cvar.
- **Precedence:** a value given on the command line or in the title's own
  `config/<TID>.config.toml` wins; the line then says `kept`. Profile values sit
  in the game-config layer, so they are never written back to the global
  `xenia.config.toml`.

Every other title runs with the upstream defaults (FORK_CLEANUP_PLAN.md
§3.4). An existing config file that stored an old fork default is upgraded on
load: each flip carries an `UPDATE_from_*` entry dated 2026-10-02.

Every mitigation now logs when it changes a result. So one profile-on run
answers "does this title depend on it?" (the **fires** column), and an A/B
with the cvar forced off on the command line answers "does it still work
without it?" (the **without** column).

## Evidence (2026-10-02, lane-d-core)

Scenarios are tools/fork-regress S1 (DC3 original), S3 (DC3 decomp layout),
S4 (RB3 clean TU5) and S5 (RB3DX). "census" means the tip with the profile on.
The S1 flow currently stalls at song_select on any build carrying the
override-API fix (see below), so DC3 S1 evidence covers boot up to
song_select only.

| cvar | upstream | fires (census) | without it | verdict |
|---|---|---|---|---|
| `soft_fault_unmapped_reads` | false | DC3: never. RB3 S4/S5: yes, but **from host code, not the guest**: guest lr = r1 = 0, reads at a not-yet-started thread's exact `stack_base`. That is `Rb3dxUiProbeThread`'s back-chain walk (titles/rb3, ~L1249) reading `r32(r1)` before the thread first runs. | RB3 all-off: crash early (S4/S5, rc 139) | **RB3: needed only by the fork's own UI probe** (Lane C: bound the probe's reads). DC3: not needed. |
| `tolerate_null_guest_calls` | false | silent when on (the CallIndirect fast path skips the call) | DC3 S3: assert at a null call during the decomp-image boot. RB3 S4: null `bctrl` at 0x82742098 = `Splash::Show()`, `params.dir->Enter()` (rb3-xenon). RB3 S5: PASS without it. DC3 S1 (to song_select): no null call. | **Needed.** DC3: only by the decomp-layout image (a defect of the rebuilt image, K11). RB3 TU5: a null splash `RndDir`, open (missing content or ordering; see below). |
| `io_force_synchronous_completion` | false | yes, every title, on async reads (DC3 S1, RB3 S4, S5) | RB3 S5: PASS. DC3 S1: reached song_select (inconclusive past it). | Rationale refuted (`ac0052e5b`). Keep until a stable DC3 S1 A/B exists. |
| `xam_enum_overlapped_nomorefiles_success` | false | RB3 S5: 2 conversions. DC3 S1, RB3 S4: none. | RB3 S5: PASS | Not needed by RB3DX; DC3 never reaches it. |
| `xam_user_grant_privileges` | false | RB3 asks for privilege 252 (S4 users 0 and 1, S5 user 0). DC3 S1: never asks. | RB3 S5: PASS (denied) | DC3: not needed. RB3: S5 fine; S4 pending. |
| `scanner_stop_on_invalid_run` | false | no truncation logged (S1, S4, S5) | RB3 S5: PASS | not needed by S1/S4/S5; S3 pending |
| `autoinit_critical_sections` | false | never fires (S1-S5) | RB3 S5: PASS | not needed by the harness titles. RB3E's `.bss` CS (the original reason) is not covered by any scenario. |
| `rtl_leave_critical_section_force_release` | false | never fires (S1-S5) | RB3 S5: PASS | as above |
| `fault_spin_limit` | 0 | never fires | RB3 S5: PASS | not needed. It only matters on RB3DX's OOM path, where it turns a spin into an exit. |
| `nui_device_present` | false | – | – | DC3 is a Kinect title: kept |
| `min_guest_thread_stack_size` | 0 | – | – | DC3: kept until Lane B moves automation off the SkeletonUpdate worker (G6) |

## Open item: RB3 TU5 `Splash::Show` null dir

`this+0xB8` is `mPreparedScreens`, a std::list of `{RndDir*, durationMs}`.
`RndDir+0x188` is the RndPollable vfptr, and slot 1 is `Enter`. The r3-logging
null-call diagnostic (`ResolveFunction: call through a null pointer from guest
82742098, r3=...`) distinguishes the two candidates:

- **`r3 = 0x188`:** the dir is null. A splash milo failed to load, which means
  missing content in the rebuilt TU5 set (`tools/fork-regress/build_content.sh`)
  or a VFS gap.
- **Any other value:** `Show()` ran on an empty list, which is an
  ordering/threading gap.
