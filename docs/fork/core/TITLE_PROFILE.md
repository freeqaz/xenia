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

## Cvars, and whether the titles still need them

Evidence sources:

- **Fires?** Does the mitigation's own log line appear in the integrate
  baseline logs (`~/tmp/fork-regress-baselines/integrate-6bf623353`, S1-S5)?
  A mitigation that never fires cannot be what a run depends on.
- **A/B:** the harness with the cvar forced to its upstream value on the
  command line (the profile respects it).

| cvar | upstream | DC3 | RB3 | fires in baseline logs? | A/B verdict |
|---|---|---|---|---|---|
| `soft_fault_unmapped_reads` | false | true | true | DC3 S1-S3: 0. RB3 S4/S5: yes, at the log cap of 20; reads at exactly a thread's `stack_base` (0x70030000, 0x70070000, ...) | pending |
| `tolerate_null_guest_calls` | false | true | true | 0 everywhere (no `ResolveFunction(00000000)` / `no function found`) | pending |
| `scanner_stop_on_invalid_run` | false | true | true | not visible at info level (XELOGD) | pending |
| `io_force_synchronous_completion` | false | true | true | no log; rationale refuted by `ac0052e5b` | pending |
| `autoinit_critical_sections` | false | true | true | 0 everywhere | pending |
| `rtl_leave_critical_section_force_release` | false | true | true | 0 everywhere | pending |
| `fault_spin_limit` | 0 | 4096 | 4096 | no `FAULT LIVELOCK` anywhere | pending |
| `xam_enum_overlapped_nomorefiles_success` | false | true | true | no log | pending |
| `xam_user_grant_privileges` | false | true | true | no log; both titles import `XamUserCheckPrivilege` | pending |
| `nui_device_present` | false | true | – | DC3 imports `XamNuiGetDeviceStatus` (Kinect title) | kept: DC3 needs a sensor |
| `min_guest_thread_stack_size` | 0 | 4 MiB | – | – | kept until Lane B moves automation off the SkeletonUpdate worker (gap analysis G6) |

The "pending" rows are filled from the step-3 runs: `xh-tip-alloff` (DC3
S1/S2/S3, RB3 S4/S5) and `xh-tip-alloff-but-sf` (RB3 S4/S5).
