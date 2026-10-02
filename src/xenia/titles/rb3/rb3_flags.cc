/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914) cvars moved out of emulator.cc (NOT upstream).
 *
 * Moved verbatim (Phase 1, FORK_CLEANUP_PLAN.md).
 ******************************************************************************
 */

#include "xenia/titles/rb3/rb3_flags.h"

DEFINE_bool(rb3_mount_update, false,
            "RB3 boot-to-menu experiment: mount update: at the disc dir so RB3 "
            "finds update:\\gen\\patch_xbox.hdr (title-update content). Default "
            "OFF (inert for DC3 and normal runs).",
            "RB3");
DEFINE_bool(
    rb3dx_alloc_probe, false,
    "RB3DX (title 0x45410914) DIAGNOSTIC, default off: bytepatch guest "
    "MemAlloc@0x827BCD38 with an observe-and-continue trampoline that logs "
    "size/align/caller-LR + a guest stack walk for allocation sizes with a "
    "non-zero top byte (main_hub OOM corrupted-size investigation). Pure "
    "observer; no guest state is modified by the handler.",
    "CPU");
DEFINE_string(
    rb3dx_alloc_trace_path, "",
    "RB3DX (title 0x45410914) DIAGNOSTIC, default off (empty): write a 32-byte "
    "binary record for EVERY guest MemAlloc@0x827BCD38 entry (tag 1: "
    "size/align/caller-LR/caller-SP), for its RETURN (tag 2: the pointer it "
    "handed back, joined by seq), and -- with --rb3dx_free_trace -- for every "
    "MemFree@0x827BC430 entry (tag 3: pointer, block header, caller-LR). "
    "Offline attribution of heap-\"main\" fragmentation by call site. Text "
    "logging is far too slow at the real call rate (~876 allocs/s mean, 1900/s "
    "peak); this is a buffered binary sink. Implies the __savegprlr_23 probe "
    "override. Title-gated so DC3-inert.",
    "CPU");
DEFINE_bool(
    rb3dx_free_trace, true,
    "RB3DX: with --rb3dx_alloc_trace_path, also trace MemFree@0x827BC430 (via "
    "an exact override of its prologue helper __savegprlr_26 @0x82829250, "
    "filter lr==0x827BC438) so allocations can be paired with their frees and "
    "block lifetimes attributed. No effect without the trace path.",
    "CPU");
DEFINE_bool(
    rb3dx_stack_trace, true,
    "RB3DX: with --rb3dx_alloc_trace_path, also emit a tag-4 record per "
    "MemAlloc carrying the next FOUR guest return addresses above the "
    "immediate caller, walked from the stack backchain (each frame's saved LR "
    "is at [caller_sp - 8] under the __savegprlr idiom). Needed because the "
    "top allocation site is XMemAlloc -- a shim -- so the immediate caller LR "
    "names the allocator, not the subsystem. Fully range-checked; unresolvable "
    "frames are reported as 0. No effect without the trace path.",
    "CPU");
DEFINE_bool(
    rb3dx_ret_trace, true,
    "RB3DX: with --rb3dx_alloc_trace_path, also capture MemAlloc's RETURN "
    "VALUE (the allocated pointer -- which says where in the arena the block "
    "landed, i.e. FirstFit-bottom vs LastFit-top) by overriding the epilogue "
    "helper __restgprlr_23 @0x82829294 that MemAlloc tail-branches through, "
    "matching on (r1 == the entry SP recorded for this thread AND the "
    "about-to-be-restored LR == that call's caller LR). Set false if the "
    "epilogue override destabilises the guest; the alloc-entry trace still "
    "works without it, only pointer-exact alloc/free pairing is lost.",
    "CPU");
DEFINE_bool(
    rb3dx_ui_probe, false,
    "RB3DX (title 0x45410914) DIAGNOSTIC, default off: passively sample the "
    "guest UI transition state every ~2s from a host thread (BandUI/UIManager "
    "@0x82DFD2B0: transition state + current/transition screen names, the "
    "transition screen's per-panel load states, and the saveload_mgr/net_sync "
    "objects found via ObjectDir::sMainDir @0x82E054B8). Read-only guest "
    "memory access; no hooks, no patches; title-gated so DC3-inert. For the "
    "main_hub load-stall investigation.",
    "CPU");
DEFINE_bool(
    rb3_loadmgr_unbudget, false,
    "RB3 TU5/DX (title 0x45410914), default off, requires --rb3dx_ui_probe: "
    "poke TheLoadMgr's per-frame Poll() time budget (the 10.0f period/split "
    "pair at 0x82E06E48/4C) to 1e30 -- the value the game itself uses inside "
    "PollUntilEmpty() for unbudgeted synchronous drains. Under Checked-config "
    "xenia a budgeted Poll() pass can exhaust its 10ms before the front "
    "loader's first state step (DirLoader::PollLoading checks CheckSplit() "
    "BEFORE advancing), starving the front loader forever: the clean-TU5 "
    "boot freeze at the char-cache extras milos. Guest-data poke only; no "
    "code patches. Title-gated => DC3-inert.",
    "CPU");
DEFINE_bool(
    rb3_splash_unwedge, false,
    "RB3 TU5 (title 0x45410914), default off, requires --rb3dx_ui_probe: break "
    "the clean-TU5 boot deadlock in Splash::EndSplasher. App::App's EndSplasher "
    "calls SetImmutableState(kTerminating) which no-ops when a Suspend is "
    "in-flight (mState<kResumed), then blocks in WaitForState(kTerminated) "
    "forever while the SplashThread worker is parked in WaitForState(kResuming). "
    "This finds the live Splash (tid=6 stack) and drives the state machine to "
    "completion (resume the worker, then terminate it) so App::App returns and "
    "the frame loop resumes pumping the loader. Guest-memory poke + NtSetEvent "
    "only. Title-gated => DC3-inert.",
    "CPU");
DEFINE_bool(
    rb3_overlapped_scan, false,
    "RB3 (title 0x45410914), default off, requires --rb3dx_ui_probe: scan the "
    "guest heap for OVERLAPPED structs stuck at Internal==STATUS_PENDING "
    "(0x103) to test the clean-TU5 loader-freeze hypothesis (AsyncFileWin::"
    "_ReadDone spins on an OVERLAPPED xenia never clears). Read-only. "
    "Title-gated => DC3-inert.",
    "CPU");
DEFINE_string(
    rb3_mogg_key_table, "",
    "RB3 TU5 (title 0x45410914), default empty (disabled): 64 hex bytes to "
    "write over the mogg key-encryption table at 0x82C76258. Retail RB3 "
    "decrypts .mogg song audio by installing a table key via XeKeysSetKey and "
    "running XeKeysAesCbc. On hardware SetKey first DEOBFUSCATES the supplied "
    "key using a console key we do not have, so the shipped (obscured) table "
    "cannot work under emulation and the song stream never leaves kInit. "
    "Passing the deobscured equivalent here makes the decrypt come out right. "
    "No keys ship with xenia -- read the 64 bytes at 0x82C76258 out of an "
    "already-deobscured RB3 image if you have one. Accepts whitespace between "
    "bytes. Title-gated => DC3-inert.",
    "CPU");
DEFINE_bool(
    rb3_stream_census, false,
    "RB3 (title 0x45410914), default off, requires --rb3dx_ui_probe: sweep the "
    "guest heap for live StandardStream objects (vtable 0x820F6A8C) and report "
    "each one's mState(+0x14), receivers vector(+0x20..+0x24) and channel "
    "array(+0x78..+0x80). The song-audio gate is MasterAudio::IsLoaded "
    "(0x8277B6E8), which tail-calls mStream->IsReady() = (mState == kReady/2). "
    "mState is written in exactly three places: the ctor stores kInit(0), "
    "0x82704880 stores kBuffering(1), and PollStream stores kReady(2). The "
    "kBuffering store is UNCONDITIONAL within 0x827046B8, and that same "
    "function is the only code that fills the receivers vector from the "
    "channel array -- so 'mState==0 with an empty receivers vector' is proof "
    "that 0x827046B8 was never called, rather than that it ran and failed. "
    "Finding streams by vtable instead of by walking the load queue lets this "
    "run against an image that DOES reach playback, making the two directly "
    "comparable. Read-only. Title-gated => DC3-inert.",
    "CPU");
DEFINE_bool(
    rb3_tu5_hash_poke, false,
    "RB3 TU5 (title 0x45410914), default off, requires --rb3dx_ui_probe: "
    "rewrite the exe's embedded ark-integrity SHA1s for the two dtbs the "
    "clean-TU5 boot patch modifies (ui.dtb, splash.dtb) so the anti-tamper "
    "check stops silently quitting the game during App::App. Equivalent to "
    "arkhelper patchcreator's exePath hash patching, applied in guest memory. "
    "Title-gated => DC3-inert.",
    "CPU");
DEFINE_bool(
    rb3_no_char_preview, false,
    "RB3 TU5 (title 0x45410914), default off: no-op CharSync::UpdateCharCache "
    "(0x82564698) via a guest-function override so the band-member preview "
    "char-cache extras (world/shared/extras/male_extras0N.milo) are never "
    "queued. Those loaders sit kLoadFront at the head of the single main-thread "
    "load FIFO and, when one stalls, head-of-line-block the splash_screen "
    "panels behind them -- freezing the whole cooperative-loader frame loop "
    "~13s into boot (clean-TU5 wedge). Byte-for-byte equivalent to the rb3 "
    "native port's RB3_NO_CHAR_PREVIEW early-return; previews are cosmetic and "
    "off the boot-to-menu path. Title-gated + default-off => DC3-inert.",
    "CPU");
DEFINE_bool(
    rb3_tu5_app_run_direct, false,
    "RB3 TU5 (title 0x45410914), default off: enter the real frame loop "
    "directly. Retail App::Run (0x822703D0) installs an unhandled-exception "
    "filter (0x822703A8) and then deliberately writes to guest address 0 "
    "(`stw r10,0(0)` at 0x822703FC); on hardware the access violation invokes "
    "the filter, and the FILTER calls App::RunWithoutDebugging (0x82270080) -- "
    "the actual unconditional frame loop (SystemPoll + all subsystem polls + "
    "TheUI.Poll + Draw). Under --protect_zero=false (required for the separate "
    "0x8275026C page-0 read) the null store succeeds silently, the filter "
    "never runs, App::Run returns, and main falls into App::~App -> clean "
    "teardown ('the flow-quit'). Byte-patch main's `bl 0x822703D0` at "
    "0x82272E90 into `bl 0x82270080` -- the exact one-word patch the patched "
    "TU5 image (RB3DX lineage) already ships at that site. Supersedes "
    "--rb3_tu5_loop_main/--rb3_tu5_hold_main. Title-gated + default-off => "
    "DC3-inert.",
    "CPU");
DEFINE_uint64(
    rb3dx_si_claim_anchor, 0,
    "RB3DX (title 0x45410914), default 0 (off), requires --rb3dx_ui_probe: "
    "guest VA of the RB3Enhanced.dll SI claim-table anchor (the lis/addi "
    "base register in SIInstallClone; wt-integration build: 0x84055FD8, "
    "decoded from the packed DLL with capstone). Layout from "
    "SameInstrumentHooks.c: gClaims[] {track,count} pairs at +0 stride 8, "
    "gImpls[] at +0xC0 stride 0xC, gClaimCount at +0x1C8, gImplCount at "
    "+0x1CC. When set, the ui probe logs these each sample -- the "
    "machine-readable twin evidence (two players on one track => a claim "
    "with count 2 and implCount 2). Read-only. Title-gated => DC3-inert.",
    "CPU");
DEFINE_bool(
    rb3dx_autoconfirm_parts, false,
    "RB3DX (title 0x45410914), default off, requires --rb3dx_ui_probe: "
    "closed-loop autopilot for the two-player part/difficulty confirm. "
    "Fixed-time --scripted_input presses cannot hit the part_difficulty_"
    "screen window reliably (menu/ark load times vary tens of seconds "
    "between runs; measured si6..si10). When the probe sees "
    "song_select_screen it injects A on pad 0 (advance/pick song); on "
    "part_difficulty_screen it alternates A on pad 1 / pad 0 each ~2s "
    "sample so both players confirm part and difficulty regardless of when "
    "the cards appear. Injection goes through the nop HID driver's "
    "per-pad InjectButtonPress; no guest writes. Title-gated => DC3-inert.",
    "CPU");
DEFINE_int32(
    rb3dx_autoconfirm_p2_up, 0,
    "RB3DX autopilot: press DPAD-UP on pad 1 for the first N samples at "
    "part_difficulty_screen before the A confirms, to navigate P2's CHOOSE "
    "INSTRUMENT list off the default first-free part (e.g. 1 = select the "
    "entry above BASS -- GUITAR when the same-instrument un-grey is armed).",
    "CPU");
DEFINE_bool(
    rb3dx_offline_join, false,
    "RB3DX (title 0x45410914), default off: complete the offline single-local-"
    "host user join synchronously so the boot advances past splash_screen to "
    "main_hub. Overrides guest NetSession::IsHost() @0x823CECE0 to return true "
    "for the offline case (mirrors the RB3 native port's IsHost()==true), which "
    "sends NetSession::AddLocalUser @0x823D2468 down its host branch and fires "
    "AddUserResultMsg(1) at once instead of an online request/response that "
    "never round-trips headless. Title-gated so DC3-inert.",
    "CPU");
DEFINE_bool(
    rb3dx_skip_calibration, false,
    "RB3DX / RB3 TU5 (title 0x45410914), default off: make first-boot skip the "
    "interactive first_time_calibration -> cal_audio_screen A/V-latency "
    "calibration (uncompletable headless with null audio + fixed-time input) so "
    "the splash advances straight to main_hub. A host thread resolves the guest "
    "profile_mgr singleton via the main-dir name hash and sets "
    "ProfileMgr::mHasSeenFirstTimeCalibration @+0x54 = 1 (the splash "
    "kSplashScreen_EndOvershell condition {!{profile_mgr "
    "get_has_seen_first_time_calibration}} then routes to main_hub_screen). "
    "Single guest byte written; title-gated so DC3-inert.",
    "CPU");
DEFINE_bool(
    rb3dx_clamp_alloc, false,
    "RB3DX / RB3 TU5 (title 0x45410914), default off: mitigate the "
    "emulation-induced main_hub-load OOM race (doc-09). At MemAlloc's "
    "__savegprlr_23 call site (lr==0x827BCD40) the request size occasionally "
    "arrives as 0xGG001524 -- a correct small size in the low 24 bits with a "
    "garbage NON-ZERO top byte (an uninitialized guest read that is zero on "
    "real hardware but garbage under Xenia). When the top byte is non-zero AND "
    "the low 24 bits are < 1 MiB (the documented small-alloc signature), clamp "
    "r3 to its low 24 bits so MemHeap::Alloc gets the intended size instead of "
    "OOMing. Legit multi-MiB allocations (low-24 >= 1 MiB) are never touched. "
    "Reuses the alloc-probe __savegprlr_23 override; title-gated so DC3-inert.",
    "CPU");
DEFINE_bool(
    si_probe, false,
    "RB3DX / RB3 TU5 (title 0x45410914), default off: passively log, from a "
    "host thread, the runtime bytes of the static same-instrument (SI) patch "
    "in guest RAM -- the enable flag @0x82C8AAA0 (expect 1), the IsActive "
    "detour word @0x826684C0 (expect 0x48621BC0 = b 0x82c8a080), and the first "
    "cave-stub words @0x82C8A080 / @0x82C8A000. Read-only; proves the static "
    "XEX cave survived decrypt/load and whether a runtime writer zeroes the "
    "flag. Title-gated so DC3-inert.",
    "CPU");
DEFINE_bool(
    si_selftest, false,
    "RB3DX / RB3 TU5 (title 0x45410914), default off: once during boot, "
    "synthetically invoke OvershellPartSelectProvider::IsActive @0x826684C0 "
    "(the same-instrument detour site) with a crafted empty `this` and log the "
    "return value, to test whether Xenia's JIT executes the .data code cave and "
    "the SI logic fires (r3==1 => cave executed + SI active; r3==0 => inert). "
    "Runs on a live guest-thread context (reuses the alloc-probe "
    "__savegprlr_23 override) with full register snapshot/restore; title-gated "
    "so DC3-inert.",
    "CPU");
DEFINE_bool(
    si_hook_verify, false,
    "RB3DX / RB3 TU5 (title 0x45410914), default off: read-only host-thread "
    "verifier for the RB3Enhanced-DLL same-instrument GAMEPLAY hooks (H1 "
    "ProcessConfig @0x8276FA08, H2 RecalcGemList @0x82794740). Samples the "
    "first instruction word at each site and decodes it: if it is a `b` "
    "(primary opcode 18) whose target lands in DLL space [0x84000000,"
    "0x84040000) and within +/-32MB of the site, the RB3E HookFunction detour "
    "is INSTALLED (PASS); if it is still the stock prologue 0x7D8802A6 "
    "(mflr r12), the DLL is not loaded / hooks not installed (NEGATIVE "
    "control). Also scans for a loaded user module based at 0x84000000. "
    "Read-only, no writes; title-gated so DC3-inert. Works regardless of HOW "
    "the DLL got mapped: verified control matrix (2026-07-09) -- stock TU5 "
    "reads 0x7D8802A6 (mflr r12); the dead static-.data-cave build reads a b "
    "into 0x82C8xxxx (correctly flagged NON-DLL); the RB3Enhanced.dll build "
    "must read a b into [0x84000000,0x84040000) = PASS. To load the DLL in "
    "Xenia, KernelState::LoadUserModule(\"game:\\\\RB3Enhanced.dll\") maps it "
    "at its preferred base 0x84000000 and runs its entry (a --si_load_dll cvar "
    "wiring this is the next harness step, pending the packed DLL artifact).",
    "CPU");
DEFINE_bool(
    si_load_dll, false,
    "RB3DX / RB3 TU5 (title 0x45410914), default off: load the produced "
    "RB3Enhanced.dll (game:\\RB3Enhanced.dll) at its preferred base 0x84000000 "
    "via KernelState::LoadUserModule(call_entry=false), then invoke the "
    "self-contained SI hook installer InitSameInstrument @0x8402DFA8 on the "
    "live boot guest-thread (reuses the MemAlloc __savegprlr_23 override, full "
    "register snapshot/restore). InitSameInstrument's inlined RB3E HookFunction "
    "rewrites the first instruction of the four SI target sites to a `b` into "
    "DLL space -- crucially H1 PlayerTrackConfigList::ProcessConfig @0x8276FA08 "
    "(kills the vector[-1] track-number-reuse crash) and H2 "
    "TrackWatcherImpl::RecalcGemList @0x82794740 (per-watcher gem-list clone). "
    "call_entry=false deliberately skips RB3E's full CRT/DllMain boot (its "
    "socket/event init is unsafe headless); only the deterministic hook "
    "installer runs. Fires once well into boot. Pair with --si_hook_verify to "
    "observe the installed detours. Title-gated + default-off => DC3-inert.",
    "CPU");
DEFINE_uint64(
    si_init_va, 0,
    "RB3DX / RB3 TU5 (title 0x45410914), default 0 (disabled): guest VA of the "
    "from-source RB3Enhanced.dll's InitSameInstrument() entry (void)(void), "
    "taken from the DLL's link map (Phase-2 mapdeploy.json initVA, e.g. "
    "0x84019830). When --si_load_dll is set and this is non-zero, Xenia does NOT "
    "host-emulate the four HookFunction detours with hardcoded old-DLL targets; "
    "instead it calls InitSameInstrument on the live boot guest-thread (via the "
    "__savegprlr_23 override + full register snapshot/restore), so the DLL's own "
    "HookFunction computes the detour targets and rewrites the four SI game "
    "sites. This is the from-source-DLL path -- addresses come from the DLL, not "
    "constants. Title-gated + default-off => DC3-inert.",
    "CPU");
DEFINE_uint64(
    si_force_allow_va, 0,
    "RB3DX / RB3 TU5 (title 0x45410914), default 0 (disabled): guest VA of the "
    "from-source RB3Enhanced.dll's config.AllowSameInstrument flag (Phase-2 "
    "mapdeploy.json allowFlagVA = config base + 0x50, e.g. 0x84829590). Because "
    "--si_load_dll uses LoadUserModule(call_entry=false), the DLL's DllMain/ini "
    "load never runs, so AllowSameInstrument stays 0 and the installed SI hooks "
    "run pass-through (install verified but behaviorally inert). When non-zero, "
    "Xenia pokes 1 to this VA right after the DLL loads, arming the hook bodies. "
    "REQUIRED for behavioral (Phase-5) runs; harmless for install-only (Phase-3) "
    "verification. Title-gated + default-off => DC3-inert.",
    "CPU");
DEFINE_string(
    si_hook_vas, "",
    "RB3DX / RB3 TU5 (title 0x45410914), default empty: comma-separated FOUR "
    "from-source RB3Enhanced.dll hook VAs, in fixed site order "
    "IsActiveHook,ResolveWaitStatesHook,ProcessConfigHook,RecalcGemListHook "
    "(hex, e.g. 0x84027B88,0x84027BC8,0x84027E30,0x84028FC8). Read from the "
    "current build's K-link/RB3Enhanced.map -- the hook VAs move on EVERY DLL "
    "rebuild, so the 2026-07 kFromSrcHooks constants in approach (b) are stale "
    "the moment the DLL is relinked. When set, overrides those constants; when "
    "empty, the retired 2026-07 constants are used unchanged (only correct for "
    "the July fromsource.dll artifact). The four game-site addresses are "
    "title-side and stable. Title-gated + default-off => DC3-inert.",
    "CPU");
