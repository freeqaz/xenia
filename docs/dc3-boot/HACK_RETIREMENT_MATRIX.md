# DC3 Hack Retirement Matrix

Purpose: track every active DC3-specific Xenia workaround as explicit, owned, retireable debt.

Rules:
- Every active workaround in code should appear here.
- Every entry must have a concrete retirement trigger and validation steps.
- “Temporary” without a trigger is not acceptable.

## Status Legend

- `required`: needed for current bring-up / workflow
- `experimental`: under evaluation / may change shape soon
- `candidate_for_removal`: likely removable pending validation
- `retired`: removed (keep history note only)

## Active Workarounds

| hack_id | category | location | mechanism | applies_to | current_status | owner_repo | reason | retirement_trigger | validation_steps | notes |
|---|---|---|---|---|---|---|---|---|---|---|
| `dc3_nui_xbc_resolver_overrides` | NUI | `src/xenia/emulator.cc` + `src/xenia/dc3_nui_patch_resolver.cc` + CPU/x64 backend | `guest_override` + semantic resolver | both | required | xenia | Stable NUI/XBC bring-up across relinks without raw-address refresh | Keep (this is the baseline); future refactor may move orchestration into a title hack pack | `tools/dc3_nui_cutover_gate.sh`; `tools/dc3_runtime_parity_gate.sh`; `xenia-core-tests "[dc3_nui_patch_resolver]"` | Legacy byte-patch fallback removed |
| `dc3_crt_constructor_sanitizer` | CRT | `src/xenia/emulator.cc` | data table patch/nullification | decomp | required | multi | Decomp build has unresolved/misresolved CRT constructor entries causing corruption or invalid execution | `dc3-decomp`/`jeff` restore missing `??__E` constructors and valid CRT tables (no bad entries/nullified forced entries needed) | Compare boot progression with sanitizer disabled; inspect CRT table stats; parity gate + runtime smoke | Includes NUI constructor skip list and bisect controls |
| `dc3_rodata_writable_for_force_unresolved` | DecompRuntimeStopgap | `src/xenia/emulator.cc` | `memory_protect` | decomp | required | multi | `/FORCE` unresolved globals resolve to image-base/RODATA and game writes fault without writable mapping | Decomp/link fixes eliminate unresolved BSS globals resolving into image image-base pages | Run decomp boot with workaround disabled; confirm no writes into protected RODATA and no SIGSEGV | Current scope intentionally limited to RODATA range |
| `dc3_zero_page_mapping` | DecompRuntimeStopgap | `src/xenia/emulator.cc` | memory mapping | decomp | required | multi | Null object/vtable reads currently fault or spin due uninitialized globals from missing constructors | Restore missing constructors/global init correctness so null-deref patterns no longer gate bring-up | Run decomp boot with zero-page mapping disabled; confirm no early null-vtable/null-object blocker | Current mode is zero-filled page (not PPC stub page) |
| `dc3_debug_fail_holmes_string_stubs` | Debug / RuntimeStopgap | `src/xenia/emulator.cc` | byte patch (stub-return) | decomp | experimental | multi | Decomp gets stuck in assertion callback loops / Holmes blocking paths / unbounded string append corruption | Replace with decomp correctness fixes (constructors/globals/string state) or promote to cleaner semantic hook if still needed | Runtime parity + milestone progression; targeted smoke with individual stubs removed | Should be split into separate tracked entries after hack-pack extraction |
| `dc3_unresolved_import_stopgaps` | Imports | `src/xenia/emulator.cc` | import thunk cleanup / marker patching | decomp | required | multi | Decomp unresolved imports / markers need stabilization to proceed through boot | Decomp/link pipeline resolves or cleanly stubs imports upstream so runtime cleanup is no longer needed | Boot smoke + import cleanup stats + parity gate | Includes PE thunk/XEX marker cleanup paths |
| `dc3_noop_stub_unresolved_call_fallback` | CPU Runtime | `src/xenia/cpu/backend/x64/x64_emitter.cc` | runtime no-op stub fallback | both (more visible on decomp) | required | xenia | Prevent crashes on null/out-of-range/unresolvable indirect calls during bring-up | Candidate for narrowing (not full removal) once runtime parity and callsite analysis show safe stricter behavior | Telemetry unresolved-call counters + hot-loop PCs; real-title smokes | This is broader than DC3 but currently documented here due DC3 bring-up use |

## Next Matrix Work (Planned)

1. Split `dc3_debug_fail_holmes_string_stubs` into individual entries after non-NUI hack extraction.
2. Add skeleton-specific workaround entries once moved out of `emulator.cc`.
3. Add `validation owner`/`last validated date` columns if the matrix starts drifting.
