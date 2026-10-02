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
DC3 S1 was re-measured on main abed23403 (Lane B's merge, where the original
DC3 plays a whole song): with the trimmed profile, S1 PASSes 3/3, with
gpState=3 at about 198 s.

| cvar | upstream | fires (census) | without it | in profile |
|---|---|---|---|---|
| `soft_fault_unmapped_reads` | false | DC3: never. RB3 S4/S5 before Lane C: yes, but **from host code**: guest lr = r1 = 0, reads at a fresh thread's exact `stack_base` (the committed no-access guard page above every guest stack). That was the RB3 UI probe's back-chain walk, whose readability test accepted any committed page. | RB3 all-off: early crash (before the probe fix). After Lane C's fix (titles/rb3 `GuestReader` reads only pages the heap grants Read; lane-c-rb3 aad61d8d2), forced off on the command line: RB3 S5 PASS 2/2; S4 PASS 1, plus 1 run aborted by the unrelated `RtlEnterCriticalSection` `cs->owning_thread == 0` assert with no unmapped read before it; 0 `soft-fault read from unmapped` lines vs 20 on main. DC3 S3: 627 without it. | **dropped** (Lane C, ca331bab5) |
| `tolerate_null_guest_calls` | false | silent when on (the fast path skips the call) | DC3 S3: assert in the decomp image's boot. RB3 S4: null `Splash::Show` dir (r3 = 0x188). DC3 S1: PASS without it (whole song). RB3 S5: PASS without it. | **DC3** (decomp image only, K11) and **RB3** (TU5 content; see below) |
| `io_force_synchronous_completion` | false | yes, on async reads (DC3 S1, RB3 S4/S5) | DC3 S1 2/2 PASS (whole song, gpState=3 at 198 s); DC3 S3 627; RB3 S4/S5 PASS | dropped |
| `xam_enum_overlapped_nomorefiles_success` | false | RB3 S5: 2 conversions; nothing elsewhere | RB3 S4/S5 PASS; DC3 S3 627 | dropped |
| `xam_user_grant_privileges` | false | RB3 asks for privilege 252; DC3 never asks | RB3 S4/S5 PASS denied | dropped |
| `scanner_stop_on_invalid_run` | false | no truncation (S1, S4, S5) | S3 627; S4/S5 PASS | dropped |
| `autoinit_critical_sections` | false | never fires (S1-S5) | S3 627; S4/S5 PASS | **RB3 only**: RB3E's unconstructed `.bss` CS, which no scenario covers |
| `rtl_leave_critical_section_force_release` | false | never fires | as above | **RB3 only**, as above |
| `fault_spin_limit` | 0 | never fires | S3 627; S4/S5 PASS | **RB3 only**: turns RB3DX's post-OOM store loop into an exit |
| `nui_device_present` | false | – | – | DC3: a Kinect title |
| `min_guest_thread_stack_size` | 0 | – | – | DC3: until Lane B moves automation off the SkeletonUpdate worker (G6) |

The runs behind the "without it" column:

- `xh-tip-alloff` (everything off): DC3 S1 ×2 and S3; RB3 S4 and S5.
- `xh-tip-alloff-but-sf`: RB3 S4 and S5.
- `xh-tip2-keep-sf-null`: RB3 S4 and S5, both PASS.
- `xh-tip2-keep-null`: DC3 S3 = 627, with even soft_fault off.

The trimmed profile is a superset of each passing configuration.

## RB3 TU5 `Splash::Show` null dir: missing content, not an emulator gap

`this+0xB8` is `mPreparedScreens`, a std::list of `{RndDir*, durationMs}`.
`RndDir+0x188` is the RndPollable vfptr, and slot 1 is `Enter`.

The null-call diagnostic printed `call through a null pointer from guest
82742098, r3=00000188`, so the dir is **null**. A splash milo failed to load,
and retail compiles out the `MILO_FAIL("Missing file")` in `PrepareNext`. The
only file failure in that run is `update:\gen\patch_xbox.hdr`: the S4
content set has no TU5 title-update patch ark (the README excludes RB3DX's
encrypted one). That makes the fix a content-provenance task for
`tools/fork-regress/build_content.sh`, not a VFS or CPU change. Until then RB3
keeps `tolerate_null_guest_calls`, as a CONTENT gap, not an emulator gap.

Searched 2026-10-02 (Lane C): the TU5 patch ark is **not on this box**. A
filesystem-wide `find` for `patch_xbox*` turns up only RB3DX's
`/srv/torrents/games/arbys/rb3/gen/patch_xbox.hdr` + `patch_xbox_0.ark`, whose
header starts `LOLZ` (the RB3DX repack's ark, which retail TU5 rejects).
`rb3-xenon/orig/45410914/` and `milo-executable-library/rb3/360 xexp/tu5/`
hold executables and the `.xexp` only. Retiring `tolerate_null_guest_calls`
for RB3 needs the TU5 title-update package's own `gen/patch_xbox.*`.

The two candidates the diagnostic was there to tell apart:

- **`r3 = 0x188`:** the dir is null. A splash milo failed to load, which means
  missing content in the rebuilt TU5 set (`tools/fork-regress/build_content.sh`)
  or a VFS gap.
- **Any other value:** `Show()` ran on an empty list, which is an
  ordering/threading gap.

## The `RtlEnterCriticalSection` `owning_thread == 0` abort (core-d2)

The abort cited in the table above, about 1 in 7 RB3 runs historically, was
not a critical-section bug. It was handle aliasing in
`XObject::GetNativeObject`, which trusted a handle stashed in a guest
dispatcher header after the object-table slot had been reused.

RB3's `XEnumerateCrossTitle` task calls `ObDereferenceObject()` on the
`X_KENUMERATOR` that `XamGetPrivateEnumStructureFromHandle` returned. The
cross-title enumerate tasks of every RB3 S4 run then do two things: they
release four live thread handles (often the running task thread's own handle,
whose slot the stale stash happened to name), and they leak all four aggregate
enumerators.
When the reused slot belonged to a critical section's event, the CS waited
on, and set, the wrong event, or none at all.

Fixed by the two `GetNativeObject` commits ("verifies a stashed handle" and
"limit the owner lookup to the Ob* exports"): a stash is honoured only if it
names an object that wraps that guest address, and the Ob* exports find the
owning object first. Measured per RB3 S4 run, main versus fixed:

| | main | fixed |
|---|---|---|
| XThread handles removed | 7 | 3 |
| bogus event wrappers removed | 4 | 0 |
| aggregate enumerators released | 0 of 4 | 4 of 4 |

The difference is deterministic: it was identical in all 8 runs on each side.
The abort itself did not reproduce on either side in that A/B (0 of 16 runs
each), so its rate is not measured here.

Nothing in the title profile depended on this: `autoinit_critical_sections`
and `rtl_leave_critical_section_force_release` never fired before the fix
either.
