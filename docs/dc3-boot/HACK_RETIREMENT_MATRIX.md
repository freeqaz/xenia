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
| `dc3_xapi_thread_notify_stub` | Imports | `src/xenia/dc3_hack_pack.cc` | byte patch (stub-return) | decomp | required | multi | Decomp CRT thread notify list may be corrupt and spins/hangs during startup | Correct decomp CRT/list initialization or upstream safe handling removes need to stub notify routines | Decomp boot smoke with stub disabled; ensure no thread-notify spin | Applies in hack-pack import/stopgap sequence |
| `dc3_crt_constructor_sanitizer` | CRT | `src/xenia/dc3_hack_pack.cc` | data table patch/nullification | decomp | required | multi | Decomp build has unresolved/misresolved CRT constructor entries causing corruption or invalid execution | `dc3-decomp`/`jeff` restore missing `??__E` constructors and valid CRT tables (no bad entries/nullified forced entries needed) | Compare boot progression with sanitizer disabled; inspect CRT table stats; parity gate + runtime smoke | Includes NUI constructor skip list, bisect controls, and InitMakeString injection |
| `dc3_rodata_writable_for_force_unresolved` | DecompRuntimeStopgap | `src/xenia/dc3_hack_pack.cc` | `memory_protect` | decomp | required | multi | `/FORCE` unresolved globals resolve to image-base/RODATA and game writes fault without writable mapping | Decomp/link fixes eliminate unresolved BSS globals resolving into image-base pages | Run decomp boot with workaround disabled; confirm no writes into protected RODATA and no SIGSEGV | Scope intentionally limited to `0x82000000-0x822E0000` (RODATA only) |
| `dc3_zero_page_and_null_guard` | DecompRuntimeStopgap | `src/xenia/dc3_hack_pack.cc` | memory mapping | decomp | required | multi | Null object/vtable reads currently fault or spin due uninitialized globals from missing constructors | Restore missing constructors/global init correctness so null-deref patterns no longer gate bring-up | Run decomp boot with zero-page/guard disabled; confirm no early null-vtable/null-object blocker | Zero-filled guest page + Linux guard page below `virtual_membase` |
| `dc3_printf_xmp_stub_block` | Debug / RuntimeStopgap | `src/xenia/dc3_hack_pack.cc` | byte patch (stub-return) | decomp | experimental | multi | `_output_l` / `_woutput_l` loop on bad locale state; XMP overrides can deadlock on uninitialized CS | Decomp constructors/global init and locale/XMP state become correct | Targeted decomp smoke with each stub removed; parity milestones | Includes `_output_l`, `_woutput_l`, `XMPOverrideBackgroundMusic`, `XMPRestoreBackgroundMusic` |
| `dc3_assert_locale_runtime_stub_block` | Debug / RuntimeStopgap | `src/xenia/dc3_hack_pack.cc` | byte patch (stub-return) | decomp | experimental | multi | Debug/assert/locale/runtime helper paths recurse, trap, or corrupt heap under incomplete decomp state | Decomp correctness fixes remove loops/traps; consider semantic hooks only if still useful | Targeted decomp smoke with staged stub removals + parity telemetry | Includes `Debug::Fail`, locale getters, `DebugBreak`, `NtAllocateVirtualMemoryWrapper`, `RtlpInsertUnCommittedPages`, etc. |
| `dc3_holmes_stub_block` | Debug / RuntimeStopgap | `src/xenia/dc3_hack_pack.cc` | byte patch (stub-return) | decomp | experimental | multi | Holmes network/file/poll APIs block or spin during boot and are not needed for current bring-up | Either disable Holmes paths in decomp or make runtime state valid enough to avoid these calls | Targeted decomp smoke with subsets restored; parity milestone progression | Split from generic debug stubs after hack-pack extraction |
| `dc3_dataarray_datanode_string_safety_stubs` | Debug / RuntimeStopgap | `src/xenia/dc3_hack_pack.cc` + `src/xenia/emulator.cc` | byte patch (stub-return/blr) | decomp | experimental | multi | Corrupt data objects / unresolved globals cause infinite prints, bad refcount writes, and unbounded string append corruption | Decomp constructors/global initialization fix object validity and string state | Targeted decomp smoke with staged re-enable; watch parity hot loops/unresolved events | Includes `DataNode::Print`, `DataArray::{AddRef,Release}`, `String::operator+=` |
| `dc3_unresolved_import_stopgaps` | Imports | `src/xenia/dc3_hack_pack.cc` | import thunk cleanup / marker patching | decomp | required | multi | Decomp unresolved imports / markers need stabilization to proceed through boot | Decomp/link pipeline resolves or cleanly stubs imports upstream so runtime cleanup is no longer needed | Boot smoke + import cleanup stats + parity gate | Includes PE thunk/XEX marker cleanup paths |
| `dc3_import_indirection_diagnostics` | Imports | `src/xenia/dc3_hack_pack.cc` | telemetry/logging only | decomp | candidate_for_removal | xenia | Debug aid to inspect JIT indirection table slots near thunk area | Remove after import cleanup + parity telemetry make this redundant | Boot smoke if removing; ensure no regression in diagnosis workflow | Pure diagnostics; should not linger indefinitely |
| `dc3_fake_kinect_skeleton_injection` | Skeleton | `src/xenia/emulator.cc` | guest stub injection + byte patches | original (dev workflow) | experimental | xenia | Synthetic skeleton data for non-sensor bring-up/testing | Replace with extracted hack-pack helper or host-side semantic hook if still needed | Original build smoke with `--fake_kinect_data=true`; verify skeleton thread progresses | Extraction attempt was deferred after phase-2 refactor surfaced semantic-drift regressions elsewhere |
| `dc3_noop_stub_unresolved_call_fallback` | CPU Runtime | `src/xenia/cpu/backend/x64/x64_emitter.cc` | runtime no-op stub fallback | both (more visible on decomp) | required | xenia | Prevent crashes on null/out-of-range/unresolvable indirect calls during bring-up | Candidate for narrowing (not full removal) once runtime parity and callsite analysis show safe stricter behavior | Telemetry unresolved-call counters + hot-loop PCs; real-title smokes | This is broader than DC3 but currently documented here due DC3 bring-up use |

## Next Matrix Work (Planned)

1. Add `last_validated` timestamps for high-risk entries (CRT sanitizer, import cleanup, zero-page).
2. Split `dc3_assert_locale_runtime_stub_block` further if specific stubs prove independently removable.
3. Re-attempt extracting `dc3_fake_kinect_skeleton_injection` after preserving exact inline semantics (or replace with a host-level hook).
