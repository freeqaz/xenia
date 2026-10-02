# Xenia fork cleanup plan

## Decisions (2026-10-02)

Recorded before Phase 1 (Lane A) started. They override the plan text below where the
two disagree.

1. **History rewrite done.** The `rb3-verify` binaries (the retail `.xex` and the PPM/PNG
   frame dumps) were filtered out of history, and `origin/frag-alloc-trace` was deleted.
   Done in `main` @ `fee3503b1`. The line numbers below still refer to the old integrate
   tip `62a667222`; `src/` on `main` is identical to integrate `6bf623353`, so re-locate
   a range before acting on it.
2. **The decomp-layout hack pack is KEPT**, isolated in `src/xenia/titles/dc3/decomp/` and
   gated exactly as before (DC3 title + detected decomp layout). It is what the DC3
   "627 forced-trap" check (harness S3) boots. The long-term goal is to replace each of
   its hacks with a real Xenia fix, then delete it; until then it is not deleted (this
   supersedes the "delete with decomp pack" actions in sections 2 and 5).
3. **The pinned inputs live at `/home/free/tmp/xenia-cleanup-plan/pinned/`** (shared toml,
   Aug-29 Checked binary, `xchan.py`, the s66 627-trap log) and are NOT in git. The
   regression harness (`tools/fork-regress/`) copies what it needs into its own content
   directory.

**Audited tree:** `/home/free/code/milohax/xenia` branch `integrate-2026-10` @ `62a667222`
(= `main` + upstream merge `c2cdfd233` (upstream `95a5c3ee2`) + `dc3-oracle` merge).
**Diff vs upstream:** `git diff upstream/master...integrate-2026-10` -- 297 files, +65,584 / -434.
**Method:** read-only. Four parallel audits (emulator.cc; DC3 TUs + HID/APU nop; core
cpu/kernel/base/memory/gpu; app + docs + tools + harness), each claim re-cited against a
static export of the branch (`git archive integrate-2026-10`). Every `file:line` below is on
`integrate-2026-10`. Spot re-verified by hand: content-wipe default + call site,
`ObReferenceObject` no-op, `XamUserGetXUID` mask, `XamNuiGetDeviceStatus`, `fault_spin_limit`,
classifier gate, IK cave address vs `symbols.txt`, Debug::Fail patch word, 627 count in
`dc3_nonreg_s66.log`, rb3-verify sizes.
**Date:** 2026-10-01. Nothing was built, run or committed.

**Pinned inputs saved by this audit** (`/home/free/tmp/xenia-cleanup-plan/pinned/`), because
they are the only copies and any un-isolated xenia run would destroy the first:
- `shared-xenia.config.toml.2026-08-29` -- the shared `~/.local/share/Xenia/xenia.config.toml`
  every 627-trap and RB3 §8v-8x result was measured under (any run without `--storage_root`
  rewrites it).
- `xenia-headless.checked.2026-08-29.783a0830c92e9cbc` -- the main checkout's Checked binary
  that produced the RB3 §8x and 627 results (xxh3 `783a0830c92e9cbc`).
- `xchan.py` -- the DTA-channel client (from `~/tmp/dc3-oracle-run/`, never committed).
- `dc3_nonreg_s66.log` -- last 627-trap reference log (627 hits, LR histogram source).

---

## 1. Executive summary

1. **The fork's risk is no longer where the 2026-08-25 review put it, and the oracle is
   more perturbed than BASELINE.md says.** The review's C1-C18 lane work landed (via
   `6bdcff4d2`), but every mitigation it added **defaults to fork behaviour for every
   title** and none is title-gated (§3.4). BASELINE.md measured `main@90eb07f81`, which does
   *not* contain those merges; on integrate the cvars exist, default ON, and several
   BASELINE rows are stale (content wipe is now live; nop audio now paced; nop_input lines
   moved).
2. **Three things actively damage the DC3 oracle today** and should be fixed before any
   golden is recorded: the `Debug::Fail` spin patch (latches `mFailing` *and* leaks a
   `MemPushHeap`), host code calling guest UI functions on the skeleton worker thread
   (nav bridge / transition forcing / LoadSong repair -- the 2026-06-02 race class and the
   most plausible *source* of the very FAIL that latches `mFailing`), and two independent
   120 BPM beat drives writing the same TaskMgr timelines.
3. **New harmful items not in any prior review:** DC3 content wipe defaults to `true` and now
   runs on the original layout and in the windowed app; IK telemetry on the original layout
   overwrites `UtilDrawPlane` and `ObjDirItr<RndMat>::Advance`; `ObReferenceObject` is a
   no-op paired with a real `ObDereferenceObject`; `XamUserGetXUID`/`XamUserCheckPrivilege`/
   `XamNuiGetDeviceStatus` changed for all titles; per-texture/per-frame `XELOGI` and a
   pipeline-cache rewrite after every pipeline creation for every Vulkan title; ungated
   async-pipeline draw dropping; `fault_spin_limit=4096` on for all titles;
   `dc3_dta_channel.cc` breaks non-Linux builds of `xenia-core`.
4. **Volume is mostly deletable.** ~5,900 of `dc3_hack_pack.cc`'s 6,965 lines are the
   decomp-layout hack pack, used only by the 627-trap boot bar; ~2,000 lines of
   `Rb3dxUiProbeThread`/`Rb3dxSaveGprLr23ProbeExtern` are forensics for refuted RB3
   hypotheses; ~690 lines of `emulator_headless.cc` are ungated stale-address DC3 dumps.
   The repo also carries a retail `.xex` (15.7 MB) and 227 MB of PPM frames in history.
5. **Target:** all title code under `src/xenia/titles/{dc3,rb3}/` in its own premake
   projects linked only by `xenia-headless`; `emulator.cc` reduced from 7,152 lines to
   ~900 (upstream 845) with four hook calls; every all-title mitigation back to upstream
   default with a per-title cvar profile applied at launch; fork docs under `docs/fork/`.
6. **Order:** build the regression harness first (Phase 0, no src changes), then one lane
   does the mechanical `emulator.cc` extraction (Phase 1, behaviour-identical), then four
   lanes with disjoint ownership in parallel (Phase 2).

### Bucket counts (rows of the §2 master table)

| Bucket | Meaning | Rows |
|---|---|---:|
| 1 | Genuine emulator fix (keep; title-agnostic; many upstreamable) | 64 |
| 2 | Title workaround needed to run (keep; title+cvar gate; per-title module) | 61 |
| 3 | Diagnostic / probe / telemetry (keep if useful; off; side-effect-free) | 41 |
| 4 | Dead or superseded (delete) | 51 |
| 5 | Wrong or harmful (fix/replace) | 41 |
| | **Total classified rows** | **258** |

(Counted mechanically from the B column of every §2 table. A row is one decision and may
group several related hunks. Four mixed rows are counted under their primary bucket: the DC3
and RB3 cvar blocks as 2, `shim_utils.h` as 1, and the upstream async-IO text as 4. Two
"preserve this note" rows are unbucketed. Several bucket-2 rows also carry a "5" sub-finding,
noted in their action column. By volume, the bucket-4 rows are most of the deletable lines:
about 5,900 lines of decomp pack, about 2,000 lines of RB3 forensics, and about 690 lines of
headless thread-6 dumps.)

### Top 10 worst hacks

| # | Hack | Where | Why it is worst |
|---|---|---|---|
| 1 | `Debug::Fail` worker spin → branch to epilogue | `dc3_hack_pack_skeleton.cc:215-228` (`0x825CE2DC ← 0x48000090`) | Skips `MemPopHeap` (`0x825CE364`) and `mFailing=0` (`0x825CE368`): every later MILO_FAIL/ASSERT on every thread is a silent no-op for the run, and a heap push leaks. Poisons all game-built oracle state. |
| 2 | Host calls guest UI code from the skeleton worker | `emulator.cc:3497-3618` (transition diag+force, `UIScreen::Enter` 3545), `3895-4088` (nav bridge `GotoScreen` Execute 4018/4042/4060/4071), `4090-4333` (LoadSong repair, Executes even when song set 4180-4193, 4298-4316) | Races the main thread (BASELINE k1 fault in `ObjectDir::FindObject`; note 3884-3893). Likely origin of the "BinkMovieImpl::Ready called in the wrong thread" FAIL that #1 then latches. |
| 3 | Two 120 BPM beat drives on the same fields | A `emulator.cc:4350-4455` (per NUI call), B `hid/nop/nop_input_driver.cc:423-583` (wall clock, called from every pad-0 poll 1046-1048) | Each overwrites the other's accumulator; song time is non-monotonic and non-reproducible. |
| 4 | All-title mitigations default ON | `mmio_handler.cc:25`, `x64_emitter.cc:50`, `ppc_scanner.cc:25`, `xboxkrnl_io.cc:30`, `xboxkrnl_rtl.cc:36,50`, `exception_handler_posix.cc:34`, `vulkan_command_processor.cc:46,60` | Every title gets silent zero reads, no-op null calls, truncated function scans, sync IO, auto-inited/force-released critical sections, thread parking after 4096 identical faults, and no submission wait headless. The review's C2/C4/C5/C6/C7/C8 are only "switchable", not fixed. |
| 5 | DC3 content wipe default `true`, now live | `dc3_hack_pack.cc:41-50, 6695-6713`; call `emulator.cc:5779-5780` (title gate only, before layout detection) | `remove_all(<content>/373307D9)` on every DC3 launch, both layouts, and in `xenia-app` (no `XE_HEADLESS_BUILD` guard). BASELINE row "inactive" is stale (`1d92efa5b` made it live; `bd3b5f76c` defaulted it on). |
| 6 | Guest writers in the HID driver, not title-gated | unpause nudge `nop_input_driver.cc:642-664`; attract→title force `878-922` (4 MiB scan ×2 then stomps UIManager +0x48/+0x4C/+0x2C) | A "probe" that writes `mWaitState`/`mPaused`; a UI stomp with no Exit/Enter; both fire for any title when a script file is set. |
| 7 | IK telemetry caves at wrong addresses on original layout | `dc3_hack_pack.cc:221` (`protocol_debug_string=0x8262F6B8`), `:287` (`0x82631C58`), applied `5918-6148`; called for original at `emulator.cc:7054-7063` | Inside `UtilDrawPlane` (`0x8262F538`+0x2F8, `symbols.txt:143296`) and `ObjDirItr<RndMat>::Advance`+0xE8. Stale decomp addresses; `kAddr` is never populated on original. Opt-in, but silently corrupts code when enabled. |
| 8 | Kernel/XAM semantic changes for every title | `xboxkrnl_ob.cc:280-283`; `xam_user.cc:102`, `498-508`; `xam_nui.cc:41-48` | No-op `ObReferenceObject` vs real `ObDereferenceObject` drops a ref it never took; XUID masks 2/4 now fail; all privileges granted; Kinect reported connected for all titles. Previously loud (unresolved import / upstream semantics). |
| 9 | `--rb3dx_ui_probe` "read-only" hosts guest writers | help text `emulator.cc:265-274` vs writers at `2044-2114` (splash unwedge + `XEvent::Set`), `2345-2360` (LoadMgr 1e30 poke, wiki says it corrupted mTimer), `2750-2808` (image SHA1 memcpy), autopilot `2428-2559` | A 1,345-line function documented as passive that pokes guest state; three writers are for refuted hypotheses. |
| 10 | Retail executable + 227 MB of frames committed | `rb3-verify/patch/clean_tu5_patched.xex` (15,675,392 B) + 86 PPMs, commit `0a98cd96b` | Copyrighted binary in history; 242 MB total; must be removed from history, not just the tip, before any push. |

Runners-up: Vulkan per-draw `steady_clock` + per-frame/per-texture `XELOGI`
(`vulkan_command_processor.cc:2718-2953`, `texture_cache.cc:615-618`,
`vulkan_texture_cache.cc:892-903`, `vulkan_render_target_cache.cc:1077-1090`) and a full cache
save after every pipeline creation from concurrent threads (`vulkan_pipeline_cache.cc:2476-2520`);
resolver auto-probing `/home/free/...` paths so the oracle's patch set depends on a mutable
`symbols.txt` (`dc3_nui_patch_resolver.cc:2393, 2408, 2422`).

---

## 2. Master classification table

Buckets: **1** genuine fix · **2** needed title workaround · **3** diagnostic · **4** dead/superseded ·
**5** wrong/harmful. "TG" = title-gated. "Layout O/D" = DC3 original / decomp layout.
Evidence for 4/5 is in the Evidence column (commit, doc §, or code read).

### 2.1 `src/xenia/emulator.cc` (7,152 lines; upstream 845)

Top-level layout: cvars 97-537 · crash snapshot 546-639 · NUI return externs 641-669 · RB3 alloc
trace 671-1375 · RB3 externs 1377-1470 · probe-thread ownership 1471-1516 · RB3 probe threads
1518-3152 · `Dc3NuiSequencerExtern` 3154-4458 · `Emulator::*` 4529-5280 · `CompleteLaunch`
5281-7152 (title hooks 5424-7140).

| Lines | Item | B | Gate | Action / evidence |
|---|---|:-:|---|---|
| 28-35, 52, 57-60, 66 | Fork includes: unused rapidjson (34-35), `<sys/mman.h>`/`<cerrno>`, `<set>`; DC3 headers; `hid/nop/nop_input_driver.h` (core→driver edge, autopilot only) | 4 | – | Delete unused; replace DC3/nop includes with `titles/title_hooks.h`. |
| 82-87, 5158-5165, 5283-5290, 5315-5317, 5380-5382 | `XE_HEADLESS_BUILD` UI guards / headless crash message | 1 | build | Keep (fork infra); later replace define with runtime `headless` (§4.4). |
| 107-117, 123-211 | 22 DC3 cvars (resolver, crt, debug probes, ik) | 2/3 | – | Move to `titles/dc3/dc3_flags.cc`; D-only ones die with decomp pack. |
| 118-122, 4728-4736 | `rb3_mount_update` (guest-visible VFS change, **not TG**) | 4 | cvar off | Delete; `08-boot-to-menu.md:51` "SECONDARY lever is dead". Keep note 4737-4744 in docs. |
| 130-136, 3629-3882 | `dc3_gameplay_probe` GATE PROBE/PKPROBE (5 guest Executes/frame) | 4 | cvar off | Delete; own help text says retired; GetKeys hang root-caused (7037-7044). |
| 143-150, 6478-6488 | `dc3_guest_overrides` -- read only to warn "ignored" | 4 | default true | Delete (review §2, still open). |
| 212-216, 546-639, 5038 | Crash snapshot JSON (title-agnostic despite name); dead `if(!mem_ptr)` 624 | 3 | `dc3_crash_snapshot_path` "" | Move to core `crash_snapshot.cc`, rename cvar, drop dead guard. |
| 217-537 | 26 RB3/SI cvars | 2/3/4 | – | Move to `titles/rb3/rb3_flags.cc`; delete the bucket-4 ones listed below. |
| 641-659, 6504-6509 | `Dc3NuiReturnOkExtern` / `Neg1Extern` | 2 | DC3 + `stub_nui_functions` | Move to `titles/dc3/dc3_nui_overrides.cc`. |
| 661-669, 6510-6512 | `Dc3NuiReturn1Extern` -- `kLiR3_1` (5843) in neither table | 4 | – | Delete (review §2, still open). |
| 671-876, 943-993, 5555-5625 | RB3DX alloc/free/return binary trace sink + overrides of `__savegprlr_26`/`__restgprlr_23` | 3 | `rb3dx_alloc_trace_path` "" + TG | Move to `titles/rb3/rb3_alloc_trace.cc`; reset sink on TerminateTitle. |
| 892-919, 5430-5458 | `__savegprlr_23` override dispatcher (overrides helper for all guest callers) | 3 | 5 cvars + TG | Keep as infra for the RB3 hooks below. |
| 922-942, 429-440 | `rb3dx_clamp_alloc` -- rewrites r3 | 4 | cvar off | Delete; OOM root-caused/fixed (`e248d624c`, wiki §4.C); wiki:415 "never fired". |
| 994-1063, 452-462 | `si_selftest` -- Executes `0x826684C0` + .data cave | 4 | cvar off | Delete; verdict recorded wiki:407-430. |
| 1064-1171, 7073-7140, 483-524 | `si_load_dll` approach (a): LoadUserModule, `InitSameInstrument` Execute; **raw host `xe::memory::Protect` RWX** 1145-1148, 7125-7126 | 2 | cvar off + TG | Move to `titles/rb3/rb3_si_harness.cc`; replace raw Protect with `heap->Protect` + restore (review §2, only annotated by `3d2a00441`). |
| 1172-1242, 525-537 | Approach (b): hardcoded July hook VAs + `si_hook_vas` | 4 | cvar | Delete; wiki:703-705 "(b) obsolete beyond install-proof". |
| 1245-1374, 5424-5428 | Alloc report (~40 XELOGE/hit, 6 fixed code dumps); `SetAllocProbeEnabled` pushed **ungated** (5428) | 3 | cvar | Demote to XELOGD; move the setter under the RB3 gate. |
| 1377-1429, 5663-5723, 5740-5746 | `Rb3dxIsHostOfflineExtern` (`NetSession::IsHost`) + signature-scan installer | 2 | `rb3dx_offline_join` + TG | Keep pending one run without it (wiki §4.D refuted it for RB3DX; still in §8v/§8w recipes). |
| 5724-5739 | Second 16 MB diagnostic bl-caller sweep | 4 | same | Delete (review §2, still open). |
| 1431-1447, 5460-5473 | `rb3_no_char_preview` (`UpdateCharCache` no-op) | 2 | cvar + TG | Keep; re-verify need (built for §8o theory refuted by §8p; `20cfa4b23` "insufficient alone"; still in §8w recipe). |
| 1449-1470 | Orphaned skip-calibration comment | 4 | – | Re-attach to its function. |
| 1471-1516, 4502-4505, 4663-4670 | Probe-thread ownership (C10 fix); **stop flag set at 1505 never reset** | 1 | – | Keep in `titles/probe_threads.cc`; reset flag on spawn. |
| 1518-1597, 5749-5767 | `Rb3dxSkipCalibrationPokeThread` -- writes profile_mgr byte every 150 ms forever | 2 | cvar + TG | Keep; make it one-shot after readback. |
| 1599-1680, 442-451, 5642-5649 | `si_probe` -- watches "the DEAD static .data cave" (1685) | 4 | cvar | Delete; wiki:402 verdict recorded. |
| 1682-1778, 463-482, 5651-5661 | `si_hook_verify` (read-only) | 3 | cvar + TG | Keep; fix stale help 480-481 ("pending"). |
| 1809-1910, 1988-2043, 2158-2186, 2263-2285, 2362-2427, 2890-3150 | UI probe: readers, census, BandUI state, loadq walk, stream census, SI claim table, panels, name table, saveload/net_sync, joypad, overshell | 3 | `rb3dx_ui_probe` + TG | Keep in `titles/rb3/rb3_ui_probe.cc`; demote 52 XELOGE→XELOGD (review §2 open); fix help text 265-274 ("read-only" is false). |
| 1911-1987, 2116-2157, 2187-2262, 2286-2344, 2560-2709, 2809-2889 | UI probe: SLOTCODE dump for the removed synthesized loop, TID6 liveness, LISTHEAD, BEATMASTER/BM-*, ldr-diff/freeze detector, 90-address CODE dump, POLLSINGLETON, gMainThreadID watch, HS-OBJ / LOADMGR-LOCATE one-shots | 4 | same | Delete; forensics for hypotheses refuted/solved in §8n-§8x (`852aec67e` removed the loop they served). |
| 2044-2114, 287-298 | `rb3_splash_unwedge` -- pokes `mState`, `heap->Protect`, `XEvent::Set` | 4 | cvar | Delete; §8n refuted by §8o (wiki:1315). |
| 2345-2360, 275-286 | `rb3_loadmgr_unbudget` -- writes 1e30 to `0x82E06E48/4C` | 4 | cvar | Delete; wiki:1111-1113 "corrupted adjacent mTimer fields… do not use". |
| 2710-2749, 299-306 | `rb3_overlapped_scan` | 4 | cvar | Delete; §8k refuted by §8l (wiki:1124). |
| 2750-2808, 337-345 | `rb3_tu5_hash_poke` -- image memcpy of SHA1s | 4 | cvar | Delete; wiki:1102-1107 "did not change the outcome"; §8w: patched arks unnecessary. |
| 2428-2559, 387-406 | RB3 autopilot (HID injection; requires ui_probe) | 2 | cvars + TG | Move to `titles/rb3/rb3_autopilot.cc` with its own gate (no ui_probe dependency). It is harness automation, not a probe. |
| 307-319, 5511-5553 | `rb3_mogg_key_table` -- 64 B at `0x82C76258` | 2 | "" + TG | Keep (§8x). No key in tree. |
| 346-357, 5475-5509 | `rb3_tu5_app_run_direct` -- `bl` patch at `0x82272E90` | 2 | cvar + TG | Keep (§8v, `c397a705d`); help text names deleted `loop_main/hold_main`. A page-0 fix in core (bucket 1) would retire it -- open research item. |
| 3154-3251 | `Dc3NuiSequencerExtern` frame fill: one skeleton, **constant 20-joint pose** 3227-3248 | 2 | DC3 + `fake_kinect_data` + O (6497) | Move to `titles/dc3/dc3_nui_frame.cc`; later feed from `DC3_POSE_SOCKET` (XENIA_ORACLE §5.2). |
| 3282-3297 | GestureMgr `mInControllerMode` := 1 on **every NUI call**; unvalidated host read; dead guards 3288/3292 | 2 | same | Make one-shot + validated. |
| 3253-3280, 3299-3358 | rdata scan range, `merge_busy` gate signal | 2 | same | Move with autonav. |
| 3360-3465 | `find_name_literal_ptr` per-**byte** readability scan (3419-3441) | 5 | same | Hoist readability to region (review §2 open). |
| 3497-3526 | Transition diag: Executes `UIScreen::CheckIsLoaded/Exiting/Entering` unconditionally on worker | 5 | same | Delete (diagnostic with guest calls off-thread). |
| 3528-3618 | Stuck-transition force-enter/complete: writes UIManager +0x2C/+0x48/+0x4C; `UIScreen::Enter` Execute 3545; `dc3_ik_telemetry` changes branch 3567-3569 | 5 | ≥120/180/240 NUI calls | Move to main-thread hook behind `--dc3_headless_autonav`; remove ik_telemetry coupling. |
| 3620-3627, 4335-4339 | Bootstrap-deleted note / stale "bootstrap disabled" comment | 4 | – | Delete stale comment; keep note. |
| 3884-3893 | NOTE 2026-06-02 LoadMgr::Poll race | – | – | **Preserve verbatim** (review "one thing worth preserving"). |
| 3895-4088 | Nav bridge: NUI-call thresholds; 4 MiB scan **twice** (3961-3994); `GotoScreen 0x8277B378` Executes from skeleton worker | 5 | `dc3_game_screen_real_goto` (true) picks path only | Move to main-thread hook (`HolmesClientPollKeyboard`, already used by DTA channel), behind `--dc3_headless_autonav`; cache scan. |
| 4090-4333 | LoadSong probe/repair: `SystemHeapAlloc`, `DataReadFile(d:\, devkit:\)`, `AddSongs`, inject ymca id 7011, `SetAssociatedPadNum`×2; probe Executes even when song set | 5 | none extra | Move to main thread; only act when song empty; drop probe Executes; better: replace by a DTA-channel `{set_song ymca}` call. |
| 4341-4349 | IK telemetry read | 3 | `dc3_ik_telemetry` | Move with `dc3_ik_telemetry.cc`. |
| 4350-4455 | **Beat drive A**: TaskMgr+0x48 := 0 (4395-4399); seconds/beats/ui timelines +1/30 s per NUI call, 120 BPM (4401-4445) | 5 | `game_screen` && `mPaused==0` | Keep one drive only (§2.4 decision), main-thread, per guest frame, behind cvar; or delete if paced nop audio drives the clock. |
| 5002-5022, 5040-5071 | ExceptionCallback: tolerate null `guest_function`; lr/ctr/CR/XER dump; fault-addr + ±0x40 code dump (dead guard 5065) | 1 | – | Keep, upstreamable after removing dead guard. |
| 5073-5144 | Validated stack walk, 512 cap (C9 partial: `013ca855f`); log predicate 5120-5121 still near-every-frame | 1 | – | Keep; add `--crash_stack_walk_frames` (64); fix log predicate. |
| 5387-5413 | milo-trace session begin/end | 3 | `milo_trace_enable` | Keep in core (title-agnostic). |
| 5779-5780 | **Content wipe** call (DC3, all layouts, before layout detection) | 5 | `dc3_clean_content_cache`=**true** | Default false; call only from decomp path (or delete with it). |
| 5782-5797 | MMIO write soft-fault range `0x83320000-0x836C0000` (DC3; no cvar; never cleared) | 2 | TG | Move to DC3 profile behind `dc3_*` cvar; clear on TerminateTitle. |
| 5799-5821, 6168-6476 | Early manifest load; `.text` FNV; layout detection; symbol/manifest/fingerprint/resolver; sig trace (6434-6476 cvar) | 2 | TG | Move to `titles/dc3/dc3_nui_overrides.cc`; pin inputs (no auto-probe). |
| 5824-5899 | NUI comment header (stale; 8 speculative TODOs) | 4 | – | Move rationale to `docs/fork/dc3/`; 2-line pointer (review §2 open). |
| 5901-6046 | Original-layout NUI table (59 overrides) | 2 | TG + `stub_nui_functions` | Move to data (JSON or constexpr table in `titles/dc3/`). |
| 6054-6166 | Decomp-layout NUI table | 4 | D | Delete with decomp pack (§2.2). |
| 6490 | `ClearGuestFunctionOverrides()` inside DC3 block | 5 | – | Delete (wipes any earlier registration; TerminateTitle/LaunchPath already clear). |
| 6491-6535, 6545-6600 | Handler selection + registration loop | 2 | TG | Move. |
| 6536-6544 | "ULTRA FORCED" duplicate sequencer registration (skips .text checks) | 4 | – | Delete (redundant with 6497). |
| 6602-6618 | Summary with `const patched=0` | 4 | – | Delete. |
| 6620-6639 | `ApplyDc3SkeletonHackPack` call | 2 | TG + `fake_kinect_data` + O | Move; see skeleton table. |
| 6642-6710 | Decomp hack pack apply + kAddr populate + IK on decomp | 4 | D | Delete with decomp pack (or park in `titles/dc3/decomp/`). |
| 6738-6745 | `SaveLoadManager::Activate` → blr | 2 | O | Move. |
| 6747-6770 | `HamPanel::FocusComponent` → UIPanel; `IsEventDialogOnTop` → 0 | 2 | O | Move. |
| 6772-6788 | `CDReadDone` → 1; `ContentMgr::RefreshDone` → 1 | 2 | O | Move; tag as "kernel async-IO gap" research item. |
| 6790-6865, 6988-6996 | Splash PrepareNext/Begin/Suspend/Resume; `SpeechMgr::Grammar::Unload`; `BinkMovieSys::Init`; `Movie::Poll` → 0 | 2 | O (+fake_kinect) | Move. |
| 6867-6961 | Calibration bypass (guard nop; 3× blr; 13-insn NavData rewrite; ShouldWaitForRecovery → 0) | 2 | O + `fake_kinect_data` | Move. |
| 6963-6986, 7022-7023 | `PauseForSkeletonLoss` → blr; `HandleWait+0x90` bne → b | 2 | same | Move; document perturbed subsystem "pause/wait". |
| 7012-7020, 7024-7025 | `XMAHALAllocateContexts` → 0; `HamAudio::IsReady+0x70` → 1 | 2 | same | Move; **re-test** -- paced nop APU (`77d85acaa`) may make both unnecessary. |
| 7027-7051 | `HamDirector::SongAnim` → EXPERT | 2 | same | Move; separate cvar `--dc3_force_expert_anim` (changes which anim plays). |
| 7054-7064 | IK telemetry on original XEX → writes caves at decomp addresses | 5 | `dc3_ik_telemetry` | Fix addresses (see §2.2) or refuse on original until fixed. |
| 7066-7071 | DTA channel install | 3 | `dc3_dta_channel` "" + O | Keep (oracle instrument). |

### 2.2 DC3 translation units (`src/xenia/dc3_*.cc`)

**`dc3_hack_pack.cc` (6,965)** -- `ApplyDc3HackPack` is called only inside
`if (DC3 && dc3_is_decomp_layout)` (`emulator.cc:6655-6699`). The decomp layout is used only by
the 627-trap boot bar: dc3-decomp `scripts/`+`docs/` have 18× `=original`, 1× `=auto`
(`docs/debugging/xenia.md:23`, May), 0× `=decomp`; `XENIA_ORACLE.md:8-10` defines the oracle as
original `debug.xex`.

| Lines | Item | B | Layout | Action / evidence |
|---|---|:-:|---|---|
| 41-50, 6695-6713 | `dc3_clean_content_cache` default **true** + `Dc3MaybeCleanStaleContentCache` | 5 | O+D | Default false; decomp-only (see emulator.cc 5779). |
| 63-289 | `Dc3Addresses kAddr` -- mixed table: `ham_ik_*` (269-286) original, `protocol_debug_string` 221 / `holmes_client_poll` 287 stale decomp; comment 72-75 says "from default.map" | 5 | mixed | Split: original-layout constants into `titles/dc3/dc3_addresses_original.h`; drop decomp half. |
| 54-61, 292-2262, 2391-2516, 2698-5550, 6156-6180, 6736-6963 | Decomp-layout hack pack: guest pool, host ArkFile::Read, `_output_l` bridges, MemAlloc bootstrap, DataArray safety, debug stub table, import stopgaps, debug stubs (host Debug::Fail, factory map, GPU/Dx stubs), XDK overrides, runtime stopgaps (4 MB stack, whole-PE RW, Linux host mmap guards 4424-4444, zero page 4544-4578), CRT patches, catalog populate | 4 | D only | **Delete** (archive tag `dc3-decomp-layout-archive`). Only consumer is the 627-trap bar, which this plan replaces (§5.3). |
| 891-1167 | `Dc3ReadCacheStreamDiagnostic` -- never referenced | 4 | – | Delete. |
| 1168-1315, 1788-1977, 2263-2369, 2517-2697 | RCS step probes, MemOrPoolAlloc probe, FindArray override modes, SystemConfig probe | 3 | D | Delete with pack. |
| 2934-2950 | JIT indirection diag (reads host 0x80000000) | 4 | D | Delete (HACK_RETIREMENT_MATRIX: candidate_for_removal). |
| 4206-4209, 4253-4256, 4336-4340, 4420-4422, 72-75, 6117 | **Wrong "ALL layouts"/address comments** (code unreachable on O); unreachable "Skipped 17 decomp-only stubs" else-branch; 20 redundant `ctx.is_decomp_layout` checks (1702, 1790, 2009, 2265, 2393, 2519, 3378, 3542, 3617, 3638, 3653, 3666, 3678, 3689, 4340, 4447, 4465, 4495, 4517, 4593) | 5 | – | Vanish with the pack; if pack kept, correct them. |
| 5551-5889, 6196-6677 | IK cave builders; `ReadDc3IKTelemetry` (read-only bone walk) | 3 | O+D | Move to `titles/dc3/dc3_ik_telemetry.cc`. |
| 5890-6150 | `ApplyDc3IKInstrumentation`: caves at `0x8262F6B8` (inside `UtilDrawPlane`, `symbols.txt:143296`), dead-hook override at `0x82631C58` (inside `ObjDirItr<RndMat>::Advance`); its own comment 6115-6124 says the hook is dead | 5 | O+D | Allocate caves via `SystemHeapAlloc`; take addresses from the original table; delete dead hook. Bucket 2 (opt-in code patch) after fix. |
| 6678-6693, 6715-6734 | Category names / dispatchers | 3 | – | Keep what IK needs. |

**`dc3_hack_pack_skeleton.cc` (364)** -- entry `ApplyDc3SkeletonHackPack`, gate DC3 + `fake_kinect_data` + O (98).

| Lines | Item | B | Action / evidence |
|---|---|:-:|---|
| 17-86, 341-356 | `GotoFirstScreen` override (`FindObject` + `GotoScreen` Execute on caller thread; "attract_screen" `SystemHeapAlloc`) | 2 | Keep; fix stale "nav bridge re-invokes this" comments 28-30, 337-340 (`emulator.cc` never references `0x8277B140`). |
| 103-204 | PPC stub + constant pose over `NuiSkeletonGetNextFrame 0x829C2790`; code page RW (123) | 4 | Shadowed: `emulator.cc:6497-6499` registers `Dc3NuiSequencerExtern` at the same address first. Delete after one confirming run. |
| 211-213 | `0x8242E74C ← li r28,0x21` (SkeletonUpdateThread INFINITE wait → 33 ms) | 2 | Keep; better: host 30 Hz timer signals the NUI event the guest waits on (unpatched wait). |
| 214 | `0x8242E1B0` nop (IsOverride branch) | 2 | Keep. |
| 215-228 | **`0x825CE2DC ← 0x48000090`** Debug::Fail spin → `b 0x825CE36C` (epilogue) | 5 | Skips `MemPopHeap` 0x825CE364 and `stb r30,0(r26)` (`mFailing=0`) 0x825CE368 (target `Debug.s:4555-4672`). Fix per §2.4. |
| 229-248 | Comments on removed patches (negative results) | – | Keep → docs. |
| 272-291 | `BinkMovieImpl::Ready` → 1 | 4 | Own comment 299-302 says insufficient; measured `mFailThreadMsg` proves the guest body still ran (vtable path). Delete after confirming run. |
| 293-315 | `MoviePanel::IsLoaded` → 1 | 2 | Keep. |

**Other DC3 TUs**

| File:lines | Item | B | Action / evidence |
|---|---|:-:|---|
| `dc3_nui_patch_resolver.cc:25-2150, 2152-2370, 2696-2790` | Signature tables (58 orig, 87 decomp), matcher (fails closed), resolve | 2 | Keep orig half; drop decomp tables with pack. |
| `dc3_nui_patch_resolver.cc:2391-2433` | Auto-probe **absolute** `/home/free/...` paths 2393, 2408 (main checkout's fingerprint file), 2422; CWD-relative 2394-2395, 2409, 2423-2424 | 5 | Remove; original layout uses compiled-in table verified by signature, no external file. Oracle patch set currently depends on a `symbols.txt` modified 2026-09-30. |
| `dc3_nui_patch_resolver.cc:2435-2672` | symbols.txt / fingerprint / manifest loaders | 2 | Manifest+fingerprint parts are D: delete with pack. |
| `testing/dc3_nui_patch_resolver_test.cc` (656) | Catch2 tests | 1 | Keep; move to `titles/dc3/testing/`. |
| `dc3_runtime_telemetry.cc:27` | `dc3_runtime_telemetry_include_ppc_words` defined, never read | 4 | Delete (review §2, still open). |
| `dc3_runtime_telemetry.cc:19-549` | JSONL sink (inactive unless enable+path) | 3 | Keep; first add a title-agnostic `cpu::UnresolvedCallObserver` so `x64_emitter.cc:41,137,638-731` and `emulator_headless.cc:46,1006-1030` stop calling a DC3 TU. |
| `dc3_dta_channel.cc:14-16` | Unconditional `<sys/socket.h>`/`<sys/un.h>`/`<unistd.h>` in a file globbed into `xenia-core` | 5 | Linux-only premake filter (breaks Windows build today). |
| `dc3_dta_channel.cc:39-102, 158-170, 211-308, 402-571` | cvars; original-only fingerprints (refuses on mismatch 659-674); throw-hook `longjmp`; per-request snapshot/restore incl. `mFailing` clear; Evaluate on guest main thread via `HolmesClientPollKeyboard` override | 3 | Keep (oracle). Note the override skips the stock body each frame (`CritSecTracker` + `SendKeyboardMessages`); assert the Holmes input queue is empty rather than only logging (513-518). |
| `dc3_dta_channel.cc:658-716` | Install: `SystemHeapAlloc(0x5000)` at install (shifts heap), throw hook, **detached server thread** (710), `g_channel` never freed | 5 | Allocate on first request; own and join the thread (C10 class). |

### 2.3 HID / APU nop drivers

`nop_input_driver.cc` (+1,195): DC3 logic runs only with `memory_` set (headless + `--scripted_input_file`,
`xenia_headless_main.cc:263-265`); **no title_id check and no cvar anywhere**.

| Lines | Item | B | Action / evidence |
|---|---|:-:|---|
| 25-33, 110-352, 742-760, 925-1003, 1005-1045, 1049-1229 | Generic scripted-input player: `scripted_pad_subtypes`, parser (`+N`=N×50 ms 291), timed injection, directive state machine, caps, GetState/Keystroke (duplicate edge logic 1140-1170 vs 1192-1218) | 1 | Keep in `hid/nop`; dedupe; replace title hooks with a `ScriptedInputTitleAdapter` interface. |
| 40-108, 386-421 (DC3 half) | DC3 original-layout screen reader (TheUI `0x82F1A8E0`) | 2 | Move to `titles/dc3/dc3_scripted_flow.cc`. |
| 354-384 | RB3 screen reader (TheBandUI `0x82DFD2B0`) | 2 | Move to `titles/rb3/`. |
| 423-583, 1046-1048 | **Beat drive B** (wall clock, writes TaskMgr+0x48 518 and 6 timeline floats 566-571), called on every pad-0 poll | 5 | Delete (§2.4). |
| 585-641, 666-740 | `ProbeDc3GameplayState` read-only part; PAUSE-ONSET diag (`sym_str` 693-697 reads host C-string after 1-byte check) | 3 | Keep as read-only probe in DC3 module; fix string read. Source of `gpState=` lines analyze_run.py counts (724). |
| 642-664 | **Unpause nudge** (game+0xA4=0, gp+0xF8=0, game+0x60=1, game+0x5E=0); function-static once-per-process | 5 | Bucket-2 writer mislabelled as probe: move to DC3 module behind `--dc3_force_unpause`, TG; reset on relaunch. |
| 760-857 | DC3 UIManager transition diagnostic (log only) | 3 | Move to DC3 module. |
| 858-877 | Attract fallback: press A every 3 s | 2 | Move to DC3 adapter. |
| 878-922 | **Attract→title force**: 4 MiB heap scan ×2 then UIManager +0x48/+0x4C/+0x2C stomp; no RB3 guard, no TG | 5 | Delete (duplicates nav bridge 3928-3929 + A-press fallback). |
| 110-131 | `NopInjectButtonPress` process singleton (RB3 autopilot) | 3 | Keep behind adapter. |
| `apu/nop/nop_audio_system.cc:26-74, 85-93` | Paced `NopAudioDriver` (5.333 ms/frame, joined in dtor) | 1 | Keep; upstream PR. Changes DC3 behaviour vs BASELINE row "callbacks never fire" (stale). |

### 2.4 Decisions for the three oracle-damaging items (Lane B)

1. **Debug::Fail.** (a) Root cause first: move every host→guest UI call (nav bridge, transition
   forcing, LoadSong repair, and `GotoFirstScreen` re-entry if any) onto the guest main thread
   via the `HolmesClientPollKeyboard` hook the DTA channel already uses
   (`dc3_dta_channel.cc:499-571`), so the `BinkMovieImpl::Ready` CHECK_THREAD should stop
   firing. (b) Then restore the original spin (faithful) and add a host **tripwire**: read
   `TheDebug+0x104` (`mFailThreadMsg`) each poll; non-null ⇒ mark the run TAINTED in the log
   (harness fails it). (c) Only if a worker must survive: retarget to
   `0x825CE2D0 ← 0x48000094` (`b 0x825CE364`), which runs `MemPopHeap` and clears `mFailing`
   (`r30 = 0x82F60000`, low byte 0 -- verify against `Debug.s` before landing); also zero
   `+0x104` and log. Option (c) is a *new* divergence and needs its own baseline.
   Confirm the hypothesis in (a) first by logging the host thread id at every
   `processor->Execute` in `Dc3NuiSequencerExtern`.
2. **Beat drives.** Measure first whether the paced nop audio driver now advances DC3's own
   song clock after unpause. If yes: delete both. If no: delete B outright; keep A's
   fixed-increment semantics, moved to the main-thread hook (one step per guest frame, equal
   to native `DC3_FAST_TIME` 1/120 s -- XENIA_ORACLE §5.1), behind `--dc3_host_beat_drive`.
3. **Autonav scope.** All of `emulator.cc:3299-3627, 3895-4333` plus the nudge goes behind one
   new cvar `--dc3_headless_autonav` (default **off**); `run_dc3_oracle.sh` passes it
   explicitly. Evaluator-tier probes (XENIA_ORACLE §3.2) then run with it off, booting to
   `title_screen` and driving through the DTA channel.

### 2.5 Core files (cpu / kernel / base / memory / gpu / vfs / ui / third_party)

**cpu/**

| File:lines | Item | B | Gate (default) | Action / evidence |
|---|---|:-:|---|---|
| `mmio_handler.cc:25-35, 600-635` | Unmapped read → zero dest + skip (first 20 logged) | 2 | `soft_fault_unmapped_reads` (**true**), not TG | Flip default false; DC3/RB3 profile opt-in. (C2 partial, `481cde322`) |
| `mmio_handler.cc:59-77, 525-559; .h:72-81` | Write-unprotect range setter (armed DC3-only `emulator.cc:5790`; static atomics never cleared) | 2 | setter | Clear on TerminateTitle. (C3 done) |
| `mmio_handler.cc:39-57, 505-514, 629-640` | RB3DX TOPHOLE logging ≥0xFFD00000 | 3 | setter | Keep behind RB3 gate. |
| `mmio_handler.cc:496-503` | Init `cur_access`; check `QueryProtect` result | 1 | – | Upstream PR (B4). |
| `mmio_handler.cc:561-598` | Skip faulting `clflush`/`prefetch` | 4 | – | Delete: prefetch can't fault; `clflush` no longer emitted (`x64_seq_memory.cc:1086-1093`). |
| `x64_emitter.cc:50-62, 606-651, 684-713, 769-780, 817-841, 864-874, 911`; `x64_seq_control.cc:15-63, 228-307` | Null/unresolvable call → no-op stub; constant-null dropped at emit | 2 | `tolerate_null_guest_calls` (**true**), not TG | Flip false; profile opt-in. (C4 partial, `5ee39f063`) |
| `x64_emitter.cc:842-863, 879-887` | Out-of-table indirect → resolve thunk; SysV old-style resolve args | 1 | – | Upstream PR (A1/C). |
| `x64_emitter.cc:41, 88-138, 652-677, 718-746` | Non-text classifier + DC3 telemetry include; gate 134-137 **never closes for a clean title** (counter only increments on a find) → `GetModules()` global lock on every resolve | 5 | – | Gate behind telemetry active only; move to observer hook. |
| `x64_emitter.cc:952-953` | Extern addr into `PPCContext::scratch` on every extern call (only DC3 externs read it) | 3 | – | Keep only if DC3 externs still need it; else delete. |
| `x64_emitter.cc:544-578` | `TrapDebugBreak` log (`tw/td forced trap hit!`, PC always 0 at 577) | 3 | – | Keep (627 harness contract); pass real PC. |
| `x64_emitter.cc:65-81, 199, 324-341, 433-521; x64_seq_memory.cc:766-1069` | milo-trace hooks | 3 | `milo_trace_enable` | Keep. |
| `x64_emitter.cc:914-925` | `UndefinedCallExtern` refactor residue | 4 | – | Revert to zero diff. |
| `x64_backend.cc:421-480, 502-545, 595-605` | SysV host↔guest + resolve thunks | 1 | Linux | Upstream PR A1 (R2 open). |
| `x64_seq_vector.cc:797,1005,1375,1469,2122,2161,2292,2305`; `x64_sequences.cc:2473-2565, 2987, 3064` | `__m128i` by value → `const vec128_t*` (SysV) | 1 | – | Upstream PR A2. |
| `x64_seq_vector.cc:2539, 2596` | `EmulateFLOAT16_2/4` still take `__m128i` by value but get a pointer (non-F16C hosts) | 5 | – | Fix in A2. |
| `x64_sequences.cc:1662-1665, 1714` | mulx comment residue after revert `1ab2e21ed` | 4 | – | Delete. |
| `x64_code_cache.cc:139-143, 290-294; .h:78-84`; `x64_function.cc:34-36` | Indirection bounds; null machine code | 1 | – | Upstream PR C. |
| `x64_seq_memory.cc:1086-1093` (+ dead `is_clflush` 1134, 1148) | `dcbst/dcbf` no `clflush` | 1 | – | PR after discussion; delete dead code. |
| `ppc_emit-private.h:27-64`; `ppc_opcode_lookup_gen.cc:2-44` | Unimplemented/decoder-miss logs restored, asserts removed (generator `tools/ppc-table-gen:429` still emits assert) | 2 | – | Restore asserts under `break_on_unimplemented_instructions`; regenerate. (C1 done for logs, `2ad04f6b2`) |
| `ppc_hir_builder.cc:110-132, 210-277` | 1 MiB cap with warning; invalid/unimplemented logs restored | 1 | – | Keep. |
| `ppc_hir_builder.cc:157-171` | `end_address` clamped to mapped heap, **silent** | 5 | – | Add log; then upstream. |
| `ppc_scanner.cc:25-34, 166-189` | Stop after 8 invalid words (truncates functions with data) | 2 | `scanner_stop_on_invalid_run` (**true**) | Flip false; profile opt-in. |
| `ppc_scanner.cc:36-45, 112-131` | Clamp scan to mapped memory | 1 | `scanner_clamp_to_mapped_memory` (true) | Upstream; drop cvar. |
| `ppc_scanner.cc:58-78` | Truncation log at XELOGD | 3 | – | Raise to XELOGW once. |
| `processor.cc:208-221` | `GetModules` resize→reserve | 1 | – | Upstream PR C. |
| `processor.cc:224-271, 362-409; .h`; `function.cc:134-176`; `function_debug_info.h`; `ppc_translator.cc`; `cpu_flags.*` | `RegisterGuestFunctionOverride` API; milo-trace plumbing; `pe_override` | 1 | – | Keep (fork infra; the override API is the title-hook seam). |
| `xex_module.cc:1054-1160` | PE override, no bounds/`fread`/`ftell` checks (C18 open: 1066-1071, 1089) | 5 | `pe_override` "" | Add checks or delete. |
| `stack_walker_posix.cc:22-212` | POSIX rbp-chain walker; `SafeReadU64` 181-187 unvalidated; wrong comment 36-38 | 1 | – | Harden, then upstream (B8). |
| `finalization_pass.cc:47-49`; `hir/value.cc:413-415`; `assembler.h:13` | Label fmt; `MulHi` sign-ext fix; `<cstdint>` | 1 | – | Upstream PR C. |

**kernel/**

| File:lines | Item | B | Gate (default) | Action / evidence |
|---|---|:-:|---|---|
| `xboxkrnl_io.cc:30-43, 356, 439, 512` | Suppress STATUS_PENDING for async files | 2 | `io_force_synchronous_completion` (**true**) | Flip false (= upstream); A/B DC3 original before choosing its profile. (C7 partial, `6a8811964`) |
| `xboxkrnl_io.cc:45-58, 244-263, 301-334, 409-424` | OVERLAPPED writeback + ABI probe | 4 | `rb3_overlapped_writeback` (false) | Delete: `ac0052e5b` proved IO status block *is* the OVERLAPPED. |
| `xboxkrnl_io.cc` (upstream async `else` branches) | Deleted, but upstream had `if (true \|\| …)` (dead) | 4 | – | Restore upstream text (behaviour-neutral); merge msg `92107d0e5` "upstream PENDING = data loss" is wrong. |
| `xboxkrnl_io.cc:157-159, 205-208, 229-241, 288-295, 646, 739` | NOT FOUND at XELOGI; XELOGW per short read | 3 | – | Demote. |
| `xboxkrnl_io.cc:842-850` | `IoDismountVolumeByFileHandle` → success | 1 | – | Upstream D9. |
| `xboxkrnl_io_info.cc:356-377` + `info/volume.h:57-78` | `XFileFsDeviceInformation` | 1 | – | Upstream D5 (`66d8d41c6`). |
| `xboxkrnl_io_info.cc:27-33, 134-141` | RB3DX QIF probe log; DECLARE tombstone | 3 | – | Delete tombstone. |
| `xboxkrnl_rtl.cc:36-48, 577-599` | Auto-init CS whose type≠1 | 2 | `autoinit_critical_sections` (**true**) | Flip false; opt in DC3 decomp + RB3DX/RB3E only. (C8 partial, `59f0b4bff`) |
| `xboxkrnl_rtl.cc:50-63, 668-700` | Leave-CS forces ownership + clamps recursion | 2 | `rtl_leave_critical_section_force_release` (**true**) | Flip false; same opt-in. |
| `xboxkrnl_rtl.cc:31-35, 446-570` | CS diagnostics -- **re-inits CS at 567 regardless of autoinit** | 5 | `crt_critical_section_diagnostics` (false) | Remove the mutation. |
| `xboxkrnl_rtl.cc:68-76`; `rtl.h:27-35` | Global atomic CS counters on every Enter/Leave (read `emulator_headless.cc:1684`) | 3 | – | Gate behind diagnostics cvar. |
| `xboxkrnl_rtl.cc:433-439, 471-474, 388-392` | CS wait-list self-ref init; `RtlImageXexHeaderField` null guard | 1 | – | Upstream D4. |
| `xboxkrnl_rtl.cc:607-622` | "Fast CAS once" | 4 | – | Revert: upstream `atomic_inc` -1→0 already acquires. |
| `xboxkrnl_rtl.cc:850-916` | `RtlUp/Downcase` ASCII-only; `RtlCaptureContext` 0x200 memset; `RtlUnwind` no-op; `__C_specific_handler` | 2 | – | C16 partial; implement or error without writing guest memory. |
| `xboxkrnl_threading.cc:384-397` | DC3 4 MB min stack (hardcoded TID literal, no cvar) | 2 | TG | Move to DC3 profile cvar `dc3_min_thread_stack`. |
| `xboxkrnl_threading.cc:40-103, 599-643, 824-940, 1268-1432` | `headless_thread_diagnostics`, `rb3_trace_shutdown`, `kernel_stack_walk_frames`; `IsMainGuestThread` (tid==6, 103); wait/SetEvent census | 3 | off | Keep; move to `xboxkrnl_threading_diag.cc`. |
| `xboxkrnl_threading.cc:104-290` | Wait census: RB3 range 0x82527A00 hardcoded (180); **detached infinite reaper thread 219-280**; fixed-depth walks ignore `kernel_stack_walk_frames` | 5 | `headless_thread_diagnostics` | Own + join the reaper (C10 regression from `1bc4a3b7a`/`c8712c9c9`); use the cvar depth. |
| `xboxkrnl_threading.cc:729-749, 1243-1253` | `KeSetEvent`/`KeWait` null object → warn-once + Release return | 1 | – | Upstream D8 (optional). |
| `xboxkrnl_threading.cc:1942-1998` | `KeTimer`/`KeMutant` stubs (memset, lies) | 2 | – | C16 partial; implement or error. |
| `xboxkrnl_crypt.cc:690-750` | `XeKeysSetKey`/`XeKeysAesCbc` (slot table, correct arg order, zero IV) | 1 | – | Upstream D1 (`f137bcedb`). |
| `xboxkrnl_crypt.cc:752-781` | `XeKeysGetConsoleID` 5×0x42; `ConsolePrivateKeySign` success w/o write | 2 | – | Keep, document. |
| `xboxkrnl_ob.cc:88-167` + `xboxkrnl_module.cc:163-184` | `Ex*ObjectType` exports; accepts sentinel or address (fails open if lookup returns 0) | 1 | – | Fix fail-open; upstream D6 (`48c193e7b`). C14 done. |
| `xboxkrnl_ob.cc:280-283` | **`ObReferenceObject` no-op** vs real `ObDereferenceObject` (195-208) | 5 | – | Implement (retain handle) or leave unresolved. |
| `xboxkrnl_ob.cc:271-278` | `ObCreateObject` → UNSUCCESSFUL + log | 2 | – | Keep. |
| `xboxkrnl_memory.cc:21-40, 188-202, 419-428` | `rb3dx_force_zero_commit` | 4 | TG, false | Delete: own help text "did NOT fix the OOM"; superseded by `posix_allocfixed_zero_commit`. Keep the comment in docs (review "preserve"). |
| `xboxkrnl_memory.cc:444-447` | `MmFreePhysicalMemory` misaligned → returns without freeing | 5 | – | Free or assert. |
| `xboxkrnl_memory.cc:693-709` | `ExAllocatePoolWithTag` → `SystemHeapAlloc` (`ExFreePool` exists) | 2 | – | Keep. |
| `xboxkrnl_audio.cc:58-144` | Per-handle dummy driver `0x4155FFFF` | 2 | – | Keep (C15 done, `adb327579`). |
| `xboxkrnl_audio.cc:154-233`; `xboxkrnl_misc.cc:26-78` | XAudio/Etx/PsCam/KeSaveFP/LDI stubs (some lack null checks) | 2 | – | Add null checks. |
| `xboxkrnl_cpp_throw_hook.h`; `xboxkrnl_debug.cc:104-112` | `g_cpp_throw_hook` (set by DTA channel 706) | 1 | null | Keep (fork hook, title-agnostic). |
| `xboxkrnl_video.cc:137 / 202-242 / 365-374` | memset cast / one-shot Vd logs / **VdSwap XELOGI + non-atomic counter** | 3 | – | Demote 371; atomic. |
| `xboxkrnl_xconfig.cc:62-112` | assert → XELOGW same error | 1 | – | Upstream D8. |
| `xboxkrnl_modules.cc:54-114`; `xam_content*.cc`; `xam_notify.cc`; `null_command_processor.cc:38-82` | Info logs | 4 | – | Revert to zero diff. |
| `util/object_table.cc:196-215, 259-278`; `xobject.cc:75-93, 446`; `xobject.h:148-155` | Teardown fixes | 1 | – | Upstream D3 (`ef5025af9`). |
| `util/shim_utils.h:320 / 401-430` | memset cast / pointer params `(elided)` | 1/3 | – | Cast: upstream F. Elision: use non-faulting read. |
| `xam/xam_enum.cc:76-98` | Overlapped NO_MORE_FILES → SUCCESS, count 0; **no cvar** | 2 | always | Add cvar (default off), DC3/RB3 profile opt-in until hardware evidence. Keep the comment block (review "preserve"). |
| `xam/xam_net.cc:39-47, 237-273` | XNetRandom real entropy | 1 | `xnet_random_constant_fill` (false) | Upstream D2 (`407c711ed`), drop cvar. |
| `xam/xam_net.cc:417-422`; `xam_msg.cc:96-100` | Alertable-APC consumers | 1 | – | Upstream with B1. |
| `xam/xam_net.cc:1081-1130`; `xam_nui.cc:87-230`; `xam_voice.cc:41-65`; `xam_input.cc:216-232`; `xam_ui.cc:573-674` | NetDll/NUI/voice/input/UI stubs (several `*ptr=0` w/o null check) | 2 | – | Add null checks. |
| `xam/user_profile.cc:26-34` | XUID `0xE00000000000BABE`, "Player1" | 1 | – | C13 open: upstream D7 with save-migration note. |
| `xam/xam_user.cc:90-110` | **`XamUserGetXUID` only honours mask bit 1** (upstream answered 2/4) | 5 | – | Restore upstream mask semantics. |
| `xam/xam_user.cc:498-508` | **`XamUserCheckPrivilege` grants everything** | 5 | – | Restore deny default; per-title profile if RB3 needs it. |
| `xam/xam_user.cc:28-80, 120-251, 857-892` | Multi-user profiles (`local_user_count`=1), signin helpers | 1 | – | Keep; upstream candidate. |
| `xam/xam_nui.cc:41-48` | **`XamNuiGetDeviceStatus` reports Kinect connected for every title**; non-atomic counter | 5 | – | Gate on `fake_kinect_data` (after moving that cvar out of gpu_flags). |
| `xam/xam_ui.cc:17-353, 435-559` | Headless auto-answer dialogs | 1 | `XE_HEADLESS_BUILD` | Keep; switch to runtime headless. |
| `xam/xam_info.cc:314, 339-343, 368-442` | assert→log; kernel32-shaped exports; FillSystemTime null check | 1 | – | Keep. |
| `xam/xam_info.cc:323-332` | "RB3DX TERMINATE" XELOGE fires for every title | 3 | – | Rename/demote. |
| `xbdm_misc.cc:47-57, 78, 102-115` | Counter / XELOGI / `DmGetSystemInfo` 0x24 memset | 3 | – | Demote; size-check memset. |
| `kernel_state.h:169` | `notify_listener_count()` no reader | 4 | – | Delete. |
| `user_module.h:66` | `set_stack_size` (only `dc3_hack_pack.cc:3899`) | 4 | – | Dies with decomp pack. |

**base/, memory, vfs, apu, third_party**

| File:lines | Item | B | Gate (default) | Action / evidence |
|---|---|:-:|---|---|
| `threading_posix.cc:175-283, 385-486, 901-943, 1052, 1114-1213, 1385-1395, 1511-1524`; `testing/threading_test.cc:1141-1264` | POSIX alertable APC rewrite (eventfd + ppoll; FIFO; delivered outside signal context) | 1 | – | Upstream B1 (`4f3a5d8bf`) **after** fixing eventfd leak (created 205-206, never closed); add tests for SignalAndWait/WaitMultiple/AlertableSleep. |
| `threading_posix.cc:752-766, 1479-1481` | Self-destroying thread detaches | 1 | – | Upstream B2 (`25c506505`). |
| `threading_posix.cc:26, 136-142, 1497-1506`; `threading.h:97-99` | `GetLastSuspendHostRip` (no caller; `REG_RIP` without arch guard) | 4 | – | Delete. |
| `exception_handler_posix.cc:70, 151, 296-299, 360-372, 389-391, 432-434` | SIGBUS via AV path; restore old handler on unhandled fault | 1 | – | Upstream B6. |
| `exception_handler_posix.cc:41-66, 200-222, 440-476`; `exception_handler.h:229-250` | Fault counters + XMA aperture split (guest 0x7FEA0000 in base/) | 3 | – | Keep; move constant out of base. |
| `exception_handler_posix.cc:26-38, 223-286` | **Livelock breaker parks thread after 4096 identical rip+addr faults** (counts MMIO/XMA too; help says "Abort", code parks) | 5 | `fault_spin_limit` (**4096**) | Default 0; RB3DX profile opt-in; count only non-advancing faults. (Code-read finding, not measured.) |
| `memory_posix.cc:90-126` | `AllocFixed` mprotect-first; MAP_PRIVATE fallback | 1 | – | C12 done (`1c34d9078`); upstream B3. |
| `memory_posix.cc:144-178` | `QueryProtect` via `/proc/self/maps` (`fopen` in SIGSEGV path) | 1 | – | Rewrite with `open`/`read`, then upstream B4 (R2 open). |
| `memory_posix.cc:208-221, 241-258, 267-272` | memfd/shm fallback; `MapFileView` MAP_FIXED | 1 | – | Add `MFD_CLOEXEC`, `MAP_FIXED_NOREPLACE`; upstream B3. |
| `memory.cc:40-74, 967-991, 1141-1148, 1519-1520`; `memory.h:209-214` | Zero newly committed runs | 1 | `posix_allocfixed_zero_commit` (true) | Upstream (true = console); drop cvar. |
| `memory.cc:918, 1116-1119, 1152-1174, 1217-1221, 1264-1281` | BaseHeap off-by-one; clamps → error; Release guards | 1 | – | Upstream B5 (C17 done, `bef769be9`). |
| `memory.cc:16-20, 226-256` | `protect_zero=false`: memset page 0 + guard mmap below membase; comment cites dc3_hack_pack | 5 | upstream cvar | Revert; RB3 page-0 handling belongs in a title profile (relates to `rb3_tu5_app_run_direct`). |
| `filesystem_posix.cc:198-215, 245`; `system_gnulinux.cc:14, 36-62` | GetInfo fields; S_ISDIR; stderr message box | 1 | – | Upstream B7. |
| `arena.cc:66-68` | Oversized alloc own chunk (incomplete) | 1 | – | Complete, upstream G. |
| `vfs/virtual_file_system.cc:248-257`; `apu/xma_context.cc:13, 709-731` | assert → logged warning | 1 | – | Upstream G (optional). |
| `console_app_main_posix.cc:17`; `hid/sdl/sdl_input_driver.cc:165,199,203`; `ui/windowed_app_main_posix.cc:20` | clang-20 warning fixes | 1 | – | Upstream F. |
| `third_party/half/include/half.hpp:1042` | Vendored literal-suffix fix (R3 open) | 1 | – | Send to half; keep as patch file. |

**gpu/, ui/**

| File:lines | Item | B | Gate (default) | Action / evidence |
|---|---|:-:|---|---|
| `vulkan_command_processor.cc:46-58, 3530-3552` | No submission wait headless (C5) | 2 | `!presenter() && headless_skip_submission_wait` (**true**) | Flip false; run scripts opt in; retest with the 1401 `EndSubmission` first. |
| `vulkan_command_processor.cc:60-70, 2749-2756, 2802-2812` | Drop draws on non-capture frames (C6) | 2 | `headless_capture_only_draws` (**true**) | Flip false; scripts opt in. |
| `vulkan_command_processor.cc:409-423, 3096-3109` | **`SetHeadlessMode(true)` (async pipelines) + "ASYNC SKIP" draw drop whenever `!presenter()`, no cvar** | 5 | none | Put behind cvar default off. |
| `vulkan_command_processor.cc:2718-2720, 2856-2953, 3074-3130` | ~12 `steady_clock::now()` + static counter on **every draw, windowed too**; unused lambda 2866 | 5 | none | Remove or gate behind `headless_verbose_diagnostics`. |
| `vulkan_command_processor.cc:1398-1404` | Presenter-less `IssueSwap` closes frame (fixes upstream leak) | 1 | – | Upstream E1. |
| `vulkan_command_processor.cc:72-100` | `dc3_persist_render_state` (true) / `dc3_replay_depth_disable` / `dc3_inline_render` -- **not TG despite names** | 2 | deferred path | Rename `headless_*` or title-gate. |
| `vulkan_command_processor.cc:424-468, 1165-1186, 1406-1815, 2757-2840, 5156-5433` | Readback, capture, PPM dumps, deferred draw record/replay; **5317-5321 writes saved resolve vertices back into guest physical memory** | 3 | `dump_frames_path`/`force_all_draws` | Keep; split into `VulkanHeadlessCapture`; audit the guest write-back (risk). |
| `vulkan_command_processor.cc:2717-2747, 3360-3378` | Per-draw diagnostics | 3 | `headless_verbose_diagnostics` (false) | Keep. |
| `vulkan_command_processor.cc:38-44`; `command_processor.cc:34` | Redundant DECLAREs | 4 | – | Delete. |
| `vulkan_pipeline_cache.cc:40-49, 102-159, 243-265, 2475` | `VkPipelineCache` persisted to temp / `--vulkan_pipeline_cache_path` | 1 | – | Default to storage_root/cache; save at shutdown; upstream E3. |
| `vulkan_pipeline_cache.cc:2476-2520` | **XELOGI + full cache save after every pipeline creation**, concurrent threads, no mutex; upstream failure-log comment deleted | 5 | none | Save once at shutdown; restore comment. |
| `vulkan_pipeline_cache.cc:67-70, 164-202; .h:339-390` | Creation threads owned + joined (C10 done, `4c1de4106`) | 1 | – | Keep. |
| `vulkan_pipeline_cache.cc:464, 579-600` | `warmup_wait_` branches inside `if (!warmup_wait_)` | 4 | – | Delete. |
| `texture_cache.cc:615-618`; `vulkan_texture_cache.cc:892-903`; `vulkan_render_target_cache.cc:1077-1090` | **`RSTAB: LOAD` per texture load, XELOGI per presented frame, `RSTAB2` per resolve -- all titles** | 5 | none | Delete (files can reach zero diff). |
| `vulkan_render_target_cache.h:117-127` | `PrepareEdramBufferForClear()` unused | 4 | – | Delete. |
| `shared_memory.cc:308; .h:88-94, 183-184` | `suppress_memory_watches_` during flush (stale-texture risk) | 2 | headless deferred | Keep; document risk. |
| `command_processor.cc:563-577` | Primary ringbuffer `assert_always` removed, upstream `break` kept | 5 | – | Restore assert in Checked or gate. |
| `command_processor.cc:584-591` | Write back read pointer after each primary packet | 1 | – | Upstream E2. |
| `command_processor.cc:965-1005, 1044-1098` | Null-GPU PPM dump; **WAIT_REG_MEM STALL XELOGI on every backend** | 3 | `dump_frames_path` / none | Demote stall log. |
| `draw_util.cc:936-937`; `graphics_system.cc:256-267` | Log additions | 1 | – | Upstream E5. |
| `gpu_flags.cc:52-76` | Capture cvars + **`stub_nui_functions`/`fake_kinect_data` (DC3 NUI cvars in GPU layer)** | 3 | false | Move NUI cvars to `titles/dc3/dc3_flags.cc` (needs the xam_nui gate to read them via a title-agnostic flag, e.g. `kinect_device_present`). |
| `graphics_system.cc:26-31, 67-88, 147-159`; `null_graphics_system.cc:15-17` | `#ifndef XE_HEADLESS_BUILD` guards -- compiled only into **unlinked** `xenia-gpu-headless` | 4 | – | Delete. |
| `null_graphics_system.cc:31-42` | No VulkanProvider when not presenting | 1 | – | Upstream E4. |
| `gpu/premake5.lua:23-75`; `gpu/null/premake5.lua:21-34` | `xenia-gpu-headless`/`xenia-gpu-null-headless` -- **nothing links them** (`app/premake5.lua:155-156` links `xenia-gpu`/`-null`) | 4 | – | Delete projects + their MAINTAINERS comment. |
| `render_target_cache.h:189-192`; `texture_cache.h:99-100`; `vulkan_texture_cache.cc:930-931` | Capture accessors | 1 | – | Keep (fork infra). |
| `shader_interpreter.h:88`; `texture_dump.cc:44,86`; `texture_cache.h:162-170,464`; `vulkan_render_target_cache.h:462-472`; `vulkan_pipeline_cache.h:200-209`; `command_processor.cc:1355` | clang-20 `-Wnontrivial-memcall` fixes | 1 | – | Upstream F. |
| `ui/vulkan/functions/device_1_0.inc:19,43,59,72` | `vkCmdFillBuffer`, PipelineCache functions | 1 | – | Upstream with E3. |

### 2.6 app/, debug/, build

| File:lines | Item | B | Action / evidence |
|---|---|:-:|---|
| `app/emulator_headless.cc:53-65, 104-915` + `debug/dc3_gdb_rsp_protocol.h` | GDB RSP server (~800 lines; no DC3 content) + protocol helpers | 1 | Move to `src/xenia/debug/gdb_rsp/`; rename `dc3_gdb_rsp_*` cvars/namespace to `gdb_rsp_*` (review §3 open). |
| `app/emulator_headless.cc:66-76, 1232-1239` | `rb3dx_hub_teardown_trace` | 3 | Move to RB3 module via a headless-report hook. |
| `app/emulator_headless.cc:78-99` | SIGUSR2 JIT IP sampler, process-wide handler, no cvar | 3 | Gate behind cvar. |
| `app/emulator_headless.cc:917-1037, 2056-2113` | Ctor/Run/timeout (`_Exit(0)` can drop last log batch)/boot reporting (`BOOT:` lines) | 1 | Keep; flush logger before `_Exit`. |
| `app/emulator_headless.cc:1043-1203` | Fault-livelock diagnosis decoding `MemHeap::Alloc 0x827BCA78` (RB3DX) **not TG**, `_Exit(70)` | 3 | Move RB3 decode to RB3 module. |
| `app/emulator_headless.cc:1206-1319, 2004-2054` | 3 s "Thread Status Report" + per-thread LR + back-chain walk (ungated) | 3 | Keep; the report line format is a **harness contract** (`analyze_run.py`). |
| `app/emulator_headless.cc:1320-2003` | `if (thread_id()==6)` block of hardcoded DC3 dumps for **any title's** thread 6: GetKeys hunt (original addrs), invarg/vsnprintf, IAT/thunk with **raw host read of `0x83A00964`** (1678-1681), ~45 Feb-2026 decomp "Function probes" (1687-1763), D3D/MainThread dumps, "XAUDIO2 NOT STUBBED" (1954-1974), XapiThreadNotify | 4 | Delete; stale decomp layout (`mainCRTStartup` now 0x830E8414 decomp / 0x82335EE0 orig). The raw host read is a 5-class crash risk on titles whose image ends below it. |
| `app/emulator_headless.cc:2115-2145` | `ReportCrash` -- no caller | 4 | Delete. |
| `app/emulator_headless.h:84-85` | `module_reporting_enabled_` never read; `exit_code_` never set | 4 | Delete / wire up. |
| `app/xenia_headless_main.cc:39-69, 72-80, 143-315` | storage/content/target/devkit_root cvars; boot report; timeout; scripted input; HeadlessMain | 1 | Keep. |
| `app/xenia_headless_main.cc:70` | `DEFINE_string(gpu, "null")` vs `xenia_main.cc:66` `"any"` (shared toml cross-contaminates) | 5 | Rename `headless_gpu` or share one definition. |
| `app/xenia_headless_main.cc:81-88` | `scripted_input_file` help hardcodes DC3 TheUI | 2 | Make generic; title adapter supplies screen reader. |
| `app/xenia_headless_main.cc:90-94` | `dc3_gdb_rsp_prelaunch_sleep_ms` | 1 | Rename. |
| `app/xenia_headless_main.cc:117-141` | Pad-presence hack for RB3DX autopilot (123-130), dummy `"999999s:A"` (133-137) | 2 | Replace with explicit `--headless_pad_count`. |
| `app/premake5.lua:131-235` | `xenia-headless` ConsoleApp | 1 | Keep; link `xenia-titles-*` here only. |
| `premake5.lua:17-29`; `kernel/premake5.lua:25-47` | `xenia-core-headless`, `xenia-kernel-headless` duplicates (compile 5 DC3 TUs twice) | 2 | Phase 3: replace `XE_HEADLESS_BUILD` with runtime `cvars::headless`; delete projects. |
| `testing/premake5.lua`; `cpu/testing`, `cpu/ppc/testing` premake | Resolver tests; extra links needed because DC3 TUs live in core | 1 | Keep tests; links drop to zero once TUs move. |
| `xenia-build:1228-1231, 1282-1337` | Wine runner + PPC binutils autodetect; `with_runner` defined twice | 1 | Dedupe; upstream candidate. |
| `.gitignore:104-105` | `/stdout`, `/xenia_dc3_patch_manifest.json` | 4 | Find/fix writers; delete entries. |

### 2.7 Docs, tools, rb3-verify

| Path | B | Action / evidence |
|---|:-:|---|
| `rb3-verify/patch/clean_tu5_patched.xex` (15,675,392 B, = rb3-xenon `_tu5probe/clean/`) | 5 | Copyrighted retail binary: purge from history (commit `0a98cd96b`) before any push. **User decision** (history rewrite of a shared branch). |
| `rb3-verify/frames/**` (86 PPM/PNG, ~227 MB) | 4 | Purge with the xex. |
| `rb3-verify/patch/*.py`, `rb3-verify/scripts/*` | 3 | Move to `tools/rb3/`. `apply_same_instrument_clean_tu5.py:11` reads a deleted rb3-xenon worktree JSON → cannot reproduce; mark. |
| `CLAUDE.md` (82) | 5 | Stale: decomp-layout-only workflow (3, 13-61); no RB3/oracle/harness. Rewrite as pointer to `docs/fork/README.md`. |
| `docs/dc3-boot/{STATUS,TODO,CONTINUATION_PLAN,GOAL,ARCHIVED}.md`, `agent_*`, `HACK_RETIREMENT_MATRIX.md`, `DC3_HEADLESS_CHANGE_AUDIT_2026-02-20.md`, `DC3_NUI_ROADMAP.md`, `rb3-bringup-notes.md`, `rb3-same-instrument-verify.md`, `jit-fault-wiki/08-*.md`, `09-*.md` | 4 | Archive under `docs/fork/archive/` (self-declared historical/superseded; Feb-2026 decomp campaign; 08/09 superseded by WORKSTREAM §8c/§8v-§8x). Delete `agent_e_extracted_addresses.txt` (stale generated header). |
| `docs/dc3-boot/dc3_nui_fingerprints.txt` | 2 | Load-bearing (`dc3_nui_patch_resolver.cc:2408`). Move only after the code stops auto-probing it. |
| `docs/fork-cleanup-review.md`, `dc3-oracle/*`, `jit-fault-wiki/WORKSTREAM-*`, `00-07`, BRIEF/CRASH-REPORT/PLAN, `dc3_gdb_debugging.md`, `dc3_render_*` | 3 | Move to `docs/fork/{cleanup,dc3,rb3,debug}/`. Add stale banners: BASELINE.md :126 (content wipe), :112 (nop audio), :146-151 (cvars exist on integrate); `01-symptom-and-evidence.md:12-14` (the "retail" xex is RB3DX; "SI bypass" is dirty-disc bypass). |
| `tools/dc3_runtime_parity_gate.sh` + `_telemetry_diff.py`, `_crash_signature_triage.py`, `_guest_disasm.py`, `test_headless.sh`, `dc3_trace_on_break.sh`, `dc3_gdb_rsp_mvp_mock.py` | 3 | Keep; parity-gate milestone contract is reusable in the harness. |
| `tools/dc3_nui_cutover_gate.sh`, `dc3_crt_bisect.sh`, `dc3_extract_addresses.py`, `analyze_poolalloc.py`, `dc3_gdb_rsp_snapshot_bridge.sh` | 4 | Archive/delete (Feb cutover done; CRT blocker resolved s37; superseded by manifest `address_catalog`; poolalloc belongs in dc3-decomp). |

---

## 3. Target structure

### 3.1 Source layout

```
src/xenia/
  emulator.cc                 ~900 lines: upstream + ExceptionCallback improvements, headless
                              guards, milo-trace session, and FOUR hook calls (3.2)
  crash_snapshot.{h,cc}       title-agnostic crash JSON (from emulator.cc:546-639, 5073-5144)
  titles/                     NOT globbed by xenia-core ("*.cc" is non-recursive)
    premake5.lua              xenia-titles (registry), xenia-titles-dc3, xenia-titles-rb3
    title_ids.h               kTitleDc3 = 0x373307D9, kTitleRb3 = 0x45410914
    title_hooks.{h,cc}        TitleLaunchContext; Register/ApplyLaunchHooks/OnTerminate/OnShutdown;
                              empty registry by default (xenia-app links nothing)
    title_profile.{h,cc}      per-title cvar profile (sets the mitigation cvars at launch, logs each)
    probe_threads.{h,cc}      owned host threads + stop flag (from emulator.cc:1471-1516)
    dc3/
      dc3_flags.cc            all dc3_* cvars + stub_nui_functions/fake_kinect_data (moved out of gpu_flags)
      dc3_addresses_original.h  one table of original-layout constants + fingerprints
      dc3_title.cc            Dc3ApplyLaunchHooks (emulator.cc 5769-5822, 6620-7071 original half)
      dc3_nui_overrides.cc    NUI table (data), resolver glue, registration (5824-6600 minus decomp)
      dc3_nui_frame.cc        Dc3NuiSequencerExtern frame fill + controller-mode (3154-3297)
      dc3_main_thread_hook.cc HolmesClientPollKeyboard hook shared by DTA channel, autonav, beat drive
      dc3_autonav.cc          nav bridge / transition forcing / LoadSong repair, main-thread only,
                              behind --dc3_headless_autonav (default off)
      dc3_beat_drive.cc       the single surviving drive (or deleted, §2.4)
      dc3_scripted_flow.cc    ScriptedInputTitleAdapter: screen reader, unpause (cvar), attract A-press
      dc3_original_patches.cc skeleton.cc minus dead stub; Debug::Fail tripwire
      dc3_ik_telemetry.cc     caves via SystemHeapAlloc, original addresses; reader
      dc3_dta_channel.{h,cc}  Linux-only filter
      dc3_runtime_telemetry.{h,cc}  registered as cpu::UnresolvedCallObserver
      dc3_nui_patch_resolver.{h,cc}, testing/dc3_nui_patch_resolver_test.cc
      decomp/                 ONLY if the decomp boot bar is kept (default: deleted; tag archive)
    rb3/
      rb3_flags.cc            surviving rb3*/si_* cvars
      rb3_title.cc            Rb3ApplyLaunchHooks (5430-5553, 5663-5767, 7073-7140)
      rb3_savegpr_hook.cc     __savegprlr_23 dispatcher (892-919)
      rb3_alloc_trace.cc      671-876, 943-993, 1245-1374, 5555-5625
      rb3_si_harness.cc       1064-1171, 1682-1778
      rb3_ui_probe.cc         read-only rows of §2.1, XELOGD
      rb3_autopilot.cc        2428-2559 (own cvar)
      rb3_scripted_flow.cc    RB3 screen-reader adapter
  debug/gdb_rsp/              RSP server + protocol (from app/emulator_headless.cc:104-915)
  hid/nop/                    generic scripted player + ScriptedInputTitleAdapter interface
  app/                        headless app; links xenia-titles-dc3/-rb3 and registers them in main()
docs/fork/
  README.md                   index; replaces CLAUDE.md body (CLAUDE.md keeps a 10-line pointer)
  cleanup/                    fork-cleanup-review.md, this plan, upstream-PR tracker
  harness/                    harness spec + pinned-asset manifest
  dc3/                        oracle (BASELINE, SPIKE_LOG), render, NUI rationale (from emulator.cc:5824-5899)
  rb3/                        WORKSTREAM (canonical), jit-fault-wiki 00-07, BRIEF/CRASH/PLAN
  debug/                      gdb/RSP usage, general debugging tips
  archive/                    dc3-boot-2026-02/, superseded RB3 docs, audits
tools/harness/                run scripts, analyzers, pinned toml (from docs/dc3-oracle/*.sh|py|toml)
tools/rb3/                    dirty-disc / SI patchers, input scripts (from rb3-verify/)
```

### 3.2 What stays in `emulator.cc` (the whole fork diff to that file)

| Site | Hook |
|---|---|
| includes (replace 57-60, 66) | `#include "xenia/titles/title_hooks.h"` |
| `~Emulator` (4505) | `titles::OnShutdown();` |
| `TerminateTitle` (4665, absorbs 4670) | `titles::OnTerminateTitle();` -- joins probe threads, ends telemetry, clears MMIO range |
| `LaunchPath` (4681-4685) | keep `ClearGuestFunctionOverrides` + `MiloTraceEnd` (core) |
| `ExceptionCallback` (5038, 5073-5144) | `WriteCrashSnapshotJson(...)`; `DumpGuestStackWalk(...)` (core) |
| `CompleteLaunch` (5424, replacing 5424-7140) | `titles::ApplyLaunchHooks({memory_.get(), processor_.get(), kernel_state_.get(), module.get(), title_id_.value_or(0), content_root_, headless});` -- must stay after `InitializeShaderStorage` (5420-5422) and before `LaunchModule` (7142): RB3 overrides must register before the JIT compiles `App::App` (5462-5463); the SI DLL must map before LaunchModule (7075-7077). `ApplyLaunchHooks` first applies `title_profile` then the title's hooks. |

`emulator.h` stays byte-identical to upstream.

### 3.3 Upstream core files we modify -- verdicts

Legend: **REV** revert to upstream (zero diff) · **UP** upstreamable fix (PR, then zero diff) ·
**HOOK** keep but move title logic out via hook/profile · **INFRA** keep fork-local
(headless / null GPU / tracing).

| Verdict | Files (net +/-) |
|---|---|
| **REV** | `kernel/kernel_state.h` +1 · `kernel/xam/xam_content.cc` +6/-1 · `xam_content_device.cc` +4 · `xam_notify.cc` +13 · `xboxkrnl_modules.cc` +8 · `gpu/null/null_command_processor.cc` +31/-2 · `gpu/texture_cache.cc` +4 · `gpu/vulkan/vulkan_render_target_cache.cc` +14 · `base/threading.h` +4 · `gpu/premake5.lua` +53 · `gpu/null/premake5.lua` +15 · `gpu/command_processor.h` +5 · `kernel/user_module.h` +1 (after decomp pack) · `gpu/graphics_system.cc` (all but a 2-line log) |
| **UP** | `cpu/backend/assembler.h` +1 · `cpu/compiler/passes/finalization_pass.cc` +2/-3 · `cpu/hir/value.cc` +3/-2 · `gpu/shader_interpreter.h` · `gpu/texture_dump.cc` · `ui/windowed_app_main_posix.cc` · `base/console_app_main_posix.cc` · `hid/sdl/sdl_input_driver.cc` · `third_party/half` (to half) · `gpu/draw_util.cc` · `cpu/backend/x64/x64_backend.cc` +41/-4 · `x64_code_cache.{cc,h}` · `x64_function.cc` · `x64_seq_vector.cc` +28/-27 · `x64_sequences.cc` (ABI hunks) · `cpu/stack_walker_posix.cc` +192 · `kernel/util/object_table.cc` · `kernel/xobject.{cc,h}` · `kernel/info/volume.h` · `xboxkrnl_crypt.cc` (SetKey/AesCbc) · `xam_msg.cc` · `xam_net.cc` (entropy+APC) · `xboxkrnl_ob.cc` + `xboxkrnl_module.cc` (after ObReferenceObject fix) · `xam/user_profile.cc` · `xboxkrnl_xconfig.cc` · `base/threading_posix.cc` +371/-26 · `base/testing/threading_test.cc` · `base/memory_posix.cc` +99/-9 · `memory.cc` +153/-12 · `memory.h` · `base/filesystem_posix.cc` · `base/system_gnulinux.cc` · `base/arena.cc` · `vfs/virtual_file_system.cc` · `apu/xma_context.cc` · `apu/nop/nop_audio_system.cc` +62/-2 · `gpu/null/null_graphics_system.cc` · `ui/vulkan/functions/device_1_0.inc` · `cpu/processor.cc` (GetModules) · `xenia-build` |
| **HOOK** | `emulator.cc` +6329/-22 (→ ~+60) · `cpu/mmio_handler.cc` +194/-6 · `x64_emitter.cc` +430/-11 · `x64_seq_control.cc` +116/-7 · `ppc/ppc_scanner.cc` +79 · `ppc/ppc_hir_builder.cc` +93/-8 · `ppc_emit-private.h` +40/-3 · `ppc_opcode_lookup_gen.cc` +27/-1 · `xboxkrnl_io.cc` +166/-52 · `xboxkrnl_rtl.cc` +289/-3 · `xboxkrnl_threading.cc` +616/-11 · `xam_enum.cc` +35/-1 · `xam_nui.cc` +159/-2 · `xam_user.cc` +170/-43 · `xboxkrnl_memory.cc` +61/-3 · `base/exception_handler_posix.cc` +191 · `hid/nop/nop_input_driver.{cc,h}` +1187/-8, +125 · `gpu/vulkan/vulkan_command_processor.{cc,h}` +1076/-3, +69 · `vulkan_pipeline_cache.{cc,h}` · `gpu/gpu_flags.{cc,h}` |
| **INFRA** | `premake5.lua` +16 · `testing/premake5.lua` · `kernel/premake5.lua` · `kernel/xam/xam_ui.cc` +167/-2 · `xboxkrnl_cpp_throw_hook.h` · `xboxkrnl_debug.cc` · `xboxkrnl_rtl.h` · `cpu/cpu_flags.*` · `cpu/function.cc` · `function_debug_info.h` · `ppc_translator.cc` · `processor.h` (override API) · `x64_emitter.h` · `x64_seq_memory.cc` (milo-trace) · `cpu/xex_module.cc` (fix C18 or delete) · `gpu/render_target_cache.h` · `gpu/shared_memory.*` · `vulkan_texture_cache.*` (drop log) · `base/exception_handler.h` · stub collections (`xboxkrnl_audio/misc/io_info/video.cc`, `xam_input/voice/info.cc`, `xbdm_misc.cc`) with log demotions + null checks |

Fork-only new files (no upstream conflict surface): `dc3_*`, `cpu/milo_trace.*`,
`cpu/mtr_format.h`, `app/emulator_headless.*`, `app/xenia_headless_main.cc`,
`debug/dc3_gdb_rsp_protocol.h`, `testing/dc3_*`.

### 3.4 Mitigation cvars: flip to upstream default + per-title profile

| cvar | file:line | now | target default | profile opt-in |
|---|---|---|---|---|
| `soft_fault_unmapped_reads` | `cpu/mmio_handler.cc:25` | true | **false** | DC3, RB3DX (measure each) |
| `tolerate_null_guest_calls` | `x64_emitter.cc:50` | true | **false** | DC3 (decomp needs it; measure original), RB3 |
| `scanner_stop_on_invalid_run` | `ppc_scanner.cc:25` | true | **false** | measure |
| `io_force_synchronous_completion` | `xboxkrnl_io.cc:30` | true | **false** | DC3 if A/B shows need |
| `autoinit_critical_sections` | `xboxkrnl_rtl.cc:36` | true | **false** | DC3 decomp, RB3DX+RB3E |
| `rtl_leave_critical_section_force_release` | `xboxkrnl_rtl.cc:50` | true | **false** | same |
| `fault_spin_limit` | `exception_handler_posix.cc:34` | 4096 | **0** | RB3DX |
| `headless_skip_submission_wait` | `vulkan_command_processor.cc:46` | true | **false** | run scripts |
| `headless_capture_only_draws` | `vulkan_command_processor.cc:60` | true | **false** | run scripts |
| *(new)* `xam_enum_overlapped_nomorefiles_success` | `xam_enum.cc:87-97` | always | **false** | DC3, RB3 |
| *(new)* `dc3_min_thread_stack` | `xboxkrnl_threading.cc:392` | DC3 hardcoded | – | DC3 = 4 MB |
| *(new)* `headless_async_pipelines` | `vulkan_command_processor.cc:413-423` | always headless | **false** | run scripts |
| `dc3_clean_content_cache` | `dc3_hack_pack.cc:42` | true | **false** | none |
| `scanner_clamp_to_mapped_memory`, `posix_allocfixed_zero_commit`, `xnet_random_constant_fill=false` | | | keep; drop cvar when upstreamed | – |

The profile is applied in `titles::ApplyLaunchHooks` *before* the first resume, logs one line
per cvar it sets (`TITLE-PROFILE 373307D9: tolerate_null_guest_calls=true`), and never
overrides a value given explicitly on the command line. This makes Phase-2 Lane D
behaviour-neutral for DC3/RB3 by construction and restores upstream behaviour for every
other title.

---

## 4. Regression harness specification (Phase 0 -- build before any cleanup lands)

### 4.1 Principles

- Every run: private `--storage_root`, pinned `--config`, every non-default cvar explicit on
  the command line, no auto-probed host paths (pass `--dc3_nui_symbol_map_path` and
  `--dc3_nui_layout_fingerprint_cache_path` pointing at **pinned copies**).
- Record: binary path + xxh3, git sha + dirty count, sha256 of xex and every input, full argv,
  rc, wall time, `/proc/loadavg` at start/end plus a 10 s max sampler.
- Verdicts: **PASS / FAIL / INCONCLUSIVE**. INCONCLUSIVE if 1-min load exceeded 30 during a
  flow scenario (BASELINE: 0/6 reached game_screen at load 100-220; 4/9 boots hung), or the
  process died from an external signal. INCONCLUSIVE is never counted as FAIL or PASS.
- Interleave candidate and reference binary runs so host-load drift cancels.
  References: `pinned/xenia-headless.checked.2026-08-29.783a0830c92e9cbc` (RB3/627 era) and
  `~/tmp/dc3-oracle-run/bin/xenia-headless.baseline-45bf3c063e801035` (BASELINE.md era), plus
  a fresh Checked build of `integrate-2026-10` @ `62a667222` made in a **separate** worktree
  (not `/home/free/tmp/xenia-integrate`).
- Pinned asset directory: `~/tmp/xenia-harness-assets/` (not `/tmp`), content-named, with a
  `MANIFEST.sha256`. Never reuse a directory name for different content (the
  `/tmp/rb3tu5boot` name meant three different things over time).
- Log lines that are **harness contracts** (cleanup must preserve or update the analyzer in
  the same commit): `Thread Status Report (<ms>ms)… SIGSEGV=` (`emulator_headless.cc:1219`),
  `tw/td forced trap hit!` (`x64_emitter.cc:554`), `DC3 Script: wait_screen '<x>' SATISFIED`
  and `gpState=` (`nop_input_driver.cc:932, 724`), `RB3DX UI PROBE[n]` (`emulator.cc:1908`),
  `STREAM-CENSUS` (`emulator.cc:2277`), `DC3 DTA channel: installed on`.

### 4.2 Scenarios

| ID | Scenario | Command / inputs | Pass criteria | N | Budget |
|---|---|---|---|---|---|
| S0 | Static, no game | Build `xenia-app` **and** `xenia-headless` (Checked); run `xenia-core-tests`, `xenia-cpu-ppc-tests`; `tools/test_headless.sh`; dump toml from an empty storage root and diff vs pinned (cvar-default drift); ratchets: count of `0x373307D9\|0x45410914` outside `src/xenia/titles/` (today 65 incl. comments), `/home/free` literals in `src/` (today 3), `XELOGI` added in `gpu/` | Green; ratchets never increase; every default change is listed in the commit message | 1 | ~5 min after build |
| S1 | DC3 original, null GPU, ymca flow | `docs/dc3-oracle/run_dc3_oracle.sh <run> null 240` (`--dc3_nui_patch_layout=original --dc3_crt_skip_nui=true --stub_nui_functions=true --fake_kinect_data=true --scripted_input_file=$DC3/scripts/dc3-input-flows/xenia-ymca.txt --headless_timeout_ms=230000`) + pinned resolver paths. Inputs: `dc3-decomp/orig/373307D9/debug.xex` sha256 `2d5e4a32…4c29c4728`; `xenia-ymca.txt` sha256 `ea733eee…` | `analyze_run.py`: title ≤30 s; game_screen ≤60 s; `gpState=2&paused=0` samples ≥60; first `gpState=3` seen; rc 0 with `TIMEOUT: 230000ms reached`; max SIGSEGV 0. **Plus (new): `mFailThreadMsg` tripwire line absent once Lane B lands.** | 3 (≥2 PASS) | 3×4 min |
| S2 | DC3 DTA channel round trip | S1 + `--dc3_dta_channel=<run>/x.sock`; client `pinned/xchan.py` | `DC3 DTA channel: installed on`; first poll on guest thread `00000006`; `{+ 1 2}` → `=> 3`; `{no_such_func 1}` → `=> !! refused: script error`; then `{+ 5 5}` → `=> 10`; `{size {object_list main Object FALSE}}` returns an int; S1 milestones still pass | 2 (share S1 runs) | +0 |
| S3 | DC3 decomp layout, **627 forced traps** | `xenia-headless --target=<assets>/dc3-decomp-2026-08-24/default.xex --dc3_nui_patch_layout=auto --dc3_crt_skip_nui=true --break_on_debugbreak=false --headless_timeout_ms=120000 --gpu=null` + `--config=pinned/shared-xenia.config.toml.2026-08-29` + private storage + explicit resolver paths to pinned `symbols.txt` (pre-2026-09-30 copy from dc3-decomp git), fingerprint file, manifest. Source: MEMORY.md:23 / logs `~/tmp/dc3_nonreg_s63..s66.log` | `grep -c 'forced trap hit'` == **627** and LR histogram equal to `pinned/dc3_nonreg_s66.log` (0x83209804×271, 0x831FDB70×271, 0x831FE3B4×15, 0x832095B4×14, …); log has `layout=decomp`, the manifest-fingerprint-mismatch line, `TIMEOUT: 120000ms reached`; rc 0 | 2 | 2×2.2 min |
| S4 | RB3 clean TU5 → gameplay (+ song audio) | Dir `~/tmp/rb3-harness/tu5-clean-nodd/`: `default.xex` = `rb3-xenon/_tu5probe/clean/clean_tu5_nodd.xex` (sha256 `6d73992c…`; word @`0x82272E90` = `4bffd541`); symlinks `gen AvatarAwards nxeart charnames.zbm` → `/srv/torrents/games/arbys/rb3/` (beware self-loop `gen/gen`). `--gpu=null --protect_zero=false --break_on_debugbreak=false --headless_timeout_ms=300000 --rb3_tu5_app_run_direct --rb3_no_char_preview --rb3dx_offline_join --rb3dx_skip_calibration --rb3dx_autoconfirm_parts --rb3dx_ui_probe --local_user_count=2 --scripted_pad_subtypes=1,8 --rb3_stream_census [--rb3_mogg_key_table=<64B>]` | `RB3: app-run-direct installed`; UI probe passes `main_hub_screen`, `song_select_screen`, `part_difficulty_screen`, `tv3_*`; with key: `game_screen transState=0` and `STREAM-CENSUS mState=3 … 11 channels/11 receivers`; no `FAULT_LIVELOCK_ABORT`. **Until the mogg key is located:** menu-only criterion (reach `tv3_*` + transition to `game_screen` begins) | 2 | 2×5 min |
| S5 | RB3DX title → menu → gameplay | Dir `~/tmp/rb3-harness/rb3dx/`: `default.xex` = `/srv/torrents/games/arbys/rb3/default.xex` (RB3DX, sha256 `6639ce25…`) + same symlinks. `--protect_zero=false --rb3dx_skip_calibration --rb3dx_ui_probe --break_on_debugbreak=false --headless_timeout_ms=360000 --scripted_input="8s:A@0,…A every ~5 s"` | `main_hub_screen` (35-50 s), then `game_screen transState=0` (90-120 s); no livelock abort; rc 0 | 2 | 2×6 min |
| S6 | Title-hook inertness | (i) DC1 `milo-executable-library/dc1/TU0/default.xex` 60 s, all title cvars off; (ii) S1 with RB3 cvars on; (iii) S5 with DC3 cvars on | No `DC3[: ]`, `[dc3-debug]`, `RB3:`, `RB3DX`, `SI `, `TITLE-PROFILE` lines from the other title. **Known baseline failures today:** `emulator_headless.cc:1687-1763, 1954-1974` (thread-6 dumps for any title), `nop_input_driver.cc` screen-aware/unpause paths with any script file, `xam_nui.cc:41-48`. Recorded as baseline; must reach zero by end of Phase 2. | 1 each | ~8 min |

Total ≈ 45-60 min per candidate on a quiet host (load < 30; the box read 52-85 during this audit).

### 4.3 RB3 content provenance (rebuild recipe for the deleted `/tmp/rb3tu5boot`)

- Retail disc content: `/srv/torrents/games/arbys/rb3/` (`gen/` 10 main arks +
  `patch_xbox_0.ark` + hdrs, `AvatarAwards`, `nxeart`, `charnames.zbm`, `default.xex` = RB3DX,
  `default_vanilla.xex`).
- Clean TU5 executable: `/home/free/code/milohax/rb3-xenon/_tu5probe/clean/clean_tu5.xex` (sha256
  `941ecfde…`). If missing: `rb3-xenon/tools/xexp-apply/build/xexp-apply
  $SRC/default_vanilla.xex "/home/free/code/milohax/milo-executable-library/rb3/360 xexp/tu5/default.xexp"
  clean_tu5.xex` (recipe `rb3-xenon/docs/plans/clean-tu5-vs-rb3dx-divergence.md:47-51`).
- Dirty-disc bypass: `rb3-verify/patch/apply_dirtydisc_bypass.py` → `clean_tu5_nodd.xex`
  (word @`0x82516320` = `4e800020`). Already present in `_tu5probe/clean/`.
- **Missing:** the 64-byte `--rb3_mogg_key_table` value. Not stored anywhere found. Candidate
  source: `rb3-xenon/orig/45410914/band.exe` (the patched DX-lineage PE) at `0x82C76258`
  (unverified). Harness lane must locate or derive it, or S4 stays menu-only.
- SI variant (si34) additionally needs `rock-band-3-deluxe/out/xbox/gen/patch_xbox_{0.ark,hdr}`
  and the SI DLL; out of scope for the first harness.

### 4.4 Harness deliverables (Lane H)

`tools/harness/{run_scenario.sh, analyze_dc3.py, analyze_627.py, analyze_rb3.py, inertness.py,
static_ratchets.sh, loadgate.sh}`, `docs/fork/harness/README.md`, the pinned asset dir with
`MANIFEST.sha256`, and **two recorded baselines** (fresh integrate build ×N and the Aug-29
reference binary) checked in as small JSON summaries. Definition of done: every scenario
produces a machine verdict; S1-S5 PASS on the integrate baseline binary on a quiet host;
S6 records today's leak lines.

---

## 5. Lanes and ordering

File ownership is **disjoint within a phase**. Every lane works in its own worktree off the
current integrate tip (never `/home/free/tmp/xenia-integrate`, never the main checkout), commits
early, and lands with `git merge --no-ff` after rebasing.

### Phase 0 -- Lane H: harness (no `src/` changes)

- Owns: `tools/harness/**`, `docs/fork/harness/**`, `~/tmp/xenia-harness-assets/`.
- Done: §4.4. Also locate the mogg key or document why S4 is menu-only.
- Gate for everything after it.

### Phase 1 -- Lane A: titles scaffold + mechanical extraction (emulator.cc is the hotspot)

- Owns: `src/xenia/emulator.cc`, `emulator.h` (must stay upstream-identical), new
  `src/xenia/titles/**`, `src/xenia/crash_snapshot.*`, `src/xenia/premake5.lua`,
  `src/xenia/testing/premake5.lua`, `src/xenia/app/premake5.lua`, `app/xenia_headless_main.cc`
  (registration call only), and `git mv` of all five `src/xenia/dc3_*.{cc,h}` +
  `testing/dc3_nui_patch_resolver_test.cc` into `titles/dc3/`.
- Scope: **verbatim moves only** per §3.1/§3.2. No logic, default, or log-text change.
  Includes `title_ids.h`, `title_hooks` registry, an **empty** `title_profile`, probe-thread
  module, `dc3_dta_channel.cc` Linux-only filter (a build fix, not behaviour), and the
  `cpu::UnresolvedCallObserver` seam so `x64_emitter.cc` stops including a DC3 header (the
  only `cpu/` edit allowed in this phase).
- Done: `emulator.cc` ≤ ~1,000 lines, its diff vs upstream ≤ ~150 lines; `xenia-app` links no
  title code (`nm` shows no `Dc3`/`Rb3` symbols); S0-S6 results equal to the Phase-0 baseline,
  **and** the sorted set of `DC3:`/`RB3`/override-install log lines identical to baseline for
  S1, S3, S4, S5.

### Phase 2 -- four parallel lanes (after A lands)

| Lane | Owns (exclusive) | Work | Definition of done | Regression requirement |
|---|---|---|---|---|
| **B -- DC3 semantics** | `src/xenia/titles/dc3/**`, `src/xenia/hid/nop/**` | 1) Behaviour-preserving deletions first (decomp pack → archive tag; dead items in §2.1/§2.2: `dc3_guest_overrides`, `Dc3NuiReturn1Extern`, ULTRA block, gameplay probe, shadowed 0x829C2790 stub, `BinkMovieImpl::Ready` after one confirming run, `include_ppc_words`, ReadCacheStream diag). 2) Content wipe default false. 3) Resolver: drop absolute auto-probes; original layout uses compiled table + signature. 4) nop_input split: generic player + `ScriptedInputTitleAdapter`; delete beat drive B and attract force; move nudge behind `--dc3_force_unpause`. 5) Main-thread hook; autonav behind `--dc3_headless_autonav`; LoadSong repair main-thread-only. 6) Debug::Fail per §2.4. 7) Beat-drive decision per §2.4. 8) IK telemetry address fix. 9) DTA channel: lazy alloc, owned thread. | All §2.1/§2.2/§2.3 DC3 rows at their target bucket; no guest writer outside `titles/dc3/`; `run_dc3_oracle.sh` updated with explicit new cvars; BASELINE.md re-measured and rewritten (new patch manifest). | Steps 1-3: S1/S2 equal to baseline (S3 retired in the same commit as the decomp pack, with a note). Steps 4-9 are **intentional oracle changes**: each is its own commit with S1×3 before/after, and the new behaviour becomes the baseline only after review. S6 DC3 leaks → 0. |
| **C -- RB3 + headless app** | `src/xenia/titles/rb3/**`, `src/xenia/app/emulator_headless.{cc,h}`, `src/xenia/debug/**` | Delete every bucket-4 RB3 row (§2.1: splash_unwedge, loadmgr_unbudget, overlapped_scan, hash_poke, clamp_alloc, si_probe, si_selftest, approach (b), forensics dumps, mount_update, offline-join sweep); autopilot gets its own gate; ui_probe read-only + XELOGD + truthful help; skip-calibration one-shot; replace raw host Protect with `heap->Protect`; re-verify `rb3dx_offline_join` / `rb3_no_char_preview` need (one run each without). Headless app: delete thread-6 DC3 block (1320-2003) and `ReportCrash`; move RSP server to `debug/gdb_rsp/` + rename cvars (keep old names as deprecated aliases one release); gate SIGUSR2 sampler; flush logger before `_Exit`; `gpu` cvar rename. | RB3 cvar count reduced to the surviving set; `emulator_headless.cc` ≤ ~700 lines; no RB3 writer documented as read-only. | S4, S5 PASS at baseline milestones; S6 (RB3 leaks + thread-6 dumps) → 0; S1 unchanged (thread status report line preserved). |
| **D -- core cpu / base / memory / kernel** | `src/xenia/cpu/**` (except the Phase-1 seam), `src/xenia/base/**`, `src/xenia/memory.*`, `src/xenia/kernel/**`, `src/xenia/vfs/**`, `src/xenia/apu/**`, `third_party/half`, and `src/xenia/titles/title_profile.*` (handed over from A) | 1) Fill `title_profile` with today's effective values for DC3 and RB3 (no-op step). 2) Flip the §3.4 defaults. 3) Fix bucket-5 kernel/xam rows (ObReferenceObject, XUID mask, CheckPrivilege, NuiGetDeviceStatus gate, MmFreePhysicalMemory, CS-diag re-init, detached reaper, eventfd leak, `fault_spin_limit` semantics, classifier gate, missed SysV sites, ppc_hir_builder silent clamp, C18). 4) Delete bucket-4 rows (overlapped writeback, force_zero_commit, fast-CAS, clflush skip, GetLastSuspendHostRip, protect_zero page-0 block, residues, log reverts) and demote hot logs. 5) Prepare upstream PR branches off `upstream/master` for groups A-G (hunks per §2.5; hand-rebuild hunks from mixed commits; strip the `Co-Authored-By` trailer in `3af392be6` when rebuilding). | Every §2.5 cpu/kernel/base row at target; S0 ratchet: no all-title default differs from upstream except the documented UP list; PR branches exist (pushing is a user decision). | Step 1-2 must leave S1-S5 identical (profile restores fork behaviour; `TITLE-PROFILE` lines appear). S6(i) DC1 now runs with upstream defaults -- record its new baseline. Each bucket-5 fix: full S1-S5. Run `xenia-cpu-ppc-tests` + `threading_test` every commit. |
| **E -- gpu + build + docs** | `src/xenia/gpu/**`, `src/xenia/ui/**`, `src/xenia/kernel/premake5.lua`, `src/xenia/gpu/**/premake5.lua`, `xenia-build`, `.gitignore`, `docs/**`, `tools/**` (except `tools/harness`), `rb3-verify/**`, `CLAUDE.md` | GPU: flip `headless_skip_submission_wait`/`headless_capture_only_draws` (run scripts opt in -- coordinate with Lane H's scripts via a one-line PR), gate `SetHeadlessMode`, delete per-draw `steady_clock` + per-frame/per-texture `XELOGI`, pipeline cache save-at-shutdown to storage root, delete dead headless GPU projects + `XE_HEADLESS_BUILD` branches in graphics_system, rename `dc3_*` render cvars, move `stub_nui_functions`/`fake_kinect_data` out of `gpu_flags` (coordinate with B: B defines them in `dc3_flags.cc`, E deletes them from `gpu_flags.cc` in the same merge window). Docs: consolidate to `docs/fork/` per §2.7/§3.1; stale banners; rewrite CLAUDE.md. Tools: archive bucket-4 scripts; move rb3-verify scripts to `tools/rb3/`. **rb3-verify binaries: prepare the history-rewrite command but do not run it** (user decision; affects every branch/worktree). | No `XELOGI` on a per-draw/per-frame/per-texture path; no unlinked premake project; `docs/fork/README.md` indexes every fork doc; no doc claims a decomp-only workflow as current. | S1 (null) unchanged; a Vulkan run of S1 (`--gpu=vulkan` + capture cvars) produces frames at the same capture indices as baseline; S5 under Vulkan reaches `main_hub`. Windowed `xenia-app` builds. |

### Phase 3 -- integration (single lane, after Phase 2)

- Replace `XE_HEADLESS_BUILD` with runtime `cvars::headless` / `display_window_ == nullptr`;
  delete `xenia-core-headless` and `xenia-kernel-headless` (touches `src/xenia/premake5.lua`,
  `kernel/premake5.lua`, `emulator.cc` ifdefs, `xam_ui.cc`, `xam_nui.cc` -- hence its own
  phase). Note `emulator.cc:6630-6633, 6694-6697` set `is_headless` from the define: Lane B's
  code must read the runtime value.
- Flip S0's literal ratchet to an allow-list (title IDs only under `titles/`).
- Full S0-S6 ×N; re-record BASELINE.md; tag `fork-cleanup-2026-10`.

### Ordering summary

```
H (harness) ──► A (emulator.cc extraction, verbatim) ──► { B dc3 | C rb3+app | D core | E gpu+build+docs } ──► Phase 3
```

Within Phase 2: B's step 6 (Debug::Fail) depends on B's step 5 (main-thread hook). D's step 1
must land before D's step 2. E's `gpu_flags` cvar move and B's `dc3_flags` definition land in
one coordinated merge window.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| **The harness itself is load-sensitive** (wall-clock script `+N`=N×50 ms, NUI-call thresholds, 33 ms worker): a cleanup can look like a regression under load. | Load gate → INCONCLUSIVE; interleaved A/B; N≥3 for S1; never conclude from a single run. Longer term, autonav on the main-thread hook makes the flow frame-driven. |
| **Intentional oracle changes (Lane B steps 4-9) move the baseline.** A green S1 after them is a new instrument, not the old one. | Each is its own commit with before/after S1×3; BASELINE.md rewritten with the new patch manifest; any golden recorded before Lane B is labelled pre-fix. |
| **Flipping defaults (Lane D) can silently break DC3/RB3** if the profile misses a cvar or applies it too late (`tolerate_null_guest_calls` is read at JIT time; `soft_fault_unmapped_reads` at fault time). | Profile applied before first resume; logs every value; D step 1 lands as a no-op with S1-S5 identical before any flip. |
| **The 627-trap bar depends on the decomp layout pack that Lane B deletes,** and on mutable inputs (`symbols.txt` changed 2026-09-30, `default.xex` overwritten by any `build_xex.py` run, shared toml rewritten by any un-isolated run). | Pinned copies (this audit saved the toml, binary and reference log). Keep S3 through Phase 1 and Lane D as a core-change tripwire; retire it in the same commit that deletes the pack, with the decision recorded. If the user wants the bar kept, move the pack to `titles/dc3/decomp/` instead of deleting. |
| **Root-cause hypothesis for the Bink wrong-thread FAIL is inferred, not measured.** If the FAIL is not caused by worker-thread Executes, step (a) of §2.4 will not stop it. | Log host thread id at every `processor->Execute` before the change; tripwire (b) catches it either way; option (c) is the fallback. |
| **Code-read-only findings** (`fault_spin_limit` parking an MMIO poll loop; raw host read at `emulator_headless.cc:1678-1681`; `vulkan_command_processor.cc:5317-5321` guest write-back; `XamUserGetXUID` regression impact) are derived, not measured. | Each fix lane reproduces before fixing; none is a prerequisite for another lane. |
| **History rewrite for `rb3-verify` binaries** breaks every worktree/branch based on integrate (xi-bis-a/b, xenia-integrate, dc3-oracle, the main checkout's `frag-alloc-trace`). | User decision; do it once, at a quiet moment, before any push; Lane E only prepares the command. |
| **Upstream drift**: upstream is at `95a5c3ee2` (2026-02-18 per its date); every month of fork work enlarges conflicts in HOOK-verdict files. | Phase 1 first (shrinks `emulator.cc` conflict surface ~85%); Lane D's PR branches remove the UP list permanently once merged upstream. |
| **Another agent is bisecting in `/home/free/tmp/xenia-integrate`.** | No lane builds or runs there; the harness never uses its binary as a reference. |
| **Paced nop audio changed DC3 behaviour since BASELINE.md** (render callbacks now fire). Beat-drive and `HamAudio::IsReady`/`XMAHAL` decisions depend on an unmeasured effect. | Lane B measures it before deciding (§2.4 item 2). |
| **Windowed `xenia-app` currently runs DC3 hooks too** (no `XE_HEADLESS_BUILD` guard around title blocks, content wipe included). Moving title code out of `xenia-core` changes that. | Intended. Document in the Phase-1 merge message; S0 builds both binaries. |
