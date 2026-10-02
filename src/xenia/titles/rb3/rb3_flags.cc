/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914) cvars (NOT upstream).
 *
 * Every cvar is title-gated and default-off. Categories are unchanged from
 * when these lived in emulator.cc, so existing config files still apply.
 *
 * Removed in the 2026-10 cleanup (FORK_CLEANUP_PLAN.md Lane C), each for a
 * hypothesis that was refuted or solved in WORKSTREAM-rb3-on-xenia-bringup.md:
 * rb3_mount_update, rb3_splash_unwedge, rb3_loadmgr_unbudget,
 * rb3_overlapped_scan, rb3_tu5_hash_poke, rb3dx_clamp_alloc, si_probe,
 * si_selftest, si_hook_vas (SI approach (b)).
 ******************************************************************************
 */

#include "xenia/titles/rb3/rb3_flags.h"

// --- workarounds the documented recipes use ---------------------------------

DEFINE_bool(
    rb3_tu5_app_run_direct, false,
    "RB3 TU5 (title 0x45410914), default off: enter the real frame loop "
    "directly. Retail App::Run (0x822703D0) installs an unhandled-exception "
    "filter (0x822703A8) and then deliberately writes to guest address 0 "
    "(`stw r10,0(0)` at 0x822703FC); on hardware the access violation invokes "
    "the filter, and the FILTER calls App::RunWithoutDebugging (0x82270080) -- "
    "the actual unconditional frame loop. Under --protect_zero=false "
    "(required for the separate 0x8275026C page-0 read) the null store "
    "succeeds silently, the filter never runs, App::Run returns, and main "
    "falls into App::~App -> clean teardown ('the flow-quit'). Byte-patches "
    "main's `bl 0x822703D0` at 0x82272E90 into `bl 0x82270080` -- the "
    "one-word patch the RB3DX-lineage image already ships at that site. "
    "Title-gated.",
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
    "bytes. Title-gated.",
    "CPU");
DEFINE_bool(
    rb3_no_char_preview, false,
    "RB3 TU5 (title 0x45410914), default off: no-op CharSync::UpdateCharCache "
    "(0x82564698) via a guest-function override so the band-member preview "
    "char-cache extras (world/shared/extras/male_extras0N.milo) are never "
    "queued. Written for the clean-TU5 splash head-of-line stall theory "
    "(WORKSTREAM §8o), which §8p refuted; still part of the §8w/§8x recipe. "
    "Equivalent to the rb3 native port's RB3_NO_CHAR_PREVIEW early-return; "
    "previews are cosmetic. Title-gated.",
    "CPU");
DEFINE_bool(
    rb3dx_offline_join, false,
    "RB3DX / RB3 TU5 (title 0x45410914), default off: complete the offline "
    "single-local-host user join synchronously. Overrides guest "
    "NetSession::IsHost() (located by instruction signature) to return true "
    "for the offline case (mirrors the RB3 native port's IsHost()==true), "
    "which sends NetSession::AddLocalUser down its host branch and fires "
    "AddUserResultMsg(1) at once instead of an online request/response that "
    "never round-trips headless. Title-gated.",
    "CPU");
DEFINE_bool(
    rb3dx_skip_calibration, false,
    "RB3DX / RB3 TU5 (title 0x45410914), default off: make first boot skip the "
    "interactive first_time_calibration -> cal_audio_screen A/V-latency "
    "calibration (uncompletable headless with null audio) so the splash "
    "advances straight to main_hub. A host thread resolves the guest "
    "profile_mgr singleton via the main-dir name hash and sets "
    "ProfileMgr::mHasSeenFirstTimeCalibration to 1 (the splash "
    "{!{profile_mgr get_has_seen_first_time_calibration}} condition then "
    "routes to main_hub_screen). Writes that one guest byte, re-asserting it "
    "until the UI has left the boot screens, then stops. Title-gated.",
    "CPU");

// --- harness automation ------------------------------------------------------

DEFINE_bool(
    rb3dx_autoconfirm_parts, false,
    "RB3DX / RB3 TU5 (title 0x45410914), default off: closed-loop menu "
    "autopilot. Every ~2 s a host thread reads the current UI screen and "
    "injects the pad press that advances it (boot screens: A; first-boot "
    "hint: DOWN/A; main_hub: UP,UP,A into PLAY NOW; song_select: join pad 1 "
    "then A; part_difficulty_screen: pad-1-first A confirms). Fixed-time "
    "--scripted_input presses cannot hit these windows reliably (menu load "
    "times vary by tens of seconds). Presses go through the nop HID driver's "
    "per-pad injection; no guest writes. Needs pad presence: pass "
    "--scripted_input or --scripted_pad_subtypes. Title-gated.",
    "CPU");
DEFINE_int32(
    rb3dx_autoconfirm_p2_up, 0,
    "RB3DX autopilot: press DPAD-UP on pad 1 for the first N samples at "
    "part_difficulty_screen before the A confirms, to navigate P2's CHOOSE "
    "INSTRUMENT list off the default first-free part (e.g. 1 = select the "
    "entry above BASS -- GUITAR when the same-instrument un-grey is armed).",
    "CPU");

// --- read-only diagnostics ---------------------------------------------------

DEFINE_bool(
    rb3dx_ui_probe, false,
    "RB3DX / RB3 TU5 (title 0x45410914) DIAGNOSTIC, default off: every ~2 s, "
    "log the guest UI state from a host thread: BandUI transition state and "
    "current/transition screen names (the `RB3DX UI PROBE[n]:` line), the "
    "screens' panels and their loaders, the LoadMgr queue, the live guest "
    "threads, and the saveload_mgr / net_sync / session / overshell objects "
    "and pad tables found via ObjectDir::sMainDir. Reads guest memory only: "
    "it writes nothing and installs no hooks. Title-gated.",
    "CPU");
DEFINE_bool(
    rb3_stream_census, false,
    "RB3 (title 0x45410914), default off, requires --rb3dx_ui_probe: sweep the "
    "guest heap for live StandardStream objects (vtable 0x820F6A8C) and report "
    "each one's mState(+0x14), receivers vector(+0x20..+0x24) and channel "
    "array(+0x78..+0x80) as `STREAM-CENSUS` lines. The song-audio gate is "
    "MasterAudio::IsLoaded (0x8277B6E8) = mStream->IsReady() = (mState == 2); "
    "'mState==0 with an empty receivers vector' proves StandardStream::"
    "InitInfo (0x827046B8) never ran (WORKSTREAM §8x). Read-only. "
    "Title-gated.",
    "CPU");
DEFINE_uint64(
    rb3dx_si_claim_anchor, 0,
    "RB3DX (title 0x45410914), default 0 (off), requires --rb3dx_ui_probe: "
    "guest VA of the RB3Enhanced.dll SI claim-table anchor (the lis/addi "
    "base register in SIInstallClone). Layout from SameInstrumentHooks.c: "
    "gClaims[] {track,count} pairs at +0 stride 8, gClaimCount at +0x1C8, "
    "gImplCount at +0x1CC. When set, the ui probe logs these each sample "
    "plus the BandUserMgr slot map and pad table. Read-only. Title-gated.",
    "CPU");
DEFINE_bool(
    si_hook_verify, false,
    "RB3DX / RB3 TU5 (title 0x45410914), default off: read-only host-thread "
    "verifier for the RB3Enhanced-DLL same-instrument GAMEPLAY hooks (H1 "
    "ProcessConfig @0x8276FA08, H2 RecalcGemList @0x82794740). Samples the "
    "first instruction word at each site and decodes it: a `b` (primary "
    "opcode 18) into the DLL image [0x84000000,0x84850000) within +/-32MB is "
    "an installed RB3E HookFunction detour (PASS); the stock prologue "
    "0x7D8802A6 (mflr r12) means the hooks are not installed. Pair with "
    "--si_load_dll. Title-gated.",
    "CPU");

// --- MemAlloc / MemFree instrumentation --------------------------------------

DEFINE_bool(
    rb3dx_alloc_probe, false,
    "RB3DX (title 0x45410914) DIAGNOSTIC, default off: override the "
    "__savegprlr_23 helper MemAlloc@0x827BCD38 calls with an exact host "
    "emulation that also logs size/align/caller-LR and a guest stack walk for "
    "allocation sizes with a non-zero top byte or >= 4 MiB (the main_hub OOM "
    "investigation, WORKSTREAM §4.C). Guest-transparent. Title-gated.",
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
    "peak); this is a buffered binary sink. Implies the __savegprlr_23 "
    "override. Title-gated.",
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
    "VALUE (the allocated pointer) by overriding the epilogue helper "
    "__restgprlr_23 @0x82829294 that MemAlloc tail-branches through, matching "
    "on (r1 == the entry SP recorded for this thread AND the about-to-be-"
    "restored LR == that call's caller LR). Set false if the epilogue "
    "override destabilises the guest; the alloc-entry trace still works "
    "without it, only pointer-exact alloc/free pairing is lost.",
    "CPU");

// --- same-instrument (RB3Enhanced.dll) harness -------------------------------

DEFINE_bool(
    si_load_dll, false,
    "RB3DX / RB3 TU5 (title 0x45410914), default off: load "
    "game:\\RB3Enhanced.dll at its preferred base 0x84000000 via "
    "KernelState::LoadUserModule(call_entry=false) before the title starts, "
    "then -- once, well into boot, from the MemAlloc __savegprlr_23 override "
    "on a live guest thread -- call the DLL's own InitSameInstrument at "
    "--si_init_va (required), which installs the SI hooks itself. The title "
    "and DLL images are first made guest-writable through the guest heap. "
    "call_entry=false skips RB3E's CRT/DllMain boot (its socket/event init is "
    "unsafe headless). Pair with --si_hook_verify. Title-gated.",
    "CPU");
DEFINE_uint64(
    si_init_va, 0,
    "RB3DX / RB3 TU5 (title 0x45410914), default 0: guest VA of the "
    "RB3Enhanced.dll InitSameInstrument() entry (void)(void), from the DLL's "
    "link map. Required by --si_load_dll. Title-gated.",
    "CPU");
DEFINE_uint64(
    si_force_allow_va, 0,
    "RB3DX / RB3 TU5 (title 0x45410914), default 0 (disabled): guest VA of the "
    "RB3Enhanced.dll config.AllowSameInstrument flag (from the DLL's own "
    "IsActiveHook disassembly; the config struct moves between builds). "
    "--si_load_dll skips DllMain, so the flag stays 0 and the installed hooks "
    "run pass-through; when non-zero, Xenia writes 1 to this byte right after "
    "the DLL loads. Required for behavioural runs. Title-gated.",
    "CPU");
