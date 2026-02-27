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

| `dc3_manifest_address_automation` | Infrastructure | `dc3_hack_pack.cc` + `emulator.cc` + manifest generator | `address_catalog` JSON → `Dc3PopulateAddressesFromCatalog()` | decomp | required | multi | Decomp XEX rebuilds shift all addresses; manual refresh is error-prone | N/A — this IS the fix for stale addresses. Becomes unnecessary only if decomp produces bit-identical XEX | Rebuild decomp XEX → regenerate manifest → boot in xenia → verify "Populated N address catalog entries" log | 73 entries auto-resolved (81% of kAddr); 13 instruction-level fields remain hardcoded |
| `dc3_hack_pack_stub_resolved` | DecompRuntimeStopgap | `dc3_hack_pack.cc` | `PatchStub8Resolved` via manifest `hack_pack_stubs` | decomp | required | multi | 25 PatchStub8 calls used raw hex addresses that went stale on XEX rebuild | N/A — these now use manifest-resolved addresses with hardcoded fallbacks | Boot decomp XEX → verify all 25 stubs resolve from manifest (check "resolved from manifest" logs) | Covers: Splash (4), LiveCameraInput (2), HasFileChecksumData, VoiceInputPanel, Fader, ShellInput, UIScreen, MoveMgr, ObjRef, list clear, GotoFirstScreen, ClassAndNameSort (2), DirLoader::SaveObjects, SkeletonUpdate (2), SkeletonHistoryArchive, GestureMgr (3), DrawRegular |
| `dc3_main_loop_stubs` | DecompRuntimeStopgap | `dc3_hack_pack.cc` | `PatchStub8Resolved` | decomp | required | multi | Main loop poll functions hit corrupt data/vtables from uninitialized subsystems | Fix underlying subsystem init (Kinect, Synth, .milo loading) | Decomp boot 60s smoke with stubs removed one-by-one | Session 39: 11 new stubs for main loop stability (SkeletonUpdate, GestureMgr, DirLoader, DrawRegular) |
| `dc3_synth_security_stub` | DecompRuntimeStopgap | `dc3_hack_pack.cc` | `PatchStub8Resolved` | decomp | required | multi | Synth::InitSecurity → ByteGrinder::Init → DataReadString → yylex infinite loop (DRM init tries DTA parsing with corrupt/garbage input) | Fix flex lexer input OR determine InitSecurity is unnecessary for decomp (DRM) | Remove stub, verify SynthInit completes without hang | Session 44: DRM security init — likely permanently unnecessary for decomp testing |
| `dc3_hamsongmgr_init_stub` | DecompRuntimeStopgap | `dc3_hack_pack.cc` | `PatchStub8Resolved` | decomp | required | multi | HamSongMgr::Init reads corrupt song count from empty config → vector::reserve(huge) → length_error throw → memcpy crash past stack (C++ exceptions unsupported in JIT) | Provide minimal song config DataArray OR guard the reserve call OR fix config loading | Remove stub, verify Init completes without crash (need valid config data) | Session 44: Blocks song selection — needs config parsing fix first |
| `dc3_flowmanager_poll_stub` | DecompRuntimeStopgap | `dc3_hack_pack.cc` | `PatchStub8Resolved` | decomp | required | multi | FlowManager::Poll iterates FlowNodes via bctrl — corrupt vtable dispatches to 0x0C000000 (24M+/sec) | Fix FlowNode vtable corruption (from /FORCE BSS shifts during FlowInit) OR fix FlowInit to not create corrupt nodes | Remove stub, verify Poll iterates FlowNodes without corrupt dispatch | Session 44: **#1 blocker** for game state progression (screens, menus) |

## Stub Tier Classification (Session 39 Audit — updated)

### Tier 1: Permanent for headless mode (~130 stubs) — NO ACTION NEEDED

| Category | Count | Why permanent |
|---|---|---|
| NUI/Kinect SDK | ~70 | No Kinect hardware in xenia |
| Holmes debug network | ~40 | Dev network doesn't exist |
| XBC/SmartGlass | 3 | No SmartGlass in xenia |
| GPU init (null GPU) | ~11 | No GPU in `--gpu=null` mode |
| XMP music | 2 | No media player |
| Bink video | 2 | Video codec needs GPU thread |

### Tier 2: Decomp build artifacts (~36 stubs) — FIXABLE VIA DECOMP/LINKER WORK

| Stub | Root cause | Fix path |
|---|---|---|
| CreateDefaults bl-patches (2) | /FORCE duplicates → RndCam dtor crash | Resolve LNK4006 duplicates |
| Object factory overrides (3) | Heap unavailable during CRT `__xc` init | Fix CRT init ordering |
| StringTable::Add override | Heap not ready during `__xc` | Same |
| gConditional sentinel write | /FORCE reorders STLport sentinel | Resolve LNK4006 |
| Fader::UpdateValue | Corrupt std::set (SynthInit skipped) | Fix XAudio2 deadlock (Tier 3) |
| UIScreen::HasPanel | Corrupt std::list from .milo load fail | Fix .milo loading |
| ObjRef::ReplaceList | Corrupt linked list from .milo fail | Same |
| list\<ObjectDir*\>::clear | Corrupt linked list from ObjDirItr | Same |
| MoveMgr::Init | Config data missing/corrupt | Fix DTB/config loading |
| ShellInput::Init | SpeechMgr never initialized | Fix NUI subsystem init ordering |
| VoiceInputPanel | Speech config parsing fails | Same |
| UIManager::GotoFirstScreen | Panel loading fails (.milo → ObjectDir, not PanelDir) | Fix .milo/Rnd loading |
| ClassAndNameSort::ClassIndex | SystemConfig("system","dir_sort") corrupt | Fix config parsing |
| ClassAndNameSort::operator() | Corrupt object vtables from .milo fail | Fix .milo loading |
| DirLoader::SaveObjects | Corrupt STL list sort from .milo fail | Fix .milo loading |
| SkeletonUpdate::InstanceHandle | sInstance null (Kinect not init) | Fix Kinect subsystem init |
| SkeletonUpdate::PostUpdate | Kinect data never initialized | Same |
| SkeletonHistoryArchive::AddToHistory | Corrupt Skeleton objects | Same |
| GestureMgr::Poll/GetSkeleton/UpdateTracked (3) | Kinect not available | Same |
| App::DrawRegular | No GPU init in headless | Headless-permanent OR fix GPU init |

### Tier 3: Subsystem init blockers (~11 stubs) — FIXABLE VIA TARGETED INVESTIGATION

| Stub | Root cause | Fix path | Priority |
|---|---|---|---|
| **FlowManager::Poll** | Corrupt FlowNode vtable (0x0C000000) from /FORCE BSS shifts | Trace FlowInit, fix FlowNode allocation/vtable | **#1 — blocks game state progression** |
| **HamSongMgr::Init** | Empty config → garbage song count → vector::reserve crash | Provide minimal config OR guard reserve | **#2 — blocks song selection** |
| **Synth::InitSecurity** | ByteGrinder DTA parsing → yylex hang | DRM init — likely unnecessary for decomp | #3 — low priority (DRM) |
| Synth360::PreInit | XAudio2 CS deadlock under nop APU | Fix xenia nop APU CS init | Medium |
| SynthInit | Same | Same | Medium |
| Fader::UpdateValue | Corrupt std::set because Synth skipped | Unstubbing Synth fixes this | Medium |
| Splash (4) | DirLoader blocks on file I/O | Investigate VFS completion | Medium |
| Locale::Init | devkit: device not in emulator | Either stub permanently or fake device | Low |

## Next Matrix Work (Planned — updated Session 44)

1. **HIGHEST PRIORITY: Investigate FlowManager::Poll** — trace FlowInit (0x83085950) to understand FlowNode creation, identify corrupt vtable source. This is the #1 blocker for game state transitions.
2. **Investigate HamSongMgr::Init** — identify the config read that produces the huge reserve size. Either provide minimal song config via host-side DataArray injection, or guard the vector::reserve call.
3. **Investigate Synth::InitSecurity** — determine if this can be permanently stubbed (DRM is unnecessary for decomp). If not, trace the DataReadString input buffer to diagnose the yylex hang.
4. **Config loading**: Many stubs (HamSongMgr, FlowManager, per-frame noise) trace back to empty gSystemConfig. Fixing DTB binary loading (currently blocked by corrupt STL containers from /FORCE) or providing host-side config data would eliminate multiple stubs at once.
5. **.milo file I/O completion** — fixing VFS or content directory issues unstubs ~10 functions (UIScreen, ObjRef, list clear, ClassAndNameSort, SaveObjects, GotoFirstScreen, Splash).
6. **Kinect subsystem init** — proper SkeletonUpdate/GestureMgr initialization eliminates ~6 stubs.
7. Investigate staged Tier 2 stub removal as decomp LNK4006/LNK2001 errors are resolved.
