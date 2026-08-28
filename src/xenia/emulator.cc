/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/emulator.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <set>
#include <thread>
#include <unordered_map>

#if XE_PLATFORM_LINUX
#include <sys/mman.h>
#include <cerrno>
#endif

#include "config.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/error/en.h"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/apu/audio_system.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/backend/null_backend.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/milo_trace.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/dc3_hack_pack.h"
#include "xenia/dc3_nui_patch_resolver.h"
#include "xenia/dc3_runtime_telemetry.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/nop/nop_input_driver.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/gameinfo_utils.h"
#include "xenia/kernel/util/xdbf_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xbdm/xbdm_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xevent.h"
#include "xenia/memory.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/devices/stfs_container_device.h"
#include "xenia/vfs/virtual_file_system.h"

#ifndef XE_HEADLESS_BUILD
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#endif

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#endif  // XE_ARCH

DECLARE_int32(user_language);

DEFINE_double(time_scalar, 1.0,
              "Scalar used to speed or slow time (1x, 2x, 1/2x, etc).",
              "General");
DEFINE_string(
    launch_module, "",
    "Executable to launch from the .iso or the package instead of default.xex "
    "or the module specified by the game. Leave blank to launch the default "
    "module.",
    "General");

DEFINE_int32(dc3_crt_bisect_max, -1,
             "DC3: max CRT constructor index to allow (-1=disabled/all run, "
             "0=only index 0 runs, N=indices 0..N run). "
             "Used for binary search to find heap-corrupting constructor.",
             "DC3");
DEFINE_string(dc3_crt_skip_indices, "",
              "DC3: comma-separated list of CRT constructor indices to "
              "nullify. Supports ranges: '69,75,98-340'. "
              "Default empty = use dc3_crt_skip_nui.",
              "DC3");
DEFINE_bool(dc3_ik_telemetry, false, "DC3: enable IK telemetry", "DC3");
DEFINE_bool(rb3_mount_update, false,
            "RB3 boot-to-menu experiment: mount update: at the disc dir so RB3 "
            "finds update:\\gen\\patch_xbox.hdr (title-update content). Default "
            "OFF (inert for DC3 and normal runs).",
            "RB3");
DEFINE_bool(dc3_game_screen_real_goto, true,
            "DC3: drive loading->game_screen via the real UIManager::GotoScreen "
            "(runs game_panel Load()->CreateGame() and the per-frame Poll state "
            "machine that creates the Game and sets up dancer anims). Only safe "
            "once the song FileMerger merge has completed (gated on merge_busy). "
            "Set false to fall back to the old host force-set (no Game created).",
            "DC3");
DEFINE_bool(dc3_gameplay_probe, false,
            "DC3: enable the host-side GATE PROBE / PKPROBE gameplay diagnostics "
            "(reads HamDirector anim state + walks mSongAnims/mPropKeys from the "
            "NUI thread). Off by default — it executes guest helper fns each "
            "game_screen frame, which perturbs timing. Was used to diagnose the "
            "RndPropAnim::GetKeys wrong-receiver hang; keep for recurrence.",
            "DC3");
DEFINE_bool(dc3_null_read_cache_stream, false, "DC3: null read cache", "DC3");
DEFINE_bool(dc3_crt_skip_nui, true,
            "DC3: auto-nullify NUI/Kinect SDK CRT constructors (indices "
            "75,98-101,210-328). These call unresolved internal NUI "
            "functions that corrupt the heap. Set false to disable.",
            "DC3");
DEFINE_bool(dc3_guest_overrides, true,
            "DC3: use guest extern overrides for eligible simple NUI/XBC "
            "stub-return functions (default cutover path; skips byte patching "
            "for registered entries; preserves fake_kinect_data "
            "NuiSkeletonGetNextFrame path). The legacy NUI/XBC byte-patch "
            "fallback path has been removed; false logs a warning and is "
            "ignored for this path.",
            "DC3");
DEFINE_bool(dc3_debug_read_cache_stream_step_override, false,
            "DC3: enable invasive ReadCacheStream step-by-step guest override "
            "for DTB debugging. WARNING: performs extra reads/seeks and can "
            "perturb checksum/parser behavior; use only in dedicated probe runs.",
            "DC3");
DEFINE_bool(dc3_debug_mempool_alloc_probe, false,
            "DC3: log-only probe for MemOrPoolAlloc. Captures caller LR, "
            "requested size, file/line/name args, and return value. "
            "Detailed logs only on failure or from known crash-path callers.",
            "DC3");
DEFINE_string(
    dc3_debug_findarray_override_mode, "off",
    "DC3: debug mode for DataArray::FindArray(Symbol,bool) override "
    "(off|log_only|stub_on_fail|null_on_fail|setupfont_fix). "
    "'setupfont_fix' enables an emulated SystemConfig(Symbol,Symbol) probe path "
    "that repairs the known Rnd::SetupFont bad 'font' key literal in some "
    "decomp builds. Use only for DC3 decomp runtime forensics/progression.",
    "DC3");
DEFINE_string(dc3_nui_patch_layout, "auto",
              "DC3: NUI/XBC patch address layout selector "
              "(auto|original|decomp). 'auto' uses the zero-padding heuristic "
              "and logs a .text fingerprint for future resolver matching.",
              "DC3");
DEFINE_string(dc3_nui_layout_fingerprint_original, "",
              "DC3: optional .text FNV1a64 fingerprint (hex) for original "
              "NUI/XBC patch layout selection in auto mode.",
              "DC3");
DEFINE_string(dc3_nui_layout_fingerprint_decomp, "",
              "DC3: optional .text FNV1a64 fingerprint (hex) for decomp "
              "NUI/XBC patch layout selection in auto mode.",
              "DC3");
DEFINE_string(
    dc3_nui_layout_fingerprint_cache_path, "",
    "DC3: optional fingerprint cache file with lines "
    "'original=<hex>' and/or 'decomp=<hex>' for auto layout selection.",
    "DC3");
DEFINE_string(
    dc3_nui_symbol_map_path, "",
    "DC3: optional symbol map manifest used by the NUI/XBC resolver "
    "(symbols.txt-style 'name = .text:0xADDR;'). If unset, a local "
    "dc3-decomp symbols.txt path is auto-probed.",
    "DC3");
DEFINE_string(dc3_nui_patch_resolver_mode, "hybrid",
              "DC3: NUI/XBC patch target resolver mode "
              "(hybrid|strict). hybrid uses manifest/symbol/signature "
              "resolution before catalog fallback; strict disables raw "
              "catalog fallback.",
              "DC3");
DEFINE_string(
    dc3_nui_patch_manifest_path, "",
    "DC3: optional machine-readable DC3 NUI/XBC patch manifest JSON "
    "(xenia_dc3_patch_manifest.json). Preferred over symbols.txt when present.",
    "DC3");
DEFINE_bool(dc3_nui_enable_signature_resolver, true,
            "DC3: enable signature-resolver hook for NUI/XBC patch targets "
            "(default on; used by hybrid/strict resolver modes).",
            "DC3");
DEFINE_bool(dc3_nui_signature_trace, false,
            "DC3: log runtime PPC words for NUI/XBC patch targets "
            "at catalog and resolved addresses (debugging signature resolver).",
            "DC3");
DEFINE_string(
    dc3_crash_snapshot_path, "",
    "DC3: optional JSON crash snapshot output path written from guest crash "
    "dumps (headless-friendly structured artifact for postmortem tools).",
    "DC3");
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
    rb3_tu5_hold_main, false,
    "RB3 TU5 (title 0x45410914), default off, DIAGNOSTIC: byte-patch main()'s "
    "terminal `blr` (0x82272EB0) into an infinite self-branch so the XEX entry "
    "thread never returns. main_impl = App::_ct(); reg(); broadcast(); return -- "
    "none is a frame loop, so it returns ~15s in (after the boot splash) and the "
    "guest CRT tears down every worker thread. This holds main alive to test "
    "whether the frontend/game loop lives on a surviving worker thread (menu "
    "would appear) or genuinely never starts. Title-gated + default-off => "
    "DC3-inert.",
    "CPU");
DEFINE_bool(
    rb3_tu5_loop_main, false,
    "RB3 TU5 (title 0x45410914), default off, EXPERIMENT: turn main()'s tail "
    "into a frame loop. main_impl's 3rd call (bl 0x82270000 at 0x82272E98) is "
    "'poll every registered Pollable once' (Game/Rnd/Graph/Synth/MoviePanel/... "
    "-- the body of a real frame loop). main calls it ONCE then returns, so the "
    "UI panel loads never finish and the frontend never renders. Byte-patch the "
    "instruction right after that call (0x82272E9C, `li r3,0`) into `b "
    "0x82272E94` so thr6 re-invokes the poll forever -- a crude frame loop that "
    "keeps the title alive and pumps the subsystems. Title-gated + default-off "
    "=> DC3-inert.",
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

namespace xe {

using namespace xe::literals;

namespace {
using namespace xe::dc3;

const char* AccessOpName(Exception::AccessViolationOperation op) {
  switch (op) {
    case Exception::AccessViolationOperation::kRead:
      return "READ";
    case Exception::AccessViolationOperation::kWrite:
      return "WRITE";
    default:
      return "UNKNOWN";
  }
}

void MaybeWriteDc3CrashSnapshotJson(const Emulator* emulator, Exception* ex,
                                    kernel::XThread* current_thread,
                                    const cpu::Function* guest_function,
                                    const cpu::ppc::PPCContext* context) {
  if (cvars::dc3_crash_snapshot_path.empty() || !emulator || !ex ||
      !current_thread || !context) {
    return;
  }

  uint32_t guest_pc = 0;
  if (guest_function && guest_function->is_guest()) {
    guest_pc = static_cast<const cpu::GuestFunction*>(guest_function)
                   ->MapMachineCodeToGuestAddress(ex->pc());
  }

  uint64_t host_fault = 0;
  uint32_t guest_fault = 0;
  bool has_fault = ex->code() == Exception::Code::kAccessViolation;
  if (has_fault) {
    host_fault = ex->fault_address();
    if (emulator->memory() && emulator->memory()->virtual_membase()) {
      uint64_t membase =
          reinterpret_cast<uintptr_t>(emulator->memory()->virtual_membase());
      guest_fault = static_cast<uint32_t>(host_fault - membase);
    }
  }

  std::ofstream out(cvars::dc3_crash_snapshot_path,
                    std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    XELOGW("DC3 crash snapshot: failed to open {}", cvars::dc3_crash_snapshot_path);
    return;
  }

  out << "{\n";
  out << fmt::format("  \"host_pc\": {},\n", static_cast<uint64_t>(ex->pc()));
  out << fmt::format("  \"guest_pc\": {},\n", guest_pc);
  out << fmt::format("  \"guest_lr\": {},\n", static_cast<uint32_t>(context->lr));
  out << fmt::format("  \"guest_ctr\": {},\n", static_cast<uint32_t>(context->ctr));
  out << fmt::format("  \"guest_cr\": {},\n", static_cast<uint32_t>(context->cr()));
  out << fmt::format(
      "  \"guest_xer\": {{\"ca\": {}, \"ov\": {}, \"so\": {}}},\n",
      static_cast<uint32_t>(context->xer_ca), static_cast<uint32_t>(context->xer_ov),
      static_cast<uint32_t>(context->xer_so));
  out << fmt::format("  \"thread\": {{\"host_id\": {}, \"guest_id\": {}, \"handle\": {}}},\n",
                     current_thread->thread()->system_id(), current_thread->thread_id(),
                     current_thread->handle());
  out << "  \"gpr\": [";
  for (int i = 0; i < 32; ++i) {
    if (i) out << ", ";
    out << static_cast<uint64_t>(context->r[i]);
  }
  out << "],\n";
  if (has_fault) {
    out << fmt::format(
        "  \"fault\": {{\"host\": {}, \"guest\": {}, \"access\": \"{}\"}},\n",
        host_fault, guest_fault, AccessOpName(ex->access_violation_operation()));
  } else {
    out << "  \"fault\": null,\n";
  }
  out << "  \"guest_code_words\": [";
  bool first = true;
  if (guest_pc && emulator->memory() && guest_pc >= 0x80000000 && guest_pc < 0xA0000000) {
    uint32_t dump_start = (guest_pc > 0x20) ? (guest_pc - 0x20) : guest_pc;
    uint32_t dump_end = guest_pc + 0x20;
    for (uint32_t addr = dump_start; addr < dump_end; addr += 4) {
      auto* mem_ptr = emulator->memory()->TranslateVirtual<uint8_t*>(addr);
      if (!mem_ptr) break;
      uint32_t instr = xe::load_and_swap<uint32_t>(mem_ptr);
      if (!first) out << ", ";
      first = false;
      out << fmt::format("{{\"addr\": {}, \"word\": {}}}", addr, instr);
    }
  }
  out << "]\n";
  out << "}\n";
  out.flush();
  if (!out.good()) {
    XELOGW("DC3 crash snapshot: write failed {}", cvars::dc3_crash_snapshot_path);
    return;
  }
  XELOGI("DC3 crash snapshot JSON written: {}", cvars::dc3_crash_snapshot_path);
}

void Dc3NuiReturnOkExtern(cpu::ppc::PPCContext* ppc_context,
                          kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }
  ppc_context->r[3] = 0;
}

void Dc3NuiReturnNeg1Extern(cpu::ppc::PPCContext* ppc_context,
                            kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }
  ppc_context->r[3] = UINT64_C(0xFFFFFFFFFFFFFFFF);
}

void Dc3NuiReturn1Extern(cpu::ppc::PPCContext* ppc_context,
                         kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }
  ppc_context->r[3] = 1;
}

// RB3DX heap-"main" fragmentation attribution (--rb3dx_alloc_trace_path).
//
// A per-allocation binary trace. Every record is 32 bytes, little-endian host
// order, appended to one file:
//
//   u8  tag    1 = MemAlloc entry, 2 = MemAlloc return, 3 = MemFree entry
//   u8  pad
//   u16 tid    guest thread id (low 16 bits)
//   u32 seq    MemAlloc ordinal (tags 1 and 2 share it; MemFree ordinal for 3)
//   u64 ns     steady_clock ns since the sink opened
//   u32 a      tag1: size (r3)      tag2: returned pointer (r3)  tag3: ptr (r3)
//   u32 b      tag1: align (r4)     tag2: 0                      tag3: header
//   u32 lr     caller LR (r12 at the callee's entry)
//   u32 sp     caller SP (r1 at the callee's entry, before its stwu)
//
// Why binary: the measured rate is ~876 MemAlloc/s mean and 1900/s peak, so a
// formatted log line per call would cost more than the emulation. A record is
// a memcpy under a mutex into a 1 MiB-buffered FILE.
namespace {

struct Rb3dxTraceRec {
  uint8_t tag;
  uint8_t pad;
  uint16_t tid;
  uint32_t seq;
  uint64_t ns;
  uint32_t a;
  uint32_t b;
  uint32_t lr;
  uint32_t sp;
};
static_assert(sizeof(Rb3dxTraceRec) == 32, "trace record must stay 32 bytes");

class Rb3dxAllocTraceSink {
 public:
  explicit Rb3dxAllocTraceSink(const std::string& path)
      : f_(fopen(path.c_str(), "wb")),
        t0_(std::chrono::steady_clock::now()) {
    if (f_) {
      setvbuf(f_, nullptr, _IOFBF, 1 << 20);
    }
  }
  ~Rb3dxAllocTraceSink() {
    if (f_) {
      fflush(f_);
      fclose(f_);
    }
  }
  bool ok() const { return f_ != nullptr; }
  uint64_t written() const { return count_.load(std::memory_order_relaxed); }

  void Emit(uint8_t tag, uint32_t tid, uint32_t seq, uint32_t a, uint32_t b,
            uint32_t lr, uint32_t sp) {
    if (!f_) {
      return;
    }
    Rb3dxTraceRec r{};
    r.tag = tag;
    r.tid = static_cast<uint16_t>(tid);
    r.seq = seq;
    r.ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0_)
            .count());
    r.a = a;
    r.b = b;
    r.lr = lr;
    r.sp = sp;
    std::lock_guard<std::mutex> g(m_);
    fwrite(&r, sizeof(r), 1, f_);
    count_.fetch_add(1, std::memory_order_relaxed);
  }
  void Flush() {
    std::lock_guard<std::mutex> g(m_);
    if (f_) {
      fflush(f_);
    }
  }

 private:
  FILE* f_;
  std::chrono::steady_clock::time_point t0_;
  std::mutex m_;
  std::atomic<uint64_t> count_{0};
};

// Owned by the emulator for the process lifetime once CompleteLaunch installs
// it; read without synchronisation from the guest-thread handlers (published
// before the guest module is launched, never torn down while the guest runs).
std::unique_ptr<Rb3dxAllocTraceSink> rb3dx_alloc_trace_sink;

// Per-guest-thread record of the MemAlloc call currently in flight, so that
// the __restgprlr_23 epilogue override can attribute the returned pointer back
// to the entry record. MemAlloc cannot recurse into itself, so one slot per
// thread is exact.
struct Rb3dxPendingAlloc {
  bool active = false;
  uint32_t sp = 0;   // MemAlloc's entry SP (== r1 at the epilogue restore)
  uint32_t lr = 0;   // caller LR (== the word reloaded from [sp-8])
  uint32_t seq = 0;  // MemAlloc ordinal of the entry record
};
thread_local Rb3dxPendingAlloc t_rb3dx_pending_alloc;

// Free-side ordinal (tag 3).
std::atomic<uint32_t> rb3dx_memfree_calls{0};
// Diagnostics for the epilogue override: how many returns we matched.
std::atomic<uint64_t> rb3dx_ret_matches{0};

}  // namespace

// RB3DX: MemFree@0x827BC430 entry probe (--rb3dx_free_trace).
//
// Same recipe as the MemAlloc side, one helper down the save chain: MemFree's
// prologue is `mflr r12 ; bl 0x82829250 ; addi r31,r1,-0x90 ; stwu r1,-0x90(r1)`,
// i.e. it calls __savegprlr_26 and returns to 0x827BC438 -- a unique filter key.
// The handler emulates __savegprlr_26 exactly (std r26..r31 at r1-0x38..r1-0x10,
// stw r12 at r1-8) so every other caller of that helper is unaffected.
// At the filter point r3 = the pointer being freed, r12 = the caller LR.
void Rb3dxSaveGprLr26ProbeExtern(cpu::ppc::PPCContext* ppc_context,
                                 kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) {
    return;
  }
  uint8_t* base = kernel_state->memory()->virtual_membase();
  uint32_t sp = static_cast<uint32_t>(ppc_context->r[1]);
  // --- exact __savegprlr_26 emulation (must be semantically transparent) ---
  for (int i = 0; i < 6; ++i) {
    xe::store_and_swap<uint64_t>(base + sp - 0x38 + i * 8,
                                 ppc_context->r[26 + i]);
  }
  xe::store_and_swap<uint32_t>(base + sp - 8,
                               static_cast<uint32_t>(ppc_context->r[12]));
  // --- probe part ---
  const uint32_t kMemFreeRet = 0x827BC438;  // bl at 0x827BC434 in MemFree
  if (static_cast<uint32_t>(ppc_context->lr) != kMemFreeRet) {
    return;
  }
  auto* sink = rb3dx_alloc_trace_sink.get();
  if (!sink) {
    return;
  }
  uint32_t ptr = static_cast<uint32_t>(ppc_context->r[3]);
  // The allocated-block header is the word immediately below the payload:
  // (totalUsedWords << 8) | (padWords << 4) | flags. Only read it for pointers
  // that plausibly live in a guest heap arena, so a MemFree(NULL) or a wild
  // pointer can never fault the host.
  uint32_t header = 0;
  if (ptr >= 0x30000000u && ptr < 0x60000000u && (ptr & 3u) == 0u) {
    header = xe::load_and_swap<uint32_t>(base + ptr - 4);
  }
  uint32_t seq = rb3dx_memfree_calls.fetch_add(1, std::memory_order_relaxed);
  uint32_t tid =
      ppc_context->thread_state ? ppc_context->thread_state->thread_id() : 0;
  sink->Emit(3, tid, seq, ptr, header,
             static_cast<uint32_t>(ppc_context->r[12]), sp);
}

// RB3DX: MemAlloc RETURN probe (--rb3dx_ret_trace).
//
// MemAlloc's epilogue is `mr r3,r30 ; addi r1,r31,0xb0 ; b 0x82c5ffc0`, and
// that thunk lands in __restgprlr_23 @0x82829294 (the shared restore chain:
// ld r23..r31 from r1-0x50..r1-0x10, lwz r12,-8(r1), mtlr r12, blr). Overriding
// it gives us the one thing the entry hook cannot see: r3, the pointer handed
// back -- which is what says whether a caller's blocks land at the FirstFit
// bottom or the LastFit top of the arena.
//
// The override is exact (it performs the same reloads and sets LR), and Xenia
// compiles the guest tail-branch as CallExtern + jmp epilog, so control still
// unwinds through the host call chain to MemAlloc's caller.
//
// Attribution filter: this helper is shared by every function that saved
// r23..r31, so we match against the per-thread pending slot -- r1 must equal
// the SP recorded at MemAlloc's entry (callees are strictly below it, callers
// strictly above) AND the LR being restored must equal that call's caller LR
// (a different call site from the same frame would restore a different one).
void Rb3dxRestGprLr23TraceExtern(cpu::ppc::PPCContext* ppc_context,
                                 kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) {
    return;
  }
  uint8_t* base = kernel_state->memory()->virtual_membase();
  uint32_t sp = static_cast<uint32_t>(ppc_context->r[1]);
  // --- exact __restgprlr_23 emulation ---
  for (int i = 0; i < 9; ++i) {
    ppc_context->r[23 + i] =
        xe::load_and_swap<uint64_t>(base + sp - 0x50 + i * 8);
  }
  uint32_t restored_lr = xe::load_and_swap<uint32_t>(base + sp - 8);
  ppc_context->r[12] = restored_lr;
  ppc_context->lr = restored_lr;
  // --- probe part ---
  auto& p = t_rb3dx_pending_alloc;
  if (!p.active || p.sp != sp || p.lr != restored_lr) {
    return;
  }
  p.active = false;
  auto* sink = rb3dx_alloc_trace_sink.get();
  if (!sink) {
    return;
  }
  rb3dx_ret_matches.fetch_add(1, std::memory_order_relaxed);
  uint32_t tid =
      ppc_context->thread_state ? ppc_context->thread_state->thread_id() : 0;
  sink->Emit(2, tid, p.seq, static_cast<uint32_t>(ppc_context->r[3]), 0,
             restored_lr, sp);
}

// RB3DX main_hub OOM investigation (--rb3dx_alloc_probe).
//
// We need MemAlloc's entry args (r3=size bytes, r4=align) plus the caller
// LR, captured while they're live. Synthetic PPC trampolines/caves proved
// fragile (the JIT's scanner mis-compiles cave functions; a .data cave also
// corrupted live zero-init statics). Instead we override a REAL, pre-declared
// guest function on MemAlloc's path: __savegprlr_23 @ 0x82829244 (declared by
// XexModule::FindSaveRest before any execution). MemAlloc's prologue is
//   mflr r12 ; bl __savegprlr_23 ; ... ; stwu r1,-0xB0(r1)
// so at the bl: r3=size, r4=align, r12=caller LR, lr=0x827BCD40 (the return
// into MemAlloc, our filter key), r1 = the CALLER's SP (stwu not yet done).
// The handler EXACTLY emulates __savegprlr_23 (std r23..r31 at r1-0x50..-0x10,
// stw r12 at r1-8) so every other function calling it is unaffected, and logs
// allocation sizes with a garbage top byte.
void Rb3dxSaveGprLr23ProbeExtern(cpu::ppc::PPCContext* ppc_context,
                                 kernel::KernelState* kernel_state) {
  static std::atomic<uint64_t> s_calls{0};
  static std::atomic<uint64_t> s_memalloc_calls{0};
  static std::atomic<uint32_t> s_reports{0};
  uint64_t n = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!ppc_context || !kernel_state) {
    return;
  }
  Memory* mem_for_emu = kernel_state->memory();
  uint8_t* base = mem_for_emu->virtual_membase();
  uint32_t sp = static_cast<uint32_t>(ppc_context->r[1]);
  // --- exact __savegprlr_23 emulation (must be semantically transparent) ---
  for (int i = 0; i < 9; ++i) {
    xe::store_and_swap<uint64_t>(base + sp - 0x50 + i * 8,
                                 ppc_context->r[23 + i]);
  }
  xe::store_and_swap<uint32_t>(base + sp - 8,
                               static_cast<uint32_t>(ppc_context->r[12]));
  // --- probe part ---
  uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
  if (n <= 3) {
    XELOGI("RB3DX ALLOC PROBE: savegprlr_23 warmup #{} lr=0x{:08X}", n, lr);
  }
  const uint32_t kMemAllocRet = 0x827BCD40;  // bl at 0x827BCD3C in MemAlloc
  if (lr != kMemAllocRet) {
    return;
  }
  uint64_t m = s_memalloc_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t size = static_cast<uint32_t>(ppc_context->r[3]);
  // --- OOM-race mitigation (--rb3dx_clamp_alloc) ---
  // doc-09: the main_hub-load OOM allocation arrives as 0xGG001524 -- a correct
  // small size in the low 24 bits with a garbage NON-ZERO top byte (an
  // uninitialized guest read; zero on hardware, garbage under Xenia). Clamp
  // ONLY that signature (top byte set AND low-24 < 1 MiB) so legit multi-MiB
  // allocations are never touched. Modifying r3 here is safe: __savegprlr_23
  // returns into MemAlloc before its stwu, and size flows straight to the
  // MemHeap::Alloc call.
  if (cvars::rb3dx_clamp_alloc && (size & 0xFF000000u) != 0u &&
      (size & 0x00FFFFFFu) < 0x00100000u) {
    static std::atomic<uint32_t> s_clamps{0};
    uint32_t clamped = size & 0x00FFFFFFu;
    ppc_context->r[3] = clamped;
    if (s_clamps.fetch_add(1, std::memory_order_relaxed) < 32) {
      XELOGW(
          "RB3DX clamp-alloc: MemAlloc size 0x{:08X} -> 0x{:08X} (garbage top "
          "byte masked; call #{})",
          size, clamped, m);
    }
    size = clamped;
  }
  // --- per-allocation binary trace (--rb3dx_alloc_trace_path) ---
  // Emitted BEFORE the suspicious/big filter below, so the trace is complete;
  // also arms this thread's pending slot so the __restgprlr_23 epilogue
  // override can attach the returned pointer to this record.
  if (auto* sink = rb3dx_alloc_trace_sink.get()) {
    uint32_t tid =
        ppc_context->thread_state ? ppc_context->thread_state->thread_id() : 0;
    uint32_t seq32 = static_cast<uint32_t>(m);
    sink->Emit(1, tid, seq32, size, static_cast<uint32_t>(ppc_context->r[4]),
               static_cast<uint32_t>(ppc_context->r[12]), sp);
    // --- caller-of-caller backtrace (tag 4, --rb3dx_stack_trace) ---
    // The immediate caller LR alone is not enough when the caller is a thin
    // shim: the dominant idle-churn site turned out to be XMemAlloc's
    // `bl MemAlloc`, which tells you the XDK allocator was used but not by
    // whom. Four more frames are recoverable for free from the guest stack.
    //
    // Idiom: every non-leaf here starts `mflr r12 ; bl __savegprlr_N ;
    // stwu r1,-F(r1)`, and __savegprlr_N stores r12 at [entry_r1 - 8]. So for
    // a frame at F_sp, the backchain word [F_sp] is the SP of that function's
    // CALLER, and that function's own return address sits at (caller_sp - 8)
    // -- frame-size independent, which is what makes this walk cheap.
    //
    // Everything is range-checked and the walk stops on the first implausible
    // link, so a frameless/leaf caller yields 0 rather than a wild host read.
    if (cvars::rb3dx_stack_trace) {
      uint32_t frames[4] = {0, 0, 0, 0};
      uint32_t f_sp = sp;
      for (int k = 0; k < 4; ++k) {
        // Guest stacks live well below the code; require a plausible, aligned,
        // strictly-upward backchain link (the stack grows down).
        if (f_sp < 0x40000000u || f_sp >= 0x80000000u || (f_sp & 3u)) {
          break;
        }
        uint32_t parent = xe::load_and_swap<uint32_t>(base + f_sp);
        if (parent <= f_sp || parent < 0x40000000u || parent >= 0x80000000u ||
            (parent & 3u)) {
          break;
        }
        uint32_t ret = xe::load_and_swap<uint32_t>(base + parent - 8);
        // Only accept something that looks like a guest .text return address.
        frames[k] = (ret >= 0x82000000u && ret < 0x83000000u) ? ret : 0;
        f_sp = parent;
      }
      sink->Emit(4, tid, seq32, frames[0], frames[1], frames[2], frames[3]);
    }
    auto& p = t_rb3dx_pending_alloc;
    p.active = true;
    p.sp = sp;
    p.lr = static_cast<uint32_t>(ppc_context->r[12]);
    p.seq = seq32;
  }
  // --- Same-instrument cave-execution self-test (--si_selftest) ---
  // Fire ONCE, well into boot (guest heaps up, 0x826684C0 resolvable), on this
  // valid guest-thread context: synthetically call the detour site
  // OvershellPartSelectProvider::IsActive @0x826684C0 with a crafted, zeroed
  // scratch `this` (mSlots.begin==end==0 => the original body's empty-check
  // early-returns 0 with no vcall / no globals touched). Because 0x826684C0's
  // first instruction is our `b 0x82C8A080`, Execute() forces Xenia's JIT to
  // translate + run the .data cave (trampoline -> orig(returns 0) -> flag(=1)
  // -> force r3=1). Return value discriminates: r3==1 => the cave EXECUTED and
  // the SI logic fires under Xenia (patch works modulo NX); r3==0 => inert
  // (cave not reached / flag path fell through) => in-emulator root-cause; a
  // fault => Xenia mis-translates the .data cave. Full volatile+nonvolatile
  // register snapshot/restore keeps the intercepted MemAlloc undisturbed.
  if (cvars::si_selftest) {
    static std::atomic<bool> s_fired{false};
    bool expected = false;
    if (m >= 800 && s_fired.compare_exchange_strong(expected, true)) {
      uint8_t* mbase = mem_for_emu->virtual_membase();
      // Snapshot the whole GPR file + link/count/cr so MemAlloc continues as if
      // nothing happened.
      uint64_t saved_r[32];
      for (int i = 0; i < 32; ++i) saved_r[i] = ppc_context->r[i];
      uint64_t saved_lr = ppc_context->lr;
      uint64_t saved_ctr = ppc_context->ctr;
      // Craft a zeroed scratch object far below the current SP (won't collide
      // with IsActive's small frame at r1-0x60).
      uint32_t scratch = (static_cast<uint32_t>(ppc_context->r[1]) - 0x2000u) &
                         ~0xFu;
      for (uint32_t off = 0; off < 0x40; off += 4) {
        xe::store_and_swap<uint32_t>(mbase + scratch + off, 0);
      }
      auto* proc = ppc_context->processor;
      auto* ts = ppc_context->thread_state;
      auto describe = [](uint32_t r3, uint32_t self) -> const char* {
        if (r3 == 1) return "SI ACTIVE (cave forced r3=1)";
        if (r3 == 0) return "STOCK/INERT (orig 0 passed through)";
        if (r3 == self) return "UNCHANGED (branch not followed / no-op)";
        if (r3 == 0xDEADBABEu) return "resolve/exec FAILED";
        return "ANOMALY";
      };
      // (A) Detour site: Execute(0x826684C0). First insn is `b 0x82C8A080`;
      // tests whether the JIT follows the .text->.data detour branch.
      {
        uint64_t args[1] = {scratch};
        uint32_t r3 = static_cast<uint32_t>(proc->Execute(ts, 0x826684C0, args,
                                                           1));
        XELOGW("SI SELFTEST (A) detour@0x826684C0 empty this=0x{:08X} -> "
               "r3=0x{:08X}  [{}]",
               scratch, r3, describe(r3, scratch));
      }
      // (B) Cave entry direct: Execute(0x82C8A080). Tests whether the .data
      // cave CODE itself translates + executes under the JIT (independent of
      // the detour branch). Rezero the scratch (A may have written the frame).
      {
        for (uint32_t off = 0; off < 0x40; off += 4)
          xe::store_and_swap<uint32_t>(mbase + scratch + off, 0);
        uint64_t args[1] = {scratch};
        uint32_t r3 = static_cast<uint32_t>(proc->Execute(ts, 0x82C8A080, args,
                                                          1));
        XELOGW("SI SELFTEST (B) cave@0x82C8A080  empty this=0x{:08X} -> "
               "r3=0x{:08X}  [{}]  (1 => cave code executes + SI logic fires "
               "under Xenia JIT)",
               scratch, r3, describe(r3, scratch));
      }
      // Restore the intercepted MemAlloc's register state exactly.
      for (int i = 0; i < 32; ++i) ppc_context->r[i] = saved_r[i];
      ppc_context->lr = saved_lr;
      ppc_context->ctr = saved_ctr;
    }
  }
  // --- from-source SI DLL: call InitSameInstrument on the guest thread ---
  // (--si_load_dll --si_init_va). The DLL was already mapped at the stable
  // pre-LaunchModule point in CompleteLaunch (loading a module from this
  // mid-boot callback, with the guest heaps live, races and crashes the
  // loader). What CAN'T run there is guest CODE: InitSameInstrument's inlined
  // RB3E HookFunction rewrites the first instruction of the four SI game sites
  // and wires trampolines -- guest stores that must execute on a live guest
  // context so Xenia's self-modifying-code path invalidates the JIT. So we fire
  // it here, ONCE, well into boot (heaps up, sites resolvable), from this valid
  // guest-thread context, exactly like --si_selftest. Unlike the retired
  // hardcoded old-DLL kHooks table, the detour TARGETS are computed by the DLL's
  // own HookFunction from its link-time addresses -- the from-source path. Full
  // GPR/LR/CTR snapshot+restore keeps the intercepted MemAlloc undisturbed.
  if (cvars::si_load_dll) {
    static std::atomic<bool> s_si_init_fired{false};
    bool expected = false;
    if (m >= 800 && s_si_init_fired.compare_exchange_strong(expected, true)) {
      uint8_t* mb2 = mem_for_emu->virtual_membase();
      if (cvars::si_init_va != 0) {
        // Approach (a): call the DLL's own InitSameInstrument on this live
        // guest thread (targets come from the DLL, nothing hardcoded). NOTE:
        // under Xenia headless this currently faults inside the DLL (Risk #3 --
        // guest-thread ABI/r13 sdata-base); use approach (b) below for the
        // validated install-verify harness.
        uint32_t init_va = static_cast<uint32_t>(cvars::si_init_va);
        // InitSameInstrument's RB3E_PokeBranch/HookFunction are plain guest
        // stores into title .text (game hook sites) and DLL .text (call-stub
        // pokes, trampoline second halves) with NO dcbst/icbi -- on hardware
        // the pages are writable, under Xenia they are mapped read-only and
        // the first SI_POKE_B guest-faults (proven /tmp/rb3-si2: guest crash
        // PC inside RB3E_PokeBranch @0x8402B458+0x40, wedging the hijacked
        // boot thread; the July "r13/sdata ABI" suspicion was wrong). Same
        // cure as the DC3 patch procedure: guest-heap Protect the committed
        // regions to R+W first. At MemAlloc #800 none of the poked sites has
        // been JIT-compiled yet (pre-UI), so lazy compilation picks up the
        // patched bytes and no invalidation is needed.
        auto make_writable = [&](uint32_t lo, uint32_t hi, const char* tag) {
          uint32_t a = lo, pages = 0;
          while (a < hi) {
            auto* heap = mem_for_emu->LookupHeap(a);
            if (!heap) {
              a += 0x10000;
              continue;
            }
            HeapAllocationInfo info = {};
            if (!heap->QueryRegionInfo(a, &info) || !info.region_size) {
              a += 0x10000;
              continue;
            }
            uint32_t region_end = static_cast<uint32_t>(
                std::min<uint64_t>(hi, static_cast<uint64_t>(info.base_address) +
                                           info.region_size));
            if ((info.state & kMemoryAllocationCommit) &&
                region_end > a) {
              heap->Protect(a, region_end - a,
                            kMemoryProtectRead | kMemoryProtectWrite);
              pages += (region_end - a) >> 12;
            }
            a = region_end > a ? region_end : a + 0x10000;
          }
          XELOGW(
              "SI LOADDLL: (a) made [0x{:08X},0x{:08X}) guest-writable for "
              "the DLL's self-installed pokes ({}; ~{} 4K pages)",
              lo, hi, tag, pages);
        };
        make_writable(0x82000000u, 0x83000000u, "title image");
        make_writable(0x84000000u, 0x84860000u, "RB3Enhanced.dll image");
        // The guest-heap walk above is bookkeeping-only and finds ZERO
        // committed pages for the title image (PROT TRACE run /tmp/rb3-si4:
        // the only Protect ever touching H1's page is the loader's
        // prot=0x1, yet QueryRegionInfo reports the range uncommitted -- the
        // title image's real state isn't tracked by the heap page table in
        // this fork). What actually faults the DLL's orig[0] store is the
        // HOST mapping, so unprotect that directly -- same mechanism
        // approach (b) uses per-page, widened to the poke surface.
        // fork-cleanup-review flags raw xe::memory::Protect as bypassing
        // heap bookkeeping and never restoring: INTENTIONAL here. The DLL
        // and its game-call stubs keep poking these ranges for the whole
        // run, so the surface must stay RWX; --si_load_dll is RB3-only and
        // default-off, and the heap page table doesn't track module images
        // in this fork anyway (see PROT TRACE note above).
        xe::memory::Protect(mb2 + 0x82000000u, 0x1000000u,
                            xe::memory::PageAccess::kExecuteReadWrite);
        xe::memory::Protect(mb2 + 0x84000000u, 0x860000u,
                            xe::memory::PageAccess::kExecuteReadWrite);
        XELOGW(
            "SI LOADDLL: (a) host mappings [0x82000000,+16MB) and "
            "[0x84000000,+0x860000) set RWX for the installer's pokes");
        uint64_t saved_r[32];
        for (int i = 0; i < 32; ++i) saved_r[i] = ppc_context->r[i];
        uint64_t saved_lr = ppc_context->lr;
        uint64_t saved_ctr = ppc_context->ctr;
        auto* proc = ppc_context->processor;
        auto* ts = ppc_context->thread_state;
        XELOGW(
            "SI LOADDLL: (a) invoking InitSameInstrument @0x{:08X} on guest "
            "thread (MemAlloc call #{})",
            init_va, m);
        bool ok = proc->Execute(ts, init_va);
        for (int i = 0; i < 32; ++i) ppc_context->r[i] = saved_r[i];
        ppc_context->lr = saved_lr;
        ppc_context->ctr = saved_ctr;
        uint32_t h1 = xe::load_and_swap<uint32_t>(mb2 + 0x8276FA08u);
        uint32_t h2 = xe::load_and_swap<uint32_t>(mb2 + 0x82794740u);
        XELOGW(
            "SI LOADDLL: (a) InitSameInstrument returned ok={}. H1@0x8276FA08="
            "0x{:08X} H2@0x82794740=0x{:08X}",
            ok, h1, h2);
      } else {
        // Approach (b), validated harness path: host-emulate RB3E's
        // first-instruction relocating detour, writing `b <dll-hook>` at each SI
        // game site -- HERE, mid-boot, on a live guest thread. The .text pages
        // are now committed and stable, so (unlike a pre-LaunchModule write, which
        // the loader's lazy .text commit reverts) the write is seen by the guest
        // and the verifier. Targets are the FROM-SOURCE DLL hook VAs from the
        // Phase-2 link map (checkpoints/rb3dx-finish/mapdeploy.json hookVAs), not
        // the retired old-spliced-DLL constants. Trampoline callbacks into
        // <hook>Orig are not populated (behavioral Phase-5 work); the gameplay
        // sites aren't reached before the hub, so boot-to-ceiling is unaffected.
        struct SiHook {
          uint32_t site, hook;
          const char* tag;
        };
        SiHook kFromSrcHooks[4] = {
            {0x826684C0u, 0x840191A8u, "IsActive"},
            {0x825B6488u, 0x840191E8u, "ResolveWaitStates"},
            {0x8276FA08u, 0x84019780u, "H1 ProcessConfig"},
            {0x82794740u, 0x84019450u, "H2 RecalcGemList"},
        };
        // The hook VAs above are the July 2026 fromsource.dll layout and go
        // stale on every DLL relink; --si_hook_vas carries the current
        // build's addresses (site order fixed: IsActive, ResolveWaitStates,
        // ProcessConfig, RecalcGemList).
        if (!cvars::si_hook_vas.empty()) {
          uint32_t vas[4] = {};
          int n = 0;
          const char* p = cvars::si_hook_vas.c_str();
          while (n < 4 && *p) {
            char* endp = nullptr;
            vas[n] = static_cast<uint32_t>(std::strtoul(p, &endp, 16));
            if (endp == p) break;
            ++n;
            p = (*endp == ',') ? endp + 1 : endp;
          }
          if (n == 4) {
            for (int i = 0; i < 4; ++i) kFromSrcHooks[i].hook = vas[i];
            XELOGW(
                "SI LOADDLL: (b) hook VAs overridden from --si_hook_vas: "
                "0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}",
                vas[0], vas[1], vas[2], vas[3]);
          } else {
            XELOGE(
                "SI LOADDLL: (b) --si_hook_vas parsed {} of 4 required VAs "
                "('{}') -- falling back to the stale 2026-07 constants",
                n, cvars::si_hook_vas);
          }
        }
        const size_t host_pg = xe::memory::page_size();
        for (const auto& h : kFromSrcHooks) {
          uint8_t* host_addr = mb2 + h.site;
          uint8_t* host_page = reinterpret_cast<uint8_t*>(
              reinterpret_cast<uintptr_t>(host_addr) & ~(host_pg - 1));
          xe::memory::Protect(host_page, host_pg,
                              xe::memory::PageAccess::kExecuteReadWrite);
          uint32_t br = 0x48000000u | ((h.hook - h.site) & 0x03FFFFFCu);
          xe::store_and_swap<uint32_t>(mb2 + h.site, br);
          XELOGW(
              "SI LOADDLL: (b) mid-boot detour {} 0x{:08X} -> 0x{:08X} "
              "(word=0x{:08X}, MemAlloc #{})",
              h.tag, h.site, h.hook, br, m);
        }
        uint32_t h1 = xe::load_and_swap<uint32_t>(mb2 + 0x8276FA08u);
        uint32_t h2 = xe::load_and_swap<uint32_t>(mb2 + 0x82794740u);
        XELOGW(
            "SI LOADDLL: (b) from-source detours installed mid-boot. "
            "H1@0x8276FA08=0x{:08X} H2@0x82794740=0x{:08X} (verify with "
            "--si_hook_verify)",
            h1, h2);
      }
    }
  }
  if (m <= 5) {
    XELOGI(
        "RB3DX ALLOC PROBE: MemAlloc warmup #{} size=0x{:08X} align={} "
        "callerLR(r12)=0x{:08X} sp=0x{:08X}",
        m, size, static_cast<int32_t>(static_cast<uint32_t>(ppc_context->r[4])),
        static_cast<uint32_t>(ppc_context->r[12]), sp);
  }
  bool suspicious = (size & 0xFF000000u) != 0;
  bool big = size >= 0x00400000u;  // >=4MB: context even when top byte clean
  if (!suspicious && !big) {
    return;
  }
  if (s_reports.fetch_add(1, std::memory_order_relaxed) >= 64) {
    return;  // cap log volume; the first hits are the interesting ones
  }
  uint32_t align = static_cast<uint32_t>(ppc_context->r[4]);
  uint32_t caller_lr = static_cast<uint32_t>(ppc_context->r[12]);
  Memory* memory = mem_for_emu;
  uint8_t* membase = base;
  auto readable = [&](uint32_t addr) -> bool {
    if (addr < 0x1000 || (addr & 3)) {
      return false;
    }
    auto* heap = memory->LookupHeap(addr);
    if (!heap) {
      return false;
    }
    uint32_t prot = 0;
    if (!heap->QueryProtect(addr, &prot)) {
      return false;
    }
    return (prot & kMemoryProtectRead) != 0;
  };
  auto load32 = [&](uint32_t addr) -> uint32_t {
    return xe::load_and_swap<uint32_t>(membase + addr);
  };
  XELOGE(
      "RB3DX ALLOC PROBE: {} call #{} size=0x{:08X} ({}) align={} "
      "callerLR=0x{:08X} sp=0x{:08X}",
      suspicious ? "SUSPICIOUS" : "BIG", m, size, size,
      static_cast<int32_t>(align), caller_lr, sp);
  // Guest stack backchain walk. Frame convention (MSVC Xenon):
  //   prologue = mflr r12 ; bl __savegprlr_N (stw r12,-8(r1)) ; stwu r1,-X(r1)
  // so from a current SP: entry_sp = [sp], saved return addr = [entry_sp - 8].
  uint32_t cur = sp;
  for (int i = 0; i < 10; ++i) {
    if (!readable(cur)) {
      break;
    }
    uint32_t prev = load32(cur);
    if (prev <= cur || prev - cur > 0x40000 || !readable(prev - 8)) {
      break;
    }
    uint32_t ret = load32(prev - 8);
    XELOGE("RB3DX ALLOC PROBE:   frame[{}] entry_sp=0x{:08X} ret=0x{:08X}", i,
           prev, ret);
    cur = prev;
  }
  // Fallback: raw code-pointer scan of the caller stack window (covers
  // leaf frames / broken backchains).
  for (uint32_t a = sp & ~3u; a < sp + 0x180; a += 4) {
    if (!readable(a)) {
      break;
    }
    uint32_t v = load32(a);
    if (v >= 0x82000000 && v < 0x82F00000) {
      XELOGE("RB3DX ALLOC PROBE:   [sp+0x{:03X}]=0x{:08X}", a - sp, v);
    }
  }
  // Full GPR snapshot (context is synced at extern-call time).
  for (int i = 0; i < 32; i += 8) {
    XELOGE(
        "RB3DX ALLOC PROBE:   r{:<2}-r{:<2} {:08X} {:08X} {:08X} {:08X} "
        "{:08X} {:08X} {:08X} {:08X}",
        i, i + 7, static_cast<uint32_t>(ppc_context->r[i]),
        static_cast<uint32_t>(ppc_context->r[i + 1]),
        static_cast<uint32_t>(ppc_context->r[i + 2]),
        static_cast<uint32_t>(ppc_context->r[i + 3]),
        static_cast<uint32_t>(ppc_context->r[i + 4]),
        static_cast<uint32_t>(ppc_context->r[i + 5]),
        static_cast<uint32_t>(ppc_context->r[i + 6]),
        static_cast<uint32_t>(ppc_context->r[i + 7]));
  }
  // Live-code integrity dump: detect RB3DX runtime self-patching of the
  // GetFileSize path (xapilib) that feeds SaveLoadManager::mSaveSize.
  struct DumpSpec {
    const char* name;
    uint32_t addr;
    int words;
  };
  const DumpSpec dumps[] = {
      {"GetFileSize@82840070", 0x82840070, 8},
      {"QuerySize64@8284BC68", 0x8284BC68, 14},
      {"ThreadGetFileSize.store@827DA84C", 0x827DA84C, 4},
      {"NtQIF.thunk@82C4C46C", 0x82C4C46C, 4},
      {"NtDispatchTbl@82C7AA78+0x1C", 0x82C7AA98, 4},
      {"gNtDispatchPtr@82C7AAA8", 0x82C7AAA8, 1},
  };
  for (const auto& d : dumps) {
    if (!readable(d.addr)) {
      XELOGE("RB3DX ALLOC PROBE:   {} UNREADABLE", d.name);
      continue;
    }
    std::string words;
    for (int i = 0; i < d.words; ++i) {
      words += fmt::format(" {:08X}", load32(d.addr + i * 4));
    }
    XELOGE("RB3DX ALLOC PROBE:   {}:{}", d.name, words);
  }
  // Recover the SaveLoadManager 'this' (r30 of the frame that called
  // operator new): its __savegprlr_23 spill sits at frame0_entry_sp-0x50..
  // -0x10 (std r23..r31), so saved r30 = u64 at entry_sp-0x18.
  if (caller_lr == 0x827BD028 && readable(sp)) {  // via operator new
    uint32_t f0_entry = load32(sp);               // operator new's caller sp
    if (readable(f0_entry) && readable(load32(f0_entry) - 0x18)) {
      uint32_t f1_entry = load32(f0_entry);  // state machine entry sp
      uint32_t obj = load32(f1_entry - 0x18 + 4);  // low half of saved r30
      XELOGE("RB3DX ALLOC PROBE:   frame1 saved r30 (state-machine this) = "
             "0x{:08X}",
             obj);
      if (obj >= 0x10000 && readable(obj + 0x50)) {
        XELOGE(
            "RB3DX ALLOC PROBE:   obj+0x50(name)=0x{:08X} +0x54(mSaveSize)="
            "0x{:08X} +0x58=0x{:08X} +0x5C(mCacheID)=0x{:08X} "
            "+0x60(mCache)=0x{:08X} +0x64(mData)=0x{:08X}",
            load32(obj + 0x50), load32(obj + 0x54), load32(obj + 0x58),
            load32(obj + 0x5C), load32(obj + 0x60), load32(obj + 0x64));
      }
    }
  }
}

// RB3DX (0x45410914) offline single-local-host join completion
// (--rb3dx_offline_join).
//
// Overrides the guest NetSession::IsHost() @ 0x823CECE0. Headless with
// XNet/XSession/Quazal stubbed, a Quazal session object still gets created
// (mQNet @ this+0x70 != 0) but this machine is not the session's "duplication
// master", so the real IsHost() (mQNet!=0 -> Quazal::Session::GetInstance()->
// IsADuplicationMaster()) returns false. That drives NetSession::AddLocalUser
// @ 0x823D2468 down its NON-host ELSE branch — SetState(kRequestingNewUser=7);
// build an AddUserRequestMsg; TheNetMessenger.DeliverMsg over the dead network
// — instead of the host branch (AddLocalToSession + fire AddUserResultMsg(1)
// synchronously). The AddUserResponseMsg never arrives, so SessionMgr never
// fires AddLocalUserResultMsg, the overshell slot never reaches allowing-input,
// overshell_allowing_input(TRUE) never fires, and splash_screen's
// kSplashScreen_WaitOvershell gate never advances to main_hub. This is the SAME
// gate the RB3 native port hit and fixed by making IsHost() return true offline
// ("single local machine", rb3_netsession_native.cpp:195).
//
// We replicate the real IsHost() using the target's own field reads (mState @
// this+0x68 — both offsets and the state constants are taken directly from the
// guest IsHost disassembly) and diverge ONLY where the real code would consult
// the (non-existent) Quazal duplication master: offline the local machine IS
// the host, so return true. kRequestingNewUser(7) and the joining states (3..6)
// are returned as not-host exactly as the real function would, preserving the
// session state-machine semantics during any transient online-session setup.
void Rb3dxIsHostOfflineExtern(cpu::ppc::PPCContext* ppc_context,
                              kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) {
    return;
  }
  uint8_t* base = kernel_state->memory()->virtual_membase();
  uint32_t self = static_cast<uint32_t>(ppc_context->r[3]);  // NetSession* this
  uint32_t mState = xe::load_and_swap<uint32_t>(base + self + 0x68);
  // kRequestingNewUser (7) or joining (3..6): genuinely not host — preserve.
  bool not_host = (mState == 7 || (mState >= 3 && mState <= 6));
  static std::atomic<uint32_t> s_calls{0};
  uint32_t n = s_calls.fetch_add(1, std::memory_order_relaxed);
  if (n < 8) {
    XELOGI(
        "RB3DX offline-join: IsHost hit #{} this=0x{:08X} mState={} "
        "lr=0x{:08X} -> host={}",
        n, self, mState, static_cast<uint32_t>(ppc_context->lr),
        not_host ? 0 : 1);
  }
  if (not_host) {
    ppc_context->r[3] = 0;
    return;
  }
  // Idle/normal (incl. kCreatingHostSession/kRegisteringHostSession) offline:
  // this machine is the host. The real code asks Quazal only when mQNet!=0
  // (this+0x70), which headless returns false and causes the join stall.
  ppc_context->r[3] = 1;
}

// RB3 TU5 clean-boot: no-op CharSync::UpdateCharCache (0x82564698, void ret).
// Register as a guest-function override so the band-member preview char cache
// is never streamed -- its extras milos (world/shared/extras/male_extras0N.milo)
// are queued kLoadFront at the head of the single main-thread load FIFO and
// head-of-line-block the splash_screen panels behind them, freezing the whole
// cooperative-loader frame loop ~13s in. Empty body = the guest fn returns
// immediately, exactly like the rb3 native port's RB3_NO_CHAR_PREVIEW.
void Rb3NoCharPreviewExtern(cpu::ppc::PPCContext* ppc_context,
                            kernel::KernelState* kernel_state) {
  static std::atomic<uint32_t> s_calls{0};
  uint32_t n = s_calls.fetch_add(1, std::memory_order_relaxed);
  if (n < 4 && ppc_context) {
    XELOGI("RB3 no-char-preview: UpdateCharCache hit #{} suppressed (lr=0x{:08X})",
           n, static_cast<uint32_t>(ppc_context->lr));
  }
  // void return; do not touch r[3].
}

// RB3DX / RB3 TU5 first-boot calibration skip (--rb3dx_skip_calibration).
//
// The splash flow, at kSplashScreen_EndOvershell, evaluates the DTA condition
// `{! {profile_mgr get_has_seen_first_time_calibration}}` (ui/splash/splash.dta):
// false-on-a-fresh-profile routes to `push_screen first_time_calibration` (the
// interactive cal_audio_screen A/V-latency calibration, uncompletable headless
// with null audio + fixed-time scripted input); true routes to
// `goto_screen main_hub_screen`. The message handler (ProfileMgr::Handle) reads
// the bool member mHasSeenFirstTimeCalibration @+0x54 INLINE (verified: the
// standalone GetHasSeenFirstTimeCalibration() leaf getter is never entered at
// boot), so a guest-function override cannot intercept it. Instead we set the
// backing byte directly.
//
// This thread resolves the `profile_mgr` object (the game's ProfileMgr /
// TheProfileMgr singleton) through the same ObjectDir::sMainDir @0x82E054B8
// name-hash walk the UI probe uses, then writes 1 to obj+0x54 repeatedly until
// the decision is taken (and beyond — cheap, idempotent, and robust to the
// GlobalOptions load re-zeroing the field before EndOvershell fires). The
// member offset (0x54) is the RB3 xbox360 layout verified from the ProfileMgr
// ctor (rb3-xenon). Guest-memory write is confined to this single bool byte;
// title-gated + default-off so DC3-inert.

// Probe-thread ownership (fork-cleanup-review.md C10). These samplers used to
// be detached std::threads with for(;;) bodies capturing Memory*; ~Emulator()
// destroyed memory_ under them mid-LookupHeap — a guaranteed shutdown
// use-after-free on every probe-enabled run. They are now owned here: spawned
// via Rb3dxSpawnProbeThread(), polling Rb3dxProbeSleep() instead of a bare
// sleep_for, and joined from Emulator::TerminateTitle() / ~Emulator().
static std::mutex s_rb3dx_probe_thread_mutex;
static std::vector<std::thread> s_rb3dx_probe_threads;
static std::atomic<bool> s_rb3dx_probe_threads_stop{false};

static bool Rb3dxProbeThreadsShouldStop() {
  return s_rb3dx_probe_threads_stop.load(std::memory_order_relaxed);
}

// Sleep in short slices so a stopping emulator never waits out a full probe
// period. Returns false (caller should exit) when a stop was requested.
static bool Rb3dxProbeSleep(uint64_t ms) {
  for (uint64_t waited = 0; waited < ms; waited += 250) {
    if (Rb3dxProbeThreadsShouldStop()) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(
        std::min<uint64_t>(250, ms - waited)));
  }
  return !Rb3dxProbeThreadsShouldStop();
}

template <typename Fn>
static void Rb3dxSpawnProbeThread(Fn&& fn) {
  std::lock_guard<std::mutex> lock(s_rb3dx_probe_thread_mutex);
  s_rb3dx_probe_threads.emplace_back(std::forward<Fn>(fn));
}

static void Rb3dxJoinProbeThreads() {
  s_rb3dx_probe_threads_stop.store(true, std::memory_order_relaxed);
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> lock(s_rb3dx_probe_thread_mutex);
    threads.swap(s_rb3dx_probe_threads);
  }
  for (auto& t : threads) {
    if (t.joinable()) {
      t.join();
    }
  }
}

static void Rb3dxSkipCalibrationPokeThread(Memory* memory) {
  using xe::load_and_swap;
  uint8_t* base = memory->virtual_membase();
  auto prot = [&](uint32_t addr, uint32_t flag) -> bool {
    if (addr < 0x1000) return false;
    auto* heap = memory->LookupHeap(addr);
    if (!heap) return false;
    uint32_t p = 0;
    if (!heap->QueryProtect(addr, &p)) return false;
    return (p & flag) != 0;
  };
  auto readable = [&](uint32_t a) { return prot(a, kMemoryProtectRead); };
  auto writable = [&](uint32_t a) { return prot(a, kMemoryProtectWrite); };
  auto r32 = [&](uint32_t a) -> uint32_t {
    return readable(a) ? load_and_swap<uint32_t>(base + a) : 0;
  };
  auto rstr = [&](uint32_t a) -> std::string {
    if (!readable(a)) return std::string();
    std::string s;
    for (int i = 0; i < 48; ++i) {
      if (!readable(a + i)) break;
      char c = static_cast<char>(*(base + a + i));
      if (!c) return s;
      if (c < 0x20 || c > 0x7E) return std::string();
      s += c;
    }
    return s;
  };
  const uint32_t kMainDirPtr = 0x82E054B8;
  // The ObjectDir stores the Hmx::Object VBASE pointer (object tail) for the
  // MsgSource-derived ProfileMgr, so mHasSeenFirstTimeCalibration (this+0x54)
  // sits at a NEGATIVE offset from that pointer. Empirically pinned: on
  // first_time_calibration.enter the setter flips two adjacent bytes together
  // at obj-0x90 and obj-0x74 (spacing 0x1C == 0x54-0x38 = flag vs
  // mGlobalOptionsDirty), fixing this = obj-0xC8, so the flag byte is obj-0x74
  // (a mapping cross-checked by obj-0x60 == this+0x68 == mOverscan).
  const int32_t kFlagOffset = -0x74;  // ProfileMgr::mHasSeenFirstTimeCalibration
  uint32_t profile_mgr = 0;
  uint32_t pokes = 0;
  for (;;) {
    // Resolve profile_mgr once via the main-dir name hash.
    if (!profile_mgr) {
      uint32_t dir = r32(kMainDirPtr);
      if (dir) {
        uint32_t entries = r32(dir + 0x8);
        int size = static_cast<int>(r32(dir + 0xC));
        if (entries && size > 0 && size < 200000) {
          for (int i = 0; i < size; ++i) {
            uint32_t name_p = r32(entries + i * 8);
            uint32_t obj = r32(entries + i * 8 + 4);
            if (!name_p || !obj) continue;
            if (rstr(name_p) == "profile_mgr") {
              profile_mgr = obj;
              XELOGE(
                  "RB3DX skip-calibration: resolved profile_mgr obj=0x{:08X} "
                  "(flag byte @0x{:08X})",
                  profile_mgr, profile_mgr + kFlagOffset);
              break;
            }
          }
        }
      }
    }
    // Poke the flag byte to 1 while it is writable (idempotent; robust to the
    // GlobalOptions load or a transient re-zero before the splash decision).
    if (profile_mgr) {
      uint32_t flag_addr = profile_mgr + kFlagOffset;
      if (writable(flag_addr)) {
        uint8_t before = *(base + flag_addr);
        *(base + flag_addr) = 1;
        if (pokes < 8 || (before == 0 && pokes < 200)) {
          XELOGI("RB3DX skip-calibration: poke #{} [0x{:08X}] {} -> 1", pokes,
                 flag_addr, before);
        }
        ++pokes;
      }
    }
    if (!Rb3dxProbeSleep(150)) return;
  }
}

// Same-instrument static-patch runtime observer (--si_probe).
//
// Passive, read-only. Logs the runtime bytes of the static SI patch that our
// default_tu5_patched.xex bakes into guest RAM, so we can prove (a) the cave
// survived XEX decrypt/load, (b) the IsActive detour word is present, and
// (c) whether a runtime writer zeroes the enable flag. Addresses (retarget /
// wave2 pins.json): enable flag @0x82C8AAA0 (expect 1), IsActive detour site
// @0x826684C0 (expect 0x48621BC0 = b 0x82c8a080), cave stub @0x82C8A080, cave
// base @0x82C8A000. Title-gated + default-off => DC3-inert; no writes.
static void Rb3dxSiProbeThread(Memory* memory) {
  using xe::load_and_swap;
  uint8_t* base = memory->virtual_membase();
  auto readable = [&](uint32_t addr) -> bool {
    if (addr < 0x1000) return false;
    auto* heap = memory->LookupHeap(addr);
    if (!heap) return false;
    uint32_t prot = 0;
    if (!heap->QueryProtect(addr, &prot)) return false;
    return (prot & kMemoryProtectRead) != 0;
  };
  auto r32 = [&](uint32_t a) -> uint32_t {
    return readable(a) ? load_and_swap<uint32_t>(base + a) : 0xEEEEEEEEu;
  };
  auto r8 = [&](uint32_t a) -> int {
    return readable(a) ? static_cast<int>(*(base + a)) : -1;
  };
  const uint32_t kFlag = 0x82C8AAA0;
  const uint32_t kIsActiveSite = 0x826684C0;
  const uint32_t kCaveStub = 0x82C8A080;
  const uint32_t kCaveBase = 0x82C8A000;
  uint32_t n = 0;
  int last_flag = -2;
  uint32_t last_flagw = 0xEEEEEEEEu;
  uint32_t last_detour = 0;
  bool dumped = false;
  for (;;) {
    int flag = r8(kFlag);
    uint32_t flagw = r32(kFlag);
    uint32_t detour = r32(kIsActiveSite);
    uint32_t cave_stub = r32(kCaveStub);
    uint32_t cave_base = r32(kCaveBase);
    // One-time hex dump of the IsActive cave stub (0x82C8A080..0x82C8A0C0) so
    // the flag-read instruction (lwz vs lbz) can be disassembled offline.
    if (!dumped && cave_stub != 0xEEEEEEEEu) {
      for (uint32_t off = 0; off < 0x40; off += 0x10) {
        XELOGI("SI PROBE stub[0x{:08X}]: {:08X} {:08X} {:08X} {:08X}",
               kCaveStub + off, r32(kCaveStub + off), r32(kCaveStub + off + 4),
               r32(kCaveStub + off + 8), r32(kCaveStub + off + 12));
      }
      // Original IsActive body (0x826684C0.. ; +0 is now the detour word).
      for (uint32_t off = 0; off < 0x80; off += 0x10) {
        XELOGI("SI PROBE orig[0x{:08X}]: {:08X} {:08X} {:08X} {:08X}",
               0x826684C0 + off, r32(0x826684C0 + off),
               r32(0x826684C0 + off + 4), r32(0x826684C0 + off + 8),
               r32(0x826684C0 + off + 12));
      }
      // Trampoline region (stolen instr + jump back), just past the flag.
      for (uint32_t off = 0; off < 0x40; off += 0x10) {
        XELOGI("SI PROBE tramp[0x{:08X}]: {:08X} {:08X} {:08X} {:08X}",
               0x82C8AAC0 + off, r32(0x82C8AAC0 + off),
               r32(0x82C8AAC0 + off + 4), r32(0x82C8AAC0 + off + 8),
               r32(0x82C8AAC0 + off + 12));
      }
      dumped = true;
    }
    bool changed = (flag != last_flag) || (detour != last_detour) ||
                   (flagw != last_flagw);
    if (n < 4 || changed || (n % 15) == 0) {
      XELOGI(
          "SI PROBE[{}]: flagByte@0x82C8AAA0={} flagWord@0x82C8AAA0=0x{:08X} "
          "detour@0x826684C0=0x{:08X}(expect 0x48621BC0) "
          "caveStub@0x82C8A080=0x{:08X} caveBase@0x82C8A000=0x{:08X}{}",
          n, flag, flagw, detour, cave_stub, cave_base,
          changed && n ? "  <-- CHANGED" : "");
    }
    last_flag = flag;
    last_flagw = flagw;
    last_detour = detour;
    ++n;
    if (!Rb3dxProbeSleep(500)) return;
  }
}

// RB3Enhanced-DLL same-instrument GAMEPLAY-hook install verifier
// (--si_hook_verify).
//
// Passive, read-only. Where --si_probe watches the DEAD static .data cave, this
// watches the LIVE DLL-delivery path: the two runtime HookFunction detours the
// RB3Enhanced.dll installs at the H1/H2 gameplay sites once it is loaded at
// 0x84000000. RB3E's HookFunction rewrites the FIRST instruction of the target
// to `b <stub-in-DLL>` (a first-instruction relocating detour). So the pass
// condition is purely observable in guest RAM: word[site] decodes to a `b`
// whose target is inside DLL image space and within +/-32MB (PPC I-form b
// reach). Stock (no DLL / hooks not installed) = 0x7D8802A6 (mflr r12).
//   H1 PlayerTrackConfigList::ProcessConfig @0x8276FA08 (kills vector[-1] crash)
//   H2 TrackWatcherImpl::RecalcGemList     @0x82794740 (per-watcher gem clone)
// Title-gated + default-off => DC3-inert; no writes.
static void Rb3dxSiHookVerifyThread(Memory* memory) {
  using xe::load_and_swap;
  uint8_t* base = memory->virtual_membase();
  const uint32_t kH1 = 0x8276FA08;  // ProcessConfig
  const uint32_t kH2 = 0x82794740;  // RecalcGemList
  const uint32_t kDllLo = 0x84000000;
  // From-source RB3Enhanced.dll packed image is ~0x850000 (boot.xex 8.7 MB,
  // image_size 0x850000 per mapdeploy.json); its hook stubs sit at ~0x84019xxx
  // and its config at 0x84829xxx. The reference-DLL 0x40000 window is too tight
  // for the from-source layout's config VA, so widen to the full mapped image.
  const uint32_t kDllHi = 0x84850000;
  const uint32_t kStockPrologue = 0x7D8802A6;  // mflr r12
  auto readable = [&](uint32_t a) -> bool {
    if (a < 0x1000) return false;
    auto* heap = memory->LookupHeap(a);
    if (!heap) return false;
    // Gate on COMMITTED state, not protect bits: /tmp/rb3-si2 showed the
    // title's .text page-table entries flip to protect=0x0 once the SI DLL
    // is loaded (guest-side protect bookkeeping), while the host mapping
    // stays readable -- the guest-thread detour writes at the same
    // addresses read real words throughout. What actually SIGSEGVs a host
    // probe read is an uncommitted/unmapped page, so that is the check.
    HeapAllocationInfo info = {};
    if (!heap->QueryRegionInfo(a, &info)) return false;
    return (info.state & kMemoryAllocationCommit) != 0;
  };
  auto r32 = [&](uint32_t a) -> uint32_t {
    return readable(a) ? load_and_swap<uint32_t>(base + a) : 0xEEEEEEEEu;
  };
  // Decode a PPC I-form branch word at `site`; returns target VA or 0 if not a
  // primary-opcode-18 branch. Handles AA (absolute) + sign-extended 26-bit LI.
  auto branch_target = [](uint32_t word, uint32_t site) -> uint32_t {
    if ((word >> 26) != 18u) return 0;               // not b/bl/ba/bla
    int32_t li = static_cast<int32_t>(word & 0x03FFFFFCu);
    if (li & 0x02000000) li |= static_cast<int32_t>(0xFC000000u);  // sign-ext
    bool aa = (word & 0x2) != 0;
    return aa ? static_cast<uint32_t>(li)
              : static_cast<uint32_t>(static_cast<int64_t>(site) + li);
  };
  auto classify = [&](const char* tag, uint32_t site) {
    uint32_t w = r32(site);
    uint32_t tgt = branch_target(w, site);
    const char* verdict;
    if (w == 0xEEEEEEEEu) {
      verdict = "UNREADABLE (image not loaded here?)";
    } else if (w == kStockPrologue) {
      verdict = "STOCK mflr r12 -- NOT hooked (DLL not loaded / init skipped)";
    } else if (tgt == 0) {
      verdict = "NON-BRANCH first word -- unexpected (patched but not a b?)";
    } else {
      bool in_dll = (tgt >= kDllLo && tgt < kDllHi);
      int64_t rel = static_cast<int64_t>(tgt) - static_cast<int64_t>(site);
      bool in_reach = rel > -33554432 && rel < 33554432;  // +/-32MB
      if (in_dll && in_reach)
        verdict = "PASS -- b into DLL space, in +/-32MB reach";
      else if (in_dll)
        verdict = "b into DLL space but OUT OF +/-32MB REACH (impossible?)";
      else
        verdict = "b to NON-DLL target (unexpected detour destination)";
      XELOGW(
          "SI HOOKVERIFY {} @0x{:08X}: word=0x{:08X} b->0x{:08X} (rel={}) [{}]",
          tag, site, w, tgt, static_cast<long long>(rel), verdict);
      return;
    }
    XELOGW("SI HOOKVERIFY {} @0x{:08X}: word=0x{:08X} [{}]", tag, site, w,
           verdict);
  };
  uint32_t n = 0;
  for (;;) {
    if (n < 3 || (n % 20) == 0) {
      // Is anything mapped at the DLL base? (loaded-module smoke check.)
      uint32_t dll_head = r32(kDllLo);
      XELOGI("SI HOOKVERIFY[{}]: DLL-base word@0x84000000=0x{:08X} ({})", n,
             dll_head,
             dll_head == 0xEEEEEEEEu ? "unmapped -- DLL NOT loaded"
                                     : "mapped");
      classify("H1", kH1);
      classify("H2", kH2);
    }
    ++n;
    if (!Rb3dxProbeSleep(500)) return;
  }
}

// RB3DX main_hub load-stall investigation (--rb3dx_ui_probe).
//
// Passive, read-only sampler thread. Anchors (all TU5/RB3DX-runtime-proven by
// RB3Enhanced's ports_xbox360.h, which patches this exact xex):
//   TheBandUI (BandUI instance)     = 0x82DFD2B0
//   ObjectDir::sMainDir (ObjectDir*) = 0x82E054B8
// Layouts (rb3-xenon decomp, Ghidra-verified for RB3-360 where noted):
//   UIManager (virtual Hmx::Object base; vftable@0, vbtbl@8, vbase tail):
//     mTransitionState @+0x10, mCurrentScreen @+0x2C (matches RB3E
//     currentScreen), mTransitionScreen @+0x30.
//   UIScreen (NON-virtual Hmx::Object base): name char* @+0x18 (matches RB3E
//     screen_name), mPanelList std::list<PanelRef> @+0x2C = embedded dummy
//     {next,prev}; node = {next@0, prev@4, PanelRef@8 = {UIPanel* @+8,
//     mActive @+0xC, mAlwaysLoad @+0xD, mLoaded @+0xE}}.
//   UIPanel (VIRTUAL Hmx::Object base; vfptr@0, vbptr@4): mDir @+0x8,
//     mLoader @+0xC, mLoaded @+0x1C, mState @+0x20 (0=kUnloaded 1=kUp
//     2=kDown), mLoadRefs @+0x28. Name via vbase: vbp=[this+4],
//     vbase=this+4+[vbp+4], name=[vbase+0x18].
//   ObjectDir: KeylessHash @+0x8 = {Entry* mEntries@+0, int mSize@+4};
//     Entry = {const char* name, Hmx::Object* obj} (8 bytes).
//   SaveLoadManager ("saveload_mgr" in main dir): mActivated @+0x1C,
//     mInitialLoadNotDone @+0x1D, mState @+0x24, mWaiting @+0x69,
//     unk7c @+0x7C, mAction @+0x80 (layout cross-checked by the alloc probe's
//     +0x5C/+0x60/+0x64 dumps during the OOM investigation).
//   NetSync ("net_sync"): unk1c @+0x1C, mDestinationScreen @+0x20,
//     mDestinationDepth @+0x24, unk28 @+0x28, unk29 @+0x29,
//     mUILockStep @+0x2C -> LockStepMgr: mLockMachine @+0x1C (InLock() =
//     mLockMachine != 0), mWaitList.mList vector {begin@+0x20, end@+0x24},
//     mHasResponded @+0x28, mLockSuccess @+0x29.
static void Rb3dxUiProbeThread(Memory* memory,
                               kernel::KernelState* kernel_state,
                               cpu::Processor* processor) {
  using xe::load_and_swap;
  uint8_t* base = memory->virtual_membase();
  auto readable = [&](uint32_t addr) -> bool {
    if (addr < 0x1000) return false;
    // The title image and any user DLL image are not tracked by the guest
    // heap page table (QueryProtect reads 0x0 / QueryRegionInfo reads
    // uncommitted the moment a user module loads -- see jit-fault wiki 8f),
    // which blinded every probe read for the whole --si_load_dll family of
    // runs. Their host pages are resident for the entire run (the JIT
    // executes from them), so treat those windows as readable and gate the
    // rest on commit state.
    if ((addr >= 0x82000000u && addr < 0x83000000u) ||
        (addr >= 0x84000000u && addr < 0x84860000u)) {
      return true;
    }
    auto* heap = memory->LookupHeap(addr);
    if (!heap) return false;
    uint32_t prot = 0;
    if (heap->QueryProtect(addr, &prot) && (prot & kMemoryProtectRead)) {
      return true;
    }
    HeapAllocationInfo info = {};
    if (!heap->QueryRegionInfo(addr, &info)) return false;
    return (info.state & kMemoryAllocationCommit) != 0;
  };
  auto r32 = [&](uint32_t a) -> uint32_t {
    return readable(a) ? load_and_swap<uint32_t>(base + a) : 0;
  };
  auto r8 = [&](uint32_t a) -> uint32_t {
    return readable(a) ? *(base + a) : 0;
  };
  auto rstr = [&](uint32_t a) -> std::string {
    if (!readable(a)) return "<unreadable>";
    std::string s;
    for (int i = 0; i < 48; ++i) {
      if (!readable(a + i)) break;
      char c = static_cast<char>(*(base + a + i));
      if (!c) return s;
      if (c < 0x20 || c > 0x7E) return "<binary>";
      s += c;
    }
    return s + "...";
  };
  auto panel_vbase = [&](uint32_t panel) -> uint32_t {
    uint32_t vbp = r32(panel + 4);
    if (!vbp) return 0;
    uint32_t delta = r32(vbp + 4);
    if (delta == 0 || delta > 0x400) return 0;
    return panel + 4 + delta;
  };
  const uint32_t kTheBandUI = 0x82DFD2B0;
  const uint32_t kMainDirPtr = 0x82E054B8;
  uint32_t saveload_obj = 0, netsync_obj = 0, session_obj = 0;
  uint32_t overshell_obj = 0;
  int sample = 0;
  for (;;) {
    if (!Rb3dxProbeSleep(2000)) return;
    ++sample;
    // --- UIManager / BandUI ---
    uint32_t ts = r32(kTheBandUI + 0x10);
    uint32_t cur = r32(kTheBandUI + 0x2C);
    uint32_t trans = r32(kTheBandUI + 0x30);
    std::string cur_name = cur ? rstr(r32(cur + 0x18)) : "<null>";
    std::string trans_name = trans ? rstr(r32(trans + 0x18)) : "<null>";
    XELOGE(
        "RB3DX UI PROBE[{}]: transState={} curScreen=0x{:08X}'{}' "
        "transScreen=0x{:08X}'{}'",
        sample, ts, cur, cur_name, trans, trans_name);
    // --- Main-thread guest stack sample (clean-TU5 flow bring-up). The TU5
    // intro_movie transition never completes and nothing in the probe surface
    // says where the main loop actually spins, so sample thid 6's live guest
    // context and walk the back-chain. Unsynchronised read of a running
    // thread's context: individual samples may tear; consistent repetition
    // across samples is the signal, single odd frames are not.
    if (processor) {
      // Definitive tid=6 (Main XThread) liveness probe every sample: is main
      // Alive(0)/Waiting(1)/Exited(2)/Zombie(3), or entirely ABSENT from the
      // registry? Distinguishes "main busy-spins" from "main already exited and
      // a worker spins" -- the linchpin for the clean-TU5 wedge diagnosis.
      {
        auto* i6 = processor->QueryThreadDebugInfo(6);
        if (!i6) {
          XELOGE("RB3DX UI PROBE[{}]: TID6 ABSENT (erased from registry)",
                 sample);
        } else {
          // Also walk tid6's guest stack EVERY sample so we can see the exact
          // frame it exits from (the teardown happens in one ~2s window between
          // two full-sweep ticks). chain = saved-LR back-chain from sp.
          std::string chain;
          if (i6->thread && i6->thread->thread_state()) {
            auto* c6 = i6->thread->thread_state()->context();
            if (c6) {
              uint32_t s = static_cast<uint32_t>(c6->r[1]);
              chain = fmt::format(" lr={:08X}", static_cast<uint32_t>(c6->lr));
              for (int i = 0; i < 12 && s; ++i) {
                uint32_t bc = r32(s);
                if (bc <= s || bc - s > 0x100000) break;
                chain += fmt::format(" {:08X}", r32(bc - 8));
                s = bc;
              }
            }
          }
          XELOGE("RB3DX UI PROBE[{}]: TID6 state={} thread_ptr={} chain:{}",
                 sample, static_cast<int>(i6->state), i6->thread ? 1 : 0, chain);
        }
      }
      // Direct per-tid state census (tids 6..31) EVERY sample. The plural
      // QueryThreadDebugInfos() sweep filters out kExited/kZombie threads, so a
      // thread that has TORN DOWN silently drops from the roster and reads as a
      // "head-of-line spin" when it is actually a mass thread teardown. Query by
      // id (entries persist post-exit) to get the true state of every game
      // thread at the freeze: 0=Alive 1=Waiting 2=Exited 3=Zombie -=absent.
      {
        std::string census;
        for (uint32_t tid = 6; tid <= 31; ++tid) {
          auto* ti = processor->QueryThreadDebugInfo(tid);
          if (!ti) continue;
          census += fmt::format(" {}:{}", tid, static_cast<int>(ti->state));
        }
        XELOGE("RB3DX UI PROBE[{}]: TIDCENSUS{}", sample, census);
      }
      // Walk every live guest thread every 5th tick; the stuck party in a
      // lockstep handshake is usually NOT the thread you first suspected, so
      // sample them all. Enumerate via the debugger's ThreadDebugInfo registry:
      // both threads_by_id_ (fork exited-thread sweep) and the object table
      // (title closes thread handles after spawn) lose guest threads within
      // ~20s of boot, which blinded the two earlier samplers in turn.
      bool full_sweep = (sample % 5) == 1;
      for (auto* info : processor->QueryThreadDebugInfos()) {
        // Accept kAlive AND kWaiting: a busy-spin that momentarily dips into a
        // guest wait gets stuck marked kWaiting (OnThreadLeavingWait is not
        // always paired), which silently dropped the main thread from the dump
        // ~20s in. Only a null thread ptr (kZombie/destroyed) is truly unreadable.
        if (!info || !info->thread ||
            (info->state != cpu::ThreadDebugInfo::State::kAlive &&
             info->state != cpu::ThreadDebugInfo::State::kWaiting)) {
          continue;
        }
        uint32_t tid = info->thread_id;
        if (!full_sweep && tid != 6) continue;
        int dbg_state = static_cast<int>(info->state);
        auto* tstate = info->thread->thread_state();
        auto* ctx = tstate ? tstate->context() : nullptr;
        if (!ctx) continue;
        uint32_t mlr = static_cast<uint32_t>(ctx->lr);
        uint32_t msp = static_cast<uint32_t>(ctx->r[1]);
        std::string chain;
        uint32_t s = msp;
        for (int i = 0; i < 16 && s; ++i) {
          uint32_t bc = r32(s);
          if (bc <= s || bc - s > 0x100000) break;
          uint32_t slr = r32(bc - 8);
          chain += fmt::format(" {:08X}", slr);
          s = bc;
        }
        XELOGE(
            "RB3DX UI PROBE[{}]:   thr{} st={} lr={:08X} sp={:08X} r3={:08X} "
            "r4={:08X} r5={:08X} chain:{}",
            sample, tid, dbg_state, mlr, msp, static_cast<uint32_t>(ctx->r[3]),
            static_cast<uint32_t>(ctx->r[4]),
            static_cast<uint32_t>(ctx->r[5]), chain);
      }
      // --rb3_splash_unwedge: break the Splash::EndSplasher boot deadlock.
      // App::App's EndSplasher (0x82742620) calls SetImmutableState(kTerminating)
      // which no-ops when a Suspend is in-flight (mState<kResumed), then blocks
      // in WaitForState(kTerminated) at ret 0x82742668 forever while the
      // SplashThread worker is parked in WaitForState(kResuming). Locate the live
      // Splash via tid=6's stack: the WaitForState frame (saved_lr==0x82742668)
      // saved r31 = Splash at (back-chain - 0x10). 360 layout: unk_0x88 event
      // handle @ +0x8C (worker->main, tid6 waits), unk_0xAC @ +0x90 (main->worker,
      // tid13 waits), mState @ +0x94. Drive the state machine to termination.
      if (cvars::rb3_splash_unwedge) {
        uint32_t splash = 0;
        for (auto* info : processor->QueryThreadDebugInfos()) {
          if (!info || info->state != cpu::ThreadDebugInfo::State::kAlive ||
              !info->thread || info->thread_id != 6) {
            continue;
          }
          auto* tstate = info->thread->thread_state();
          auto* ctx = tstate ? tstate->context() : nullptr;
          if (!ctx) continue;
          uint32_t s = static_cast<uint32_t>(ctx->r[1]);
          for (int i = 0; i < 24 && s; ++i) {
            uint32_t bc = r32(s);
            if (bc <= s || bc - s > 0x100000) break;
            if (r32(bc - 8) == 0x82742668u) {
              splash = r32(bc - 0x10);
              break;
            }
            s = bc;
          }
        }
        if (splash >= 0x40000000u && splash < 0x50000000u) {
          uint32_t mstate = r32(splash + 0x94);
          uint32_t ev_main = r32(splash + 0x8C);
          uint32_t ev_wrk = r32(splash + 0x90);
          XELOGE(
              "RB3DX UI PROBE[{}]: SPLASH-UNWEDGE splash={:08X} mState={} "
              "ev_main={:08X} ev_wrk={:08X}",
              sample, splash, mstate, ev_main, ev_wrk);
          auto poke = [&](uint32_t a, uint32_t v) {
            if (auto* heap = memory->LookupHeap(a)) {
              heap->Protect(a & ~0xFFFu, 0x2000,
                            kMemoryProtectRead | kMemoryProtectWrite);
            }
            xe::store_and_swap<uint32_t>(base + a, v);
          };
          auto sig = [&](uint32_t handle) {
            if (!handle) return;
            auto ev = kernel_state->object_table()->LookupObject<kernel::XEvent>(
                handle);
            if (ev) ev->Set(0, false);
          };
          // State-driven drive toward kTerminated(8): satisfy the worker's
          // WaitForState(kResuming=4) first, let it advance, then terminate and
          // wake main. mState<8 means main is still stuck in EndSplasher.
          if (mstate != 8) {
            if (mstate < 4) {
              poke(splash + 0x94, 4);  // kResuming -> release the worker
              sig(ev_wrk);
            } else if (mstate == 4 || mstate == 5) {
              poke(splash + 0x94, 7);  // kTerminating
              sig(ev_wrk);
            } else {  // 6/7
              poke(splash + 0x94, 8);  // kTerminated
              sig(ev_wrk);
              sig(ev_main);  // wake main out of WaitForState(kTerminated)
            }
          } else {
            sig(ev_main);  // ensure main woke to observe kTerminated
          }
        }
      }
    }
    // --- TheLoadMgr candidate dump (clean-TU5 stall): the loader-code region
    // 0x82740000-0x82760000 materializes 0x82E00050/58/60/68/80 far more than
    // any other data address, so the LoadMgr singleton almost certainly lives
    // at ~0x82E00040. Dump the window each tick; if this is right, the queued
    // panel DirLoader pointers show up in the mLoading list nodes and the
    // mPeriod float (10.0f/26.67f/0) is directly readable.
    {
      // TheLoadMgr located via the si46 list-node prev-walk: the mLoading
      // embedded dummy is 0x82E06E38, mPeriod (10.0f) sits at 0x82E06E48.
      // Dump the object window every tick -- mTimer restarting between ticks
      // distinguishes "Poll runs but starves" from "Poll never called".
      std::string row;
      uint32_t win_lo = 0x82E06DE0, win_hi = 0x82E06EA0;
      for (uint32_t a = win_lo; a < win_hi; a += 4) {
        if (((a - win_lo) & 0x1F) == 0 && !row.empty()) {
          XELOGE("RB3DX UI PROBE[{}]:   loadmgr[{:08X}]:{}", sample, a - 0x20,
                 row);
          row.clear();
        }
        row += fmt::format(" {:08X}", r32(a));
      }
      if (!row.empty()) {
        XELOGE("RB3DX UI PROBE[{}]:   loadmgr[{:08X}]:{}", sample,
               win_hi - 0x20, row);
      }
      // LoadMgr layout probe: LoadMgr::Poll (0x827bf700) gets this=0x82E06E38
      // and drains this->0x18 (list sentinel @0x82E06E50). Dump BOTH candidate
      // list heads to resolve which list the pump actually walks vs which one
      // the stuck inline_help_center loader sits in. For each: raw {next,prev}
      // and the front loader's file.
      for (uint32_t lh : {0x82E06E38u, 0x82E06E50u}) {
        uint32_t nx = r32(lh), pv = r32(lh + 4);
        std::string ff;
        if (nx && nx != lh) {
          uint32_t l = r32(nx + 8);
          ff = fmt::format(" front_ldr={:08X} mState20={:08X}", l, r32(l + 0x20));
          std::string s = rstr(r32(l + 0x14));
          if (s.size() >= 3 && s.size() < 47) ff += " '" + s + "'";
        }
        XELOGE("RB3DX UI PROBE[{}]: LISTHEAD {:08X} next={:08X} prev={:08X}{}",
               sample, lh, nx, pv, ff);
      }
      // Walk mLoading (embedded dummy @0x82E06E38, nodes {next,prev,Loader*})
      // and dump each queued loader's header + any string its early fields
      // point at -- names the file the front (stuck) loader is on.
      uint32_t dummy = 0x82E06E38;
      uint32_t node = r32(dummy);
      for (int n = 0; n < 4 && node && node != dummy; ++n) {
        uint32_t ldr = r32(node + 8);
        std::string hdr, strs;
        for (uint32_t d = 0; d < 0x80; d += 4) {  // extend to 0x80 to reach
                                                  // FileLoader mState (PTMF @0x44)
          uint32_t v = r32(ldr + d);
          hdr += fmt::format(" {:08X}", v);
          if (v >= 0x1000 && (v < 0x50000000 || (v >= 0x82000000 &&
                                                 v < 0x83000000))) {
            std::string s = rstr(v);
            if (s.size() >= 3 && s.size() < 47 && s != "<binary>") {
              strs += fmt::format(" +0x{:X}->'{}'", d, s);
            }
          }
        }
        // The loader's vtable (word[0]) names its class; dump the first 6 vptr
        // slots so IsLoaded/PollLoading/StateName can be mapped via scope_map.
        uint32_t vt = r32(ldr);
        std::string vts;
        for (uint32_t s = 0; s < 0x18; s += 4) vts += fmt::format(" {:08X}", r32(vt + s));
        XELOGE("RB3DX UI PROBE[{}]:   loadq[{}] node={:08X} ldr={:08X}:{}{} vt[{:08X}]:{}",
               sample, n, node, ldr, hdr, strs, vt, vts);
        node = r32(node);
      }
      // DirLoader mState locator: dump every .text-range word in the first two
      // queued loaders' objects [0,0x140). The FRONT loader (loadq[0]) is being
      // actively polled so its mState PTMF has advanced past &DirLoader::OpenFile,
      // while the loader behind it (loadq[1]) has never been polled and still
      // holds &OpenFile. The offset whose code-address VALUE differs between the
      // two is mState; front's value names the stuck state (OpenFile/LoadHeader/
      // LoadDir/LoadResources/CreateObjects/LoadObjs/DoneLoading, declared in
      // that order so their addresses tend to be monotonic).
      {
        uint32_t n2 = r32(dummy);
        uint32_t objs[2] = {0, 0};
        for (int q = 0; q < 2 && n2 && n2 != dummy; ++q) {
          uint32_t ldr = r32(n2 + 8);
          objs[q] = ldr;
          std::string all;
          for (uint32_t d = 0; d < 0xB0; d += 4) {
            all += fmt::format(" {:08X}", r32(ldr + d));
          }
          XELOGE("RB3DX UI PROBE[{}]:   ldr-all[{}] {:08X}:{}", sample, q, ldr,
                 all);
          n2 = r32(n2);
        }
        // Explicit per-word diff of the two loader objects: names exactly which
        // offset(s) the actively-polled front loader has advanced vs the queued
        // (OpenFile-state) one behind it.
        if (objs[0] && objs[1]) {
          std::string diff;
          for (uint32_t d = 0; d < 0xB0; d += 4) {
            uint32_t a = r32(objs[0] + d), b = r32(objs[1] + d);
            if (a != b) diff += fmt::format(" +{:X}:{:08X}vs{:08X}", d, a, b);
          }
          XELOGE("RB3DX UI PROBE[{}]:   ldr-diff front-vs-queued:{}", sample,
                 diff.empty() ? " (identical)" : diff.c_str());
        }
      }
      // Front-loader change detector: distinguishes "PollLoading body never
      // runs" (frozen bytes = budget starvation before the first state step,
      // the HX_NATIVE-documented CheckSplit gate) from "OpenFile runs but
      // retries forever" (fields churn).
      uint32_t front_node = r32(dummy);
      if (front_node && front_node != dummy) {
        uint32_t fldr = r32(front_node + 8);
        uint64_t h = 1469598103934665603ull;
        for (uint32_t d = 0; d < 0x80; d += 4) {
          h = (h ^ r32(fldr + d)) * 1099511628211ull;
        }
        static uint32_t s_front_ldr = 0;
        static uint64_t s_front_hash = 0;
        static int s_front_frozen = 0;
        if (fldr == s_front_ldr && h == s_front_hash) {
          s_front_frozen++;
        } else {
          s_front_frozen = 0;
        }
        s_front_ldr = fldr;
        s_front_hash = h;
        XELOGE("RB3DX UI PROBE[{}]:   front-ldr {:08X} hash={:016X} frozen x{}",
               sample, fldr, h, s_front_frozen);
      }
      // --rb3_loadmgr_unbudget: poke the LoadMgr period/split floats
      // (10.0f pair at 0x82E06E48/4C) to 1e30 -- the game's own
      // PollUntilEmpty() value -- so every per-frame Poll() drains
      // unbudgeted. If the boot then proceeds, the CheckSplit starvation
      // diagnosis is confirmed AND the flow is unblocked in one stroke.
      if (cvars::rb3_loadmgr_unbudget) {
        // 0x82E06Exx is title .data (RW pages; host mapping writable -- no
        // heap->Protect needed, unlike code pages).
        for (uint32_t a : {0x82E06E48u, 0x82E06E4Cu}) {
          if (readable(a) && r32(a) != 0x7149F2CAu) {
            xe::store_and_swap<uint32_t>(base + a, 0x7149F2CAu);  // 1e30f
            XELOGE("RB3DX UI PROBE[{}]: loadmgr unbudget poke @{:08X}", sample,
                   a);
          }
        }
      }
    }
    // --- SI claim-table dump (--rb3dx_si_claim_anchor): the DLL's own
    // gClaims/gImpls, the ground truth for "two players on one track".
    if (cvars::rb3dx_si_claim_anchor != 0) {
      uint32_t anchor = static_cast<uint32_t>(cvars::rb3dx_si_claim_anchor);
      uint32_t claim_count = r32(anchor + 0x1C8);
      uint32_t impl_count = r32(anchor + 0x1CC);
      std::string claims;
      for (uint32_t i = 0; i < 3; ++i) {
        claims += fmt::format(" claim{}={{track={},cnt={}}}", i,
                              static_cast<int32_t>(r32(anchor + i * 8)),
                              r32(anchor + i * 8 + 4));
      }
      XELOGE("RB3DX UI PROBE[{}]:   SI claims: claimCount={} implCount={}{}",
             sample, claim_count, impl_count, claims);
      // --- band/pad snapshot: TheBandUserMgr slot-map guids, the
      // participants vector (BandUser subobjects), and the Joypad
      // pad->LocalUser association table. Layouts re-derived from
      // GetBandUserFromSlot @0x82682B60 (slot guids at mgr+0x50 stride
      // 0x10, empty == all-zero; participants vector begin/end at
      // mgr+0x28/+0x2C of BandUser*, guid on the vbase-adjusted subobject
      // +0x30) and RB3E ports_xbox360.h (PORT_THEBANDUSERMGR,
      // PORT_JOYPAD_USERPTR_BASE/PADFLAG_BASE; button messages self-filter
      // on pad-stamped LocalUser identity, so two same-instrument players
      // need two distinct connected pads with distinct LocalUsers).
      const uint32_t kTheBandUserMgr = 0x82E023B8;
      const uint32_t kJoypadUserBase = 0x82CCB30C;  // + p*0xD4
      const uint32_t kJoypadFlagBase = 0x82CCB2A0;  // + p; 0 == connected
      uint32_t mgr = r32(kTheBandUserMgr);
      if (mgr) {
        std::string slots;
        for (uint32_t s = 0; s < 4; ++s) {
          uint32_t g0 = r32(mgr + 0x50 + s * 0x10);
          uint32_t g1 = r32(mgr + 0x50 + s * 0x10 + 4);
          uint32_t g2 = r32(mgr + 0x50 + s * 0x10 + 8);
          uint32_t g3 = r32(mgr + 0x50 + s * 0x10 + 12);
          slots += fmt::format(" slot{}={:08X}{:08X}{:08X}{:08X}", s, g0, g1,
                               g2, g3);
        }
        uint32_t vb = r32(mgr + 0x28);
        uint32_t ve = r32(mgr + 0x2C);
        uint32_t n = (ve > vb && ve - vb < 0x100) ? (ve - vb) / 4 : 0;
        std::string parts;
        for (uint32_t i = 0; i < n && i < 4; ++i) {
          uint32_t u = r32(vb + i * 4);
          if (!u) continue;
          uint32_t inner = r32(u + 4);
          uint32_t adj = inner ? r32(inner + 4) : 0;
          uint32_t lb = (adj < 0x1000) ? u + adj : u;
          parts += fmt::format(
              " user{}={{bu=0x{:08X} track={} diff={} shell={} adj=0x{:X} "
              "guid={:08X}{:08X}{:08X}{:08X}}}",
              i, u, static_cast<int32_t>(r32(u + 0x10)),
              static_cast<int32_t>(r32(u + 0x8)), r32(u + 0x20), adj,
              r32(lb + 0x30), r32(lb + 0x34), r32(lb + 0x38), r32(lb + 0x3C));
        }
        std::string pads;
        for (uint32_t p = 0; p < 4; ++p) {
          uint32_t flag = r8(kJoypadFlagBase + p);
          uint32_t lu = r32(kJoypadUserBase + p * 0xD4);
          pads += fmt::format(" pad{}={{conn={} lu=0x{:08X}}}", p,
                              flag == 0 ? 1 : 0, lu);
        }
        XELOGE("RB3DX UI PROBE[{}]:   band: mgr=0x{:08X} n={}{} |{} |{}",
               sample, mgr, n, slots, parts, pads);
      }
    }
    // --- closed-loop part/difficulty autopilot (--rb3dx_autoconfirm_parts).
    // Screen-conditional so it is immune to the tens-of-seconds menu-load
    // variance that made fixed-time A@1 presses land at the hub (opening
    // P2's overshell menu) or after the cards were gone (si6..si10).
    if (cvars::rb3dx_autoconfirm_parts && ts == 0) {
      // Pad-1-first ordering is load-bearing: if P1 confirms while P2's card
      // is untouched, the game leaves part_difficulty_screen immediately and
      // AutoAssignMissingSlots handles P2 -- with the SI hooks armed that
      // auto-assign path wedges in the classic track_-1 vector[-1] fault
      // livelock at guest EA 0xFFFFFFFC (si12; the exact crash family H1/
      // #32b were built against, but on hardware players always picked
      // explicitly so auto-assign+SI was never exercised). Give P2 the first
      // three samples (part + difficulty confirms), then bring P1 along.
      static int s_part_screen_samples = 0;
      static int s_p2_ups_done = 0;
      // Occupied slots in BandUserMgr's guid-keyed slot map: the real "who
      // has joined the band" signal (pad-table LocalUser binding is just
      // sign-in and is nonzero for every --local_user_count user).
      auto slots_occupied = [&]() {
        uint32_t mgr = r32(0x82E023B8 /*kTheBandUserMgr*/);
        if (!mgr) return 0;
        int n = 0;
        for (uint32_t s = 0; s < 4; ++s) {
          uint32_t base = mgr + 0x50 + s * 0x10;
          if (r32(base) | r32(base + 4) | r32(base + 8) | r32(base + 12)) ++n;
        }
        return n;
      };
      if (cur_name == "part_difficulty_screen") {
        ++s_part_screen_samples;
        // si33: an A on pad 1 at the part screen JOINS P2 directly (no START
        // dance needed), so the join happens here -- late enough that the
        // instrument pick stays in this screen instead of resolving inside
        // the hub join overshell (si30). Sequence: A@1 until the slot map
        // shows a second member, then N UPs (--rb3dx_autoconfirm_p2_up) to
        // walk P2's CHOOSE INSTRUMENT list off its slot default (BASS even
        // when guitar is free, si22) onto the SI-un-greyed duplicate, then
        // the confirm cadence.
        if (slots_occupied() < 2) {
          xe::hid::nop::NopInjectButtonPress(1, 0x1000 /*A*/, 250);
          XELOGW(
              "RB3DX UI PROBE[{}]: autopilot A@1 join (part_difficulty_screen "
              "sample {}, slots={})",
              sample, s_part_screen_samples, slots_occupied());
        } else if (s_p2_ups_done < cvars::rb3dx_autoconfirm_p2_up) {
          ++s_p2_ups_done;
          xe::hid::nop::NopInjectButtonPress(1, 0x0001 /*DPAD_UP*/, 250);
          XELOGW(
              "RB3DX UI PROBE[{}]: autopilot UP@1 (part_difficulty_screen "
              "up {}/{})",
              sample, s_p2_ups_done, cvars::rb3dx_autoconfirm_p2_up);
        } else {
          uint32_t pad = (s_part_screen_samples <= 6 || (sample & 1)) ? 1u : 0u;
          xe::hid::nop::NopInjectButtonPress(pad, 0x1000 /*X_INPUT_GAMEPAD_A*/,
                                             250);
          XELOGW(
              "RB3DX UI PROBE[{}]: autopilot A@{} (part_difficulty_screen "
              "sample {})",
              sample, pad, s_part_screen_samples);
        }
      } else {
        s_part_screen_samples = 0;
        s_p2_ups_done = 0;
        static int s_hub_samples = 0;
        if (cur_name != "main_hub_screen") s_hub_samples = 0;
        if (cur_name == "song_select_screen" && (sample & 1)) {
          // P2 joins HERE, state-driven: a join initiated from the hub (si30)
          // resolves the instrument inside the join overshell (u1 went
          // straight to ChooseDiff on BASS before the part screen could be
          // navigated), while a join from song_select defers the part pick to
          // part_difficulty_screen -- the si18/si26 shape the p2_up
          // navigation was built for. Alternate START/A on pad 1 until its
          // LocalUser binds, and only then confirm the song: confirming with
          // P2 unjoined hands the empty slot to AutoAssignMissingSlots, the
          // auto-assign+SI track_-1 livelock (si12).
          const uint32_t kJoypadUserBaseNav = 0x82CCB30C;  // + p*0xD4
          uint32_t p2_lu = r32(kJoypadUserBaseNav + 1 * 0xD4);
          if (p2_lu != 0) {
            xe::hid::nop::NopInjectButtonPress(0, 0x1000, 250);
            XELOGW("RB3DX UI PROBE[{}]: autopilot A@0 (song_select_screen)",
                   sample);
          } else {
            uint32_t mask = (sample & 2) ? 0x0010u /*START*/ : 0x1000u /*A*/;
            xe::hid::nop::NopInjectButtonPress(1, mask, 250);
            XELOGW(
                "RB3DX UI PROBE[{}]: autopilot {}@1 (song_select_screen, "
                "joining P2)",
                sample, (mask == 0x0010u) ? "START" : "A");
          }
        } else if (cur_name == "intro_movie_screen" ||
                   cur_name == "splash_screen" ||
                   cur_name == "dx_welcome_screen" ||
                   cur_name == "dx_settings_error_screen") {
          if (sample & 1) {
            xe::hid::nop::NopInjectButtonPress(0, 0x1000, 250);
            XELOGW("RB3DX UI PROBE[{}]: autopilot A@0 ({})", sample, cur_name);
          }
        } else if (cur_name == "hint_rb3_welcome_screen") {
          // Fresh-save first-boot chain: message page(s), then a
          // CUSTOMIZE BAND / CONTINUE choice whose default is CUSTOMIZE.
          // Alternate DOWN / A: DOWN is a no-op on message pages and moves
          // focus to CONTINUE on the choice page, so either phase of the
          // alternation dismisses the chain without entering Customize Band
          // (si28/si29: blind A landed on CUSTOMIZE and parked the run in
          // manage_band_screen for 250s).
          uint32_t mask = (sample & 1) ? 0x1000u /*A*/ : 0x0002u /*DPAD_DOWN*/;
          xe::hid::nop::NopInjectButtonPress(0, mask, 250);
          XELOGW("RB3DX UI PROBE[{}]: autopilot {}@0 (hint_rb3_welcome_screen)",
                 sample, (mask == 0x1000u) ? "A" : "DOWN");
        } else if (cur_name == "manage_band_screen") {
          // Recovery: a stray confirm entered Customize Band; B backs out.
          if (sample & 1) {
            xe::hid::nop::NopInjectButtonPress(0, 0x2000 /*B*/, 250);
            XELOGW("RB3DX UI PROBE[{}]: autopilot B@0 (manage_band_screen)",
                   sample);
          }
        } else if (cur_name == "main_hub_screen") {
          // Deterministic PLAY NOW: the hub list has PLAY NOW at the top, so
          // two UPs pin focus there from any tile, then A enters it.
          ++s_hub_samples;
          if (s_hub_samples <= 2) {
            xe::hid::nop::NopInjectButtonPress(0, 0x0001 /*DPAD_UP*/, 250);
            XELOGW("RB3DX UI PROBE[{}]: autopilot UP@0 (main_hub_screen {})",
                   sample, s_hub_samples);
          } else if (sample & 1) {
            xe::hid::nop::NopInjectButtonPress(0, 0x1000, 250);
            XELOGW("RB3DX UI PROBE[{}]: autopilot A@0 (main_hub_screen)",
                   sample);
          }
        }
      }
    }
    // --- one-shot guest-code dump: the xex-file-offset disasm used off-line
    // is shifted (section raw/virtual gaps), so dump live code for offline
    // capstone at the chain addresses of interest.
    static bool s_codedump_done = false;
    static uint32_t s_gmainthread_addr = 0;
    if (!s_codedump_done && sample == 8) {
      s_codedump_done = true;
      // 0x82741200..0x82741B00: the WaitForState/SetMutableState/Resume fn
      // cluster (WaitForState proper at 0x827414E0; callers 0x827413A4,
      // 0x82741948, 0x82741A58 seen in wait-census frames).
      // 0x824A4C10: MainThread() -- the bl target inside WaitForState's
      // event-pick branch. 0x827CAFxx: the parked worker's loop fn (census
      // frame[1] ret 0x827CAFD0). 0x82736800/0x8246B880: main-thread caller
      // frames above the stuck WaitForState.
      // Second wave (si57 analysis): the 130s post-App-ctor wedge lives in
      // the chain 0x8250F854 -> 0x823E0804 -> 0x823EDCE8 -> 0x82A89AEC ->
      // leaf 0x82A8BA6C (looks like DTA dispatch into a spinning native
      // handler); dump those regions plus 0x82742620 (the App-boot
      // WaitForState caller that DID complete) and the boot fn 0x82272E00.
      for (uint32_t fn :
           {0x8279A650u, 0x82742600u, 0x82527A80u, 0x82844C80u, 0x82271500u,
            0x824A4C10u, 0x827CAF00u, 0x827CB000u, 0x827CB100u, 0x82736800u,
            0x8246B880u, 0x82741200u, 0x82741300u, 0x82741400u, 0x82741500u,
            0x82741600u, 0x82741700u, 0x82741800u, 0x82741900u, 0x82741A00u,
            0x82A89A00u, 0x82A8B900u, 0x82A8BA00u, 0x823E0700u, 0x823EDC00u,
            0x8250F800u, 0x82272E00u, 0x822703D0u, 0x82270400u,
            // tid=9 loader/consumer wait site (lr 0x8286C5BC), the worker-pool
            // producer that sets events (lr 0x8283D3D4), and the worker idle
            // loop (0x82844CF8) -- to identify the thread-pool dispatch path.
            0x8286C580u, 0x8283D380u, 0x82844CC0u,
            // The tid=6 WaitForState handshake stall: SetMutableState/WaitForState
            // cluster (0x82741400/500/E80), the scoped caller (0x82742620), the
            // boot/frame fn that invokes it (0x82271500), and main's caller frames
            // (0x82272E40..0x82272F80) to place this in App::App vs the frame loop.
            0x82742620u, 0x827414C0u, 0x82741E80u, 0x82271580u, 0x82271600u,
            0x82272E40u, 0x82272EC0u, 0x82272F40u,
            // Sample-9 busy-spin chain (post-EndSplasher App::App init): main
            // loops here forever without pumping TheLoadMgr (mTimer frozen).
            // Return sites: 82272E9C(App::App) -> 8250F854 -> 8240EE38 ->
            // 8273B2CC -> 82739BB8 -> 827334CC -> 8273CD10 -> 82858408 ->
            // 82857F40, lr=82273334. Dump 0x180 around each to decode the loop.
            0x8250F780u, 0x8240ED80u, 0x8273B200u, 0x82739B00u, 0x82733400u,
            0x8273CC80u, 0x82858380u, 0x82857E80u, 0x82273280u, 0x82272E00u,
            // main_impl(0x82272E60) calls three fns in order: 0x82270E68
            // (the big boot fn -- main BLOCKS ~15s here inside EndSplasher's
            // WaitForState at 0x8227158C), then 0x822703D0 (tiny: register App
            // singleton), then 0x82270000 (subscriber broadcast). NONE loops;
            // 0x82270E68's tail (0x822715AC) is an UNCONDITIONAL return after
            // EndSplasher. Dump 0x82270E68..0x822715B0 CONTIGUOUSLY to find any
            // loop-vs-return gate EARLIER in the fn (the 0x82271068..0x82271500
            // stretch was previously an un-dumped gap).
            0x82270000u, 0x82270100u, 0x82270200u, 0x82270300u, 0x822703D0u,
            0x82270E68u, 0x82271068u, 0x82271200u, 0x82271400u,
            0x8250F820u, 0x8250F890u, 0x8240EE10u, 0x82270E00u,
            // Splash-WORKER (thr14) call chain: entry 8284D6DC -> 0x82742978
            // -> 0x82742884 -> 0x8274279C -> 0x82741948 -> WaitForState. This
            // is where the terminate-vs-transition-to-frontend decision lives
            // (main/thr6 parks in EndSplasher 0x82742620 until thr14 sets
            // kTerminated). Dump the whole 0x82742700..0x82742B00 worker region
            // + PollTheSplasher 0x827429E0.
            0x82742700u, 0x82742780u, 0x82742880u, 0x82742900u, 0x82742978u,
            0x82742A00u, 0x82742A80u, 0x82741940u,
            // CRT after main_impl returns (thr6 chain bottom 0x8283CEB0 = the
            // caller of main). Decode whether the CRT loops/keeps a frame loop
            // or just exits -- the structural "where is App::Run" question.
            0x8283CE00u, 0x8283CE80u, 0x8283CF00u,
            // The broadcast's post-loop callees: 0x82718880 (Pollable.cpp),
            // 0x8250F4E0 (Debug.cpp), and 0x8283D4F0 (residual, fired only when
            // r5=1, with string 0x82000C55) -- the teardown-trigger suspects.
            0x82718840u, 0x8250F4C0u, 0x8283D4C0u, 0x8283D540u,
            // os/System.cpp frame functions: the real per-frame System::Poll
            // (pumps TheLoadMgr.Poll + pollables + render) that main should be
            // looping on. Decode 0x825100C8 (size 292) + 0x82510510 (size 120).
            0x82510080u, 0x82510180u, 0x82510280u, 0x82510480u, 0x82510500u,
            // DirLoader state dispatcher: PollLoading 0x82754A58, IsLoaded
            // 0x82754DB0, StateName 0x82757998. Decode how mState is loaded/
            // called to confirm its offset + what a stuck value means. Plus the
            // 0x82106900 region (holds the stuck loader's mState=0x8210693C).
            0x82754A40u, 0x82754D80u, 0x82757980u, 0x82106900u, 0x82106980u,
            // The loader-pump System::Poll invokes (bl 0x827bf700, r3=0x82E06E38
            // = TheLoadMgr.mLoading): is it LoadMgr::Poll -> PollFrontLoader (which
            // should POP a front loader whose IsLoaded()==true)? The stuck front
            // reports IsLoaded=true yet is never popped. Dump 0x827BF700 + the
            // Loader.cpp poll cluster around it.
            0x827BF700u, 0x827BF7D0u, 0x827BFA00u, 0x827BFAC0u}) {
        std::string row;
        for (uint32_t d = 0; d < 0x200; d += 4) {
          row += fmt::format(" {:08X}", r32(fn + d));
        }
        XELOGE("RB3DX UI PROBE[{}]: CODE {:08X}:{}", sample, fn, row);
      }
      // Pollable-list walk: main's 3rd call (0x82270000) broadcasts to the list
      // at source 0x82CC9874 (r3 = lis -0x7d33 + addi -0x678c). Each node has
      // next=[node+0], handler=[node+8]. Walk it and log each node + handler so
      // we can name the Pollables (via rb3-xenon scope_map) and find which
      // fires the teardown. Head = [0x82CC9874 + 0x28]; sentinel = 0x82CC989C.
      {
        const uint32_t src = 0x82CC9874u;
        const uint32_t sentinel = src + 0x28u;
        uint32_t node = r32(sentinel);
        std::string pl;
        for (int i = 0; i < 32 && node && node != sentinel; ++i) {
          uint32_t handler = r32(node + 8);
          pl += fmt::format(" [{:08X}]h={:08X}", node, handler);
          node = r32(node);
        }
        XELOGE("RB3DX UI PROBE[{}]: POLLABLES src={:08X}:{}", sample, src, pl);
      }
      // Poll-singleton dump: 0x8240EE10 loads *(0x82C76B68) then calls its
      // vtable slot 0x60. Dump the pointer, its vtable head, and the object so
      // we can name the manager main polls each loop iteration.
      {
        uint32_t obj = r32(0x82C76B68u);
        uint32_t vt = r32(obj);
        std::string orow, vrow;
        for (uint32_t d = 0; d < 0x40; d += 4) orow += fmt::format(" {:08X}", r32(obj + d));
        for (uint32_t d = 0; d < 0x80; d += 4) vrow += fmt::format(" {:08X}", r32(vt + d));
        XELOGE("RB3DX UI PROBE[{}]: POLLSINGLETON obj={:08X} vt={:08X} obj:[{}] vt:[{}]",
               sample, obj, vt, orow, vrow);
      }
      // Decode &gMainThreadID out of MainThread() at 0x824A4C10: find the
      // first lis rD,H / lwz rT,L(rD) pair (MSVC absolute-address global
      // load). MainThread() returns true for EVERY thread when the global is
      // 0 -- which would send every worker onto the main-side event and
      // explain a multi-object handshake wedge -- so watch its value live.
      {
        uint32_t hi_val[32] = {0};
        bool hi_set[32] = {false};
        for (uint32_t d = 0; d < 0x60 && !s_gmainthread_addr; d += 4) {
          uint32_t insn = r32(0x824A4C10u + d);
          if ((insn & 0xFC1F0000u) == 0x3C000000u) {  // lis rD, IMM
            uint32_t rd = (insn >> 21) & 31;
            hi_val[rd] = insn << 16;
            hi_set[rd] = true;
          } else if ((insn & 0xFC000000u) == 0x80000000u) {  // lwz rT, d(rB)
            uint32_t rb = (insn >> 16) & 31;
            if (rb != 0 && hi_set[rb]) {
              int16_t lo = static_cast<int16_t>(insn & 0xFFFF);
              s_gmainthread_addr = hi_val[rb] + lo;
            }
          }
        }
        XELOGE("RB3DX UI PROBE[{}]: gMainThreadID decode -> addr={:08X}",
               sample, s_gmainthread_addr);
      }
    }
    if (s_gmainthread_addr) {
      XELOGE("RB3DX UI PROBE[{}]: gMainThreadID @{:08X} = {:08X}", sample,
             s_gmainthread_addr, r32(s_gmainthread_addr));
    }
    // --rb3_overlapped_scan: test the async-completion hypothesis for the
    // clean-TU5 loader freeze. AsyncFileWin::_ReadDone spins while
    // OVERLAPPED.Internal == STATUS_PENDING (0x103); if xenia's synchronous
    // read completion never clears it, exactly one OVERLAPPED stays 0x103
    // across the whole stuck phase. Scan the guest heap for words == 0x103
    // whose following word (InternalHigh) is a plausible byte count, and
    // report any address that reads 0x103 on two consecutive samples.
    if (cvars::rb3_overlapped_scan && (sample % 4) == 0) {
      static std::map<uint32_t, int> s_pending_seen;
      std::map<uint32_t, int> now_pending;
      auto scan = [&](uint32_t lo, uint32_t hi) {
        for (uint32_t page = lo; page < hi; page += 0x1000) {
          if (!readable(page)) continue;
          for (uint32_t a = page; a < page + 0x1000 - 8; a += 4) {
            if (r32(a) != 0x00000103u) continue;
            uint32_t high = r32(a + 4);
            if (high <= 0x400000u) {  // plausible InternalHigh (bytes)
              now_pending[a] = s_pending_seen.count(a) ? s_pending_seen[a] + 1
                                                       : 1;
            }
          }
        }
      };
      scan(0x40000000u, 0x44000000u);
      int persistent = 0;
      for (auto& [a, cnt] : now_pending) {
        if (cnt >= 2) {
          persistent++;
          if (persistent <= 6) {
            XELOGE(
                "RB3DX UI PROBE[{}]: OVERLAPPED-PENDING @{:08X} Internal=103 "
                "InternalHigh={:08X} persisted x{}",
                sample, a, r32(a + 4), cnt);
          }
        }
      }
      XELOGE("RB3DX UI PROBE[{}]: overlapped-scan: {} words==0x103, {} persistent",
             sample, (int)now_pending.size(), persistent);
      s_pending_seen = std::move(now_pending);
    }
    // --rb3_tu5_hash_poke: the TU5 exe embeds 922 per-file SHA1s of ark
    // entries (arkhelper hashfinder verified); a patchcreator ark whose
    // ui.dtb/splash.dtb no longer match makes the game silently set its quit
    // flag during App::App and exit App::Run before the first frame (the
    // "clean-TU5 boot freeze" = that quit + a teardown spin). arkhelper's own
    // fix is patching the hashes into the exe; do the equivalent in guest
    // memory: find the two original 20-byte hashes in the image once, then
    // overwrite (and re-assert each tick) with the patched files' hashes.
    if (cvars::rb3_tu5_hash_poke) {
      struct HashFix {
        uint8_t orig[20];
        uint8_t fixed[20];
        uint32_t addr;  // 0 until found
      };
      // ui/gen/ui.dtb, ui/splash/gen/splash.dtb (patched 2026-08-26).
      static HashFix s_fixes[2] = {
          {{0xE0, 0x16, 0x62, 0x1C, 0xAF, 0xDA, 0xFD, 0xAB, 0xDA, 0xDA,
            0x1A, 0x3A, 0x91, 0xB1, 0x83, 0xA4, 0x9E, 0x00, 0xC8, 0x8A},
           {0x57, 0x35, 0xA7, 0xC1, 0x74, 0x04, 0xE4, 0xCB, 0xE1, 0x15,
            0xCA, 0x21, 0x22, 0xA9, 0x46, 0x71, 0xD4, 0x5D, 0x90, 0x30},
           0},
          {{0x5B, 0x7B, 0x45, 0xD9, 0x12, 0x5A, 0xF5, 0x4D, 0xC0, 0xE7,
            0x5E, 0x9B, 0xF3, 0x89, 0x03, 0x5B, 0x30, 0xC3, 0x0C, 0x25},
           {0x53, 0xB2, 0x51, 0x54, 0x50, 0xFA, 0x40, 0xD9, 0xDC, 0x15,
            0x31, 0x85, 0xDE, 0xDE, 0x23, 0x9C, 0x02, 0x49, 0xAA, 0x16},
           0}};
      for (auto& fix : s_fixes) {
        if (!fix.addr) {
          for (uint32_t a = 0x82000000u; a < 0x82E00000u && !fix.addr;
               a += 4) {
            if (r32(a) != (uint32_t(fix.orig[0]) << 24 |
                           uint32_t(fix.orig[1]) << 16 |
                           uint32_t(fix.orig[2]) << 8 | fix.orig[3])) {
              continue;
            }
            bool all = true;
            for (int k = 4; k < 20 && all; ++k) {
              all = *(base + a + k) == fix.orig[k];
            }
            if (all) fix.addr = a;
          }
          if (fix.addr) {
            XELOGE("RB3DX UI PROBE[{}]: TU5 hash table entry found @{:08X}",
                   sample, fix.addr);
          }
        }
        if (fix.addr && *(base + fix.addr) != fix.fixed[0]) {
          // The table likely lives in a read-only image section; unprotect
          // the pages first (same rule as PPC bytepatches).
          if (auto* heap = memory->LookupHeap(fix.addr)) {
            heap->Protect(fix.addr & ~0xFFFu, 0x2000,
                          kMemoryProtectRead | kMemoryProtectWrite);
          }
          std::memcpy(base + fix.addr, fix.fixed, 20);
          XELOGE("RB3DX UI PROBE[{}]: TU5 hash poked @{:08X}", sample,
                 fix.addr);
        }
      }
    }
    // --- one-shot handshake-object locator: the boot-blocking wait loop
    // (fn 0x827414E0) waits on two event handles at obj+0x8C/+0x90 with the
    // state word at obj+0x94. Kernel handles are 0xF80000xx; find adjacent
    // handle pairs in heap/statics and dump the surrounding object.
    static bool s_hs_locate_done = false;
    if (!s_hs_locate_done && sample == 12) {
      s_hs_locate_done = true;
      int found = 0;
      auto scan_range = [&](uint32_t lo, uint32_t hi) {
        for (uint32_t page = lo; page < hi && found < 10; page += 0x1000) {
          if (!readable(page)) continue;
          for (uint32_t a = page; a < page + 0x1000 - 8; a += 4) {
            uint32_t v1 = r32(a), v2 = r32(a + 4);
            if ((v1 & 0xFFFFFF03) == 0xF8000000 && v1 != v2 &&
                (v2 & 0xFFFFFF03) == 0xF8000000 && (v2 - v1) < 0x40) {
              uint32_t obj = a - 0x8C;
              std::string row;
              for (uint32_t d = 0x80; d <= 0xA8; d += 4) {
                row += fmt::format(" +{:02X}={:08X}", d, r32(obj + d));
              }
              XELOGE(
                  "RB3DX UI PROBE[{}]: HS-OBJ candidate obj={:08X} "
                  "handles={:08X}/{:08X}{}",
                  sample, obj, v1, v2, row);
              if (++found >= 10) break;
            }
          }
        }
      };
      scan_range(0x82C34400, 0x83000000);
      scan_range(0x40000000, 0x44000000);
    }
    // --- one-shot TheLoadMgr locator (clean-TU5 loader-freeze) ---
    // Milo std::list embeds its dummy node inside the owning object, so any
    // queued Loader* value found in a heap list node whose next/prev points
    // into static .data/.bss locates TheLoadMgr.mLoading directly -- no
    // symbols needed. Armed by the first nonzero panel mLoader seen below.
    static uint32_t s_locate_loader_target = 0;
    static bool s_locate_done = false;
    if (s_locate_loader_target && !s_locate_done) {
      s_locate_done = true;
      const uint32_t lo = 0x40000000, hi = 0x48000000;
      int hits = 0;
      for (uint32_t page = lo; page < hi && hits < 8; page += 0x1000) {
        if (!readable(page)) continue;
        for (uint32_t a = page; a < page + 0x1000; a += 4) {
          if (r32(a) != s_locate_loader_target) continue;
          uint32_t node = a - 8;
          uint32_t nxt = r32(node), prv = r32(node + 4);
          bool nxt_static = nxt >= 0x82C34400 && nxt < 0x83000000;
          bool prv_static = prv >= 0x82C34400 && prv < 0x83000000;
          XELOGE(
              "RB3DX UI PROBE[{}]: LOADMGR-LOCATE hit value@{:08X} node={:08X} "
              "next={:08X}{} prev={:08X}{}",
              sample, a, node, nxt, nxt_static ? "(STATIC)" : "", prv,
              prv_static ? "(STATIC)" : "");
          // Walk the prev chain until it leaves the heap: the terminal static
          // address is the embedded list dummy inside TheLoadMgr.
          uint32_t walk = prv;
          std::string walked;
          for (int w = 0; w < 32 && walk; ++w) {
            walked += fmt::format(" {:08X}", walk);
            if (walk >= 0x82C34400 && walk < 0x83000000) break;
            if (walk < 0x40000000 || walk >= 0x50000000) break;
            walk = r32(walk + 4);
          }
          XELOGE("RB3DX UI PROBE[{}]: LOADMGR-LOCATE prevwalk:{}", sample,
                 walked);
          for (uint32_t st : {nxt_static ? nxt : 0u, prv_static ? prv : 0u}) {
            if (!st) continue;
            std::string row;
            for (uint32_t d = st - 0x20; d < st + 0x40; d += 4) {
              row += fmt::format(" {:08X}", r32(d));
            }
            XELOGE("RB3DX UI PROBE[{}]: LOADMGR-LOCATE static {:08X}-0x20:{}"
                   , sample, st, row);
          }
          if (++hits >= 8) break;
        }
      }
    }
    // --- panels of the transition screen and current screen ---
    // RB3-360 UIScreen: mPanelList is an EMBEDDED circular {next,prev} dummy
    // at screen+0x28 (calibrated empirically: walking with s+0x2C as the
    // sentinel yielded a phantom node at s+0x28 whose "panel" was
    // mFocusPanel@+0x30). Nodes = {next@0, prev@4, PanelRef@8}.
    auto dump_panels = [&](uint32_t s, const char* tag) {
      if (!s) return;
      uint32_t dummy = s + 0x28;
      uint32_t node = r32(dummy);
      int n = 0;
      while (node && node != dummy && n < 24) {
        uint32_t panel = r32(node + 0x8);
        uint32_t active = r8(node + 0xC);
        uint32_t loaded_ref = r8(node + 0xE);
        if (panel) {
          uint32_t vb = panel_vbase(panel);
          std::string pname = vb ? rstr(r32(vb + 0x18)) : "<?>";
          uint32_t pstate = r32(panel + 0x20);
          uint32_t ploaded = r8(panel + 0x1C);
          uint32_t ploader = r32(panel + 0xC);
          uint32_t prefs = r32(panel + 0x28);
          if (ploader && !s_locate_loader_target) {
            s_locate_loader_target = ploader;
          }
          XELOGE(
              "RB3DX UI PROBE[{}]:   {}panel[{}]@0x{:08X} 0x{:08X}'{}' "
              "active={} refLoaded={} mState={} mLoaded={} mLoader=0x{:08X} "
              "mLoadRefs={}",
              sample, tag, n, node, panel, pname, active, loaded_ref, pstate,
              ploaded, ploader, prefs);
          // SyncGameStartPanel ('sync_audio_net_panel'): the song-start sync
          // gate. Retail layout (rb3-xenon SyncGameStartPanel.h, verified vs
          // retail ctor): own mState @0x3C (0..3 lockstep, 4=StartGame issued,
          // 5=synced/IsLoaded), LockStepMgr member @0x40 with mLockMachine
          // @+0x1C (InLock != 0), mHasResponded @+0x28, mLockSuccess @+0x29.
          if (pname == "sync_audio_net_panel") {
            XELOGE(
                "RB3DX UI PROBE[{}]:     syncstart mState={} "
                "lockMachine=0x{:08X} hasResponded={} lockSuccess={} "
                "externalBlock={}",
                sample, r32(panel + 0x3C), r32(panel + 0x40 + 0x1C),
                r8(panel + 0x40 + 0x28), r8(panel + 0x40 + 0x29),
                r8(panel + 0x80));
          }
        }
        node = r32(node);
        ++n;
      }
    };
    dump_panels(trans, "T:");
    if (cur != trans) dump_panels(cur, "C:");
    // --- find saveload_mgr / net_sync via main-dir hash (once) ---
    static bool s_dumped_names = false;
    if (!saveload_obj || !netsync_obj || !session_obj || !overshell_obj ||
        (!s_dumped_names && cur)) {
      uint32_t dir = r32(kMainDirPtr);
      if (dir) {
        uint32_t entries = r32(dir + 0x8);
        int size = static_cast<int>(r32(dir + 0xC));
        if (entries && size > 0 && size < 200000) {
          for (int i = 0; i < size; ++i) {
            uint32_t name_p = r32(entries + i * 8);
            uint32_t obj = r32(entries + i * 8 + 4);
            if (!name_p || !obj) continue;
            std::string nm = rstr(name_p);
            if (nm == "saveload_mgr" && !saveload_obj) {
              saveload_obj = obj;
              XELOGE("RB3DX UI PROBE: found saveload_mgr obj=0x{:08X}",
                     saveload_obj);
            } else if (nm == "net_sync" && !netsync_obj) {
              netsync_obj = obj;
              XELOGE("RB3DX UI PROBE: found net_sync obj=0x{:08X}",
                     netsync_obj);
            } else if (nm == "session" && !session_obj) {
              session_obj = obj;
              XELOGE("RB3DX UI PROBE: found session obj=0x{:08X}", session_obj);
            } else if (nm == "overshell" && !overshell_obj) {
              overshell_obj = obj;
              XELOGE("RB3DX UI PROBE: found overshell obj=0x{:08X}",
                     overshell_obj);
            }
            if (saveload_obj && netsync_obj && session_obj && overshell_obj)
              break;
          }
          // One-time: dump the whole main-dir name table so session/overshell
          // objects can be located offline.
          if (!s_dumped_names && cur) {
            s_dumped_names = true;
            for (int i = 0; i < size; ++i) {
              uint32_t name_p = r32(entries + i * 8);
              uint32_t obj = r32(entries + i * 8 + 4);
              if (!name_p || !obj) continue;
              std::string nm = rstr(name_p);
              XELOGE("RB3DX UI PROBE: maindir['{}'] = 0x{:08X}", nm, obj);
            }
          }
        }
      }
    }
    // --- saveload_mgr / net_sync raw windows ---
    // ObjectDir::Entry.obj stores the Hmx::Object* BASE pointer. For classes
    // with a VIRTUAL Object base (MsgSource-derived SaveLoadManager; NetSync)
    // that is the vbase at the object's TAIL, so the derived fields live at
    // NEGATIVE offsets. Dump a raw window below the vbase for offline
    // calibration; values that change frame-to-frame identify the live state
    // fields (SaveLoadManager::mState etc.).
    // Find the DERIVED object head from an Hmx::Object vbase pointer by the
    // inverse of the vbptr math: head H has vfptr@H, vbptr P@H+4, and
    // [P+4] == vbase - (H+4). Scan down from the vbase.
    auto find_derived_head = [&](uint32_t vbase) -> uint32_t {
      for (uint32_t h = vbase - 8; h + 0x400 >= vbase; h -= 4) {
        uint32_t vf = r32(h);
        if (vf < 0x82000000 || vf >= 0x83000000) continue;
        uint32_t vbp = r32(h + 4);
        if (vbp < 0x82000000 || vbp >= 0x83000000) continue;
        uint32_t delta = r32(vbp + 4);
        if (h + 4 + delta == vbase) return h;
      }
      return 0;
    };
    auto dump_obj = [&](const char* tag, uint32_t o) {
      if (!o) return;
      std::string ident = rstr(r32(o + 0x18));
      uint32_t head = find_derived_head(o);
      // Labeled dump relative to the derived head when found, else relative
      // to the vbase (plain non-virtual Object base: fields ABOVE o).
      uint32_t base_addr = head ? head : o;
      XELOGE("RB3DX UI PROBE[{}]:   {}('{}') head=0x{:08X} vbase=0x{:08X}",
             sample, tag, ident, head, o);
      for (uint32_t row = 0; row < 0xC0; row += 0x20) {
        std::string words;
        for (uint32_t k = 0; k < 0x20; k += 4) {
          words += fmt::format(" {:08X}", r32(base_addr + row + k));
        }
        XELOGE("RB3DX UI PROBE[{}]:   {}[+0x{:02X}]:{}", sample, tag, row,
               words);
      }
    };
    dump_obj("saveload_mgr", saveload_obj);
    dump_obj("net_sync", netsync_obj);
    // session (NetSession): the join gate. mState @head+0x68, mQNet @head+0x70
    // (offsets from the guest IsHost disassembly). mState kIdle(0) -> not
    // requesting; kRequestingNewUser(7) -> the online join was issued and is
    // waiting for a response (the stall the offline-join fix targets).
    if (session_obj) {
      uint32_t head = find_derived_head(session_obj);
      uint32_t b = head ? head : session_obj;
      XELOGE(
          "RB3DX UI PROBE[{}]:   session head=0x{:08X} mState={} mQNet=0x{:08X} "
          "mUsers[begin=0x{:08X} end=0x{:08X}]",
          sample, b, r32(b + 0x68), r32(b + 0x70), r32(b + 0x14),
          r32(b + 0x18));
    }
    // --- gJoypadData[4]: the two-pad join gate. Base/stride decoded from the
    // running xex's JoypadGetPadData @0x82524998 (RB3E PORT_JOYPADGETPADDATA):
    // lis 0x82CD; mulli r10,r3,0xD4; addi -0x4D38 => gJoypadData = 0x82CCB2C8,
    // 0xD4 per pad. Offsets from rb3-xenon Joypad.h: mButtons@0x00,
    // mUser@0x44, mConnected@0x48, mType@0x6C. If a pad polls SUCCESS at the
    // XamInput layer but mConnected stays 0 here, the connection is dying
    // inside the guest reader thread, not in the HID driver.
    {
      const uint32_t kJoypadData = 0x82CCB2C8;
      std::string pads;
      for (int p = 0; p < 4; ++p) {
        uint32_t b2 = kJoypadData + p * 0xD4;
        pads += fmt::format(" [{}]conn={} user=0x{:08X} type={} btn=0x{:04X}",
                            p, r8(b2 + 0x48), r32(b2 + 0x44),
                            static_cast<int32_t>(r32(b2 + 0x6C)),
                            r32(b2 + 0x0));
      }
      XELOGE("RB3DX UI PROBE[{}]:   joypads:{}", sample, pads);
    }
    // --- overshell slots + per-slot join lists ---
    // maindir['overshell'] is the OvershellPanel's Hmx::Object VBASE (tail,
    // +0x4D4 from the head per the rb3-xenon RTTI note). Rather than trust
    // that constant, auto-locate mSlots ONCE: scan the head region for a
    // vector triple {begin<=end<=cap} of 3..8 guest pointers whose pointees
    // read a plausible mSlotNum (int 0..7 at slot+0x40, matching its index).
    // Slot layout (retail, rb3-xenon OvershellSlot.h): mState@0x2C,
    // mSlotNum@0x40, mPotentialUsers vector@0x6C of {LocalBandUser*, JoinState}
    // 8-byte entries.
    static uint32_t s_overshell_slots_vec = 0;
    if (overshell_obj && !s_overshell_slots_vec) {
      uint32_t head = overshell_obj - 0x4D4;
      for (uint32_t off = 0; off < 0x200 && !s_overshell_slots_vec; off += 4) {
        uint32_t b2 = r32(head + off), e2 = r32(head + off + 4);
        if (!b2 || e2 <= b2 || (e2 - b2) % 4 != 0) continue;
        uint32_t n = (e2 - b2) / 4;
        if (n < 3 || n > 8) continue;
        bool ok = true;
        for (uint32_t i = 0; i < n && ok; ++i) {
          uint32_t slot = r32(b2 + i * 4);
          if (slot < 0x40000000 || slot >= 0x80000000 ||
              r32(slot + 0x40) != i) {
            ok = false;
          }
        }
        if (ok) {
          s_overshell_slots_vec = head + off;
          XELOGE(
              "RB3DX UI PROBE: overshell head=0x{:08X} mSlots located at "
              "+0x{:X} ({} slots)",
              head, off, n);
        }
      }
    }
    if (s_overshell_slots_vec) {
      uint32_t b2 = r32(s_overshell_slots_vec), e2 = r32(s_overshell_slots_vec + 4);
      for (uint32_t sp = b2; sp < e2 && sp < b2 + 0x20; sp += 4) {
        uint32_t slot = r32(sp);
        if (!slot) continue;
        uint32_t pu_b = r32(slot + 0x6C), pu_e = r32(slot + 0x70);
        std::string pus;
        for (uint32_t p = pu_b; p + 8 <= pu_e && p < pu_b + 0x40; p += 8) {
          pus += fmt::format(" {{user=0x{:08X} join={}}}", r32(p), r32(p + 4));
        }
        XELOGE(
            "RB3DX UI PROBE[{}]:   oshell slot{} @0x{:08X} state=0x{:08X} "
            "npot={}{}",
            sample, r32(slot + 0x40), slot, r32(slot + 0x2C),
            (pu_e > pu_b) ? (pu_e - pu_b) / 8 : 0, pus);
      }
    }
  }
}

void Dc3NuiSequencerExtern(
    cpu::ppc::PPCContext* ppc_context, kernel::KernelState* kernel_state) {
  uint32_t frame_guest_addr = static_cast<uint32_t>(ppc_context->r[4]);
  Memory* memory = kernel_state->memory();
  static int s_skel_calls = 0;
  static uint32_t s_last_screen = 0;
  static int s_screen_stable_count = 0;
  static uint32_t s_fake_frame_number = 0;
  static bool s_nui_entry_logged = false;
  static bool s_screen_name_scan_range_logged = false;
  static uint32_t s_scan_name_min = 0;
  static uint32_t s_scan_name_max = 0;
  static std::unordered_map<std::string, uint32_t> s_name_literal_cache;
  static bool s_loadsong_probe_logged = false;
  static bool s_loadsong_repair_attempted = false;
  static bool s_content_refresh_forced = false;
  static bool s_host_beat_drive_active = false;
  static float s_host_song_seconds = 0.0f;
  static float s_host_song_beat = 0.0f;
  static uint32_t s_last_stuck_cur_screen = 0;
  static uint32_t s_last_stuck_trans_screen = 0;
  static uint32_t s_last_stuck_trans_state = 0;
  static int s_stuck_transition_count = 0;

  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }

  if (!frame_guest_addr) {
    ppc_context->r[3] = 0x80004003u;
    return;
  }

  auto* frame = memory->TranslateVirtual<uint8_t*>(frame_guest_addr);
  if (!frame) {
    ppc_context->r[3] = 0x80004005u;
    return;
  }

  auto write_u32 = [frame](uint32_t offset, uint32_t value) {
    xe::store_and_swap<uint32_t>(frame + offset, value);
  };
  auto write_u64 = [frame](uint32_t offset, uint64_t value) {
    xe::store_and_swap<uint64_t>(frame + offset, value);
  };
  auto write_float = [frame](uint32_t offset, float value) {
    xe::store_and_swap<float>(frame + offset, value);
  };

  uint32_t frame_number = ++s_fake_frame_number;
  constexpr uint32_t kFrameSize = 0x30 + 6 * 0x1B4;
  std::memset(frame, 0, kFrameSize);

  write_u64(0x00, static_cast<uint64_t>(frame_number) * 33333);
  write_u32(0x08, frame_number);
  write_float(0x10, 0.0f);
  write_float(0x14, 1.0f);
  write_float(0x18, 0.0f);
  write_float(0x1C, 0.0f);
  write_float(0x20, 0.0f);
  write_float(0x24, 1.0f);
  write_float(0x28, 0.0f);
  write_float(0x2C, 0.0f);

  constexpr uint32_t kSkel0 = 0x30;
  write_u32(kSkel0 + 0x00, 2);
  write_u32(kSkel0 + 0x04, 1);
  write_float(kSkel0 + 0x10, 0.0f);
  write_float(kSkel0 + 0x14, 0.9f);
  write_float(kSkel0 + 0x18, 2.0f);
  write_float(kSkel0 + 0x1C, 0.0f);

  struct JointPos {
    float x;
    float y;
    float z;
  };
  static constexpr JointPos kJoints[20] = {
      {0.00f, 0.90f, 2.0f},  {0.00f, 1.10f, 2.0f},  {0.00f, 1.40f, 2.0f},
      {0.00f, 1.60f, 2.0f},  {-0.20f, 1.40f, 2.0f}, {-0.30f, 1.10f, 2.0f},
      {-0.25f, 0.90f, 2.0f}, {-0.20f, 0.80f, 2.0f}, {0.20f, 1.40f, 2.0f},
      {0.30f, 1.10f, 2.0f},  {0.25f, 0.90f, 2.0f},  {0.20f, 0.80f, 2.0f},
      {-0.10f, 0.90f, 2.0f}, {-0.10f, 0.50f, 2.0f}, {-0.10f, 0.10f, 2.0f},
      {-0.10f, 0.00f, 2.0f}, {0.10f, 0.90f, 2.0f},  {0.10f, 0.50f, 2.0f},
      {0.10f, 0.10f, 2.0f},  {0.10f, 0.00f, 2.0f},
  };
  constexpr uint32_t kJointsOff = kSkel0 + 0x20;
  for (int j = 0; j < 20; ++j) {
    uint32_t off = kJointsOff + j * 16;
    write_float(off + 0, kJoints[j].x);
    write_float(off + 4, kJoints[j].y);
    write_float(off + 8, kJoints[j].z);
    write_float(off + 12, 0.0f);
  }

  s_skel_calls++;
  ppc_context->r[3] = 0;

  if (!s_nui_entry_logged) {
    XELOGI("DC3: NUI callback alive (original layout) frameBuf={:08X} "
           "fakeFrame={} nuiFrame={}",
           frame_guest_addr, frame_number, s_skel_calls);
    s_nui_entry_logged = true;
  }

  if (!s_scan_name_max) {
    if (auto module = kernel_state->GetExecutableModule()) {
      if (auto* xex = module->xex_module()) {
        if (auto* rdata = xex->GetPESection(".rdata")) {
          s_scan_name_min = rdata->address;
          s_scan_name_max = rdata->address + rdata->size;
        }
      }
    }
    if (!s_scan_name_max) {
      // Original debug XEX strings live in low 0x82xxxxxx; keep the fallback
      // tight so speculative scans don't treat arbitrary data as names.
      s_scan_name_min = 0x82000000;
      s_scan_name_max = 0x82400000;
    }
  }
  if (!s_screen_name_scan_range_logged) {
    XELOGI("DC3: screen-name scan range {:08X}-{:08X}", s_scan_name_min,
           s_scan_name_max);
    s_screen_name_scan_range_logged = true;
  }

  // Force GestureMgr.mInControllerMode=true so the game processes
  // XInput button presses for menu navigation (DC3 normally uses Kinect).
  {
    constexpr uint32_t kTheGestureMgr = 0x82F5F7B4;
    constexpr uint32_t kInControllerModeOff = 0x426D;
    auto* gm_slot = memory->TranslateVirtual<uint8_t*>(kTheGestureMgr);
    if (gm_slot) {
      uint32_t gm_addr = xe::load_and_swap<uint32_t>(gm_slot);
      if (gm_addr && gm_addr < 0xF0000000) {
        auto* gm = memory->TranslateVirtual<uint8_t*>(gm_addr);
        if (gm) {
          gm[kInControllerModeOff] = 1;
        }
      }
    }
  }

  auto is_guest_readable = [&](uint32_t guest_addr, uint32_t size) -> bool {
    if (!guest_addr || guest_addr >= 0xF0000000 || !size) {
      return false;
    }
    uint32_t guest_end = guest_addr + size - 1;
    if (guest_end < guest_addr) {
      return false;
    }
    auto* heap = memory->LookupHeap(guest_addr);
    if (!heap) {
      return false;
    }
    return heap->QueryRangeAccess(guest_addr, guest_end) !=
           xe::memory::PageAccess::kNoAccess;
  };

  constexpr uint32_t kTheUI = 0x82F1A8E0;
  auto* ui_ptr = is_guest_readable(kTheUI, 4)
                     ? memory->TranslateVirtual<uint8_t*>(kTheUI)
                     : nullptr;
  uint32_t ui_addr = ui_ptr ? xe::load_and_swap<uint32_t>(ui_ptr) : 0;
  if (is_guest_readable(ui_addr, 0x50)) {
    auto* ui_obj = memory->TranslateVirtual<uint8_t*>(ui_addr);
    if (ui_obj) {
      uint32_t cur_screen_h = xe::load_and_swap<uint32_t>(ui_obj + 0x48);
      uint32_t trans_state_h = xe::load_and_swap<uint32_t>(ui_obj + 0x2c);
      uint32_t trans_screen_h = xe::load_and_swap<uint32_t>(ui_obj + 0x4c);
      if (cur_screen_h != s_last_screen) { s_last_screen = cur_screen_h; s_screen_stable_count = 0; }
      else if (trans_state_h == 0) s_screen_stable_count++;

      // Blocker 2 (song content not ready): the song .milo / RndPropAnim
      // FileMerger merge runs ASYNC, driven by the MAIN thread's LoadMgr::Poll.
      // We must NOT force the loading->game_screen transition until that merge
      // completes, or HamDirector::Enter reads a half-built anim (corrupt
      // mSongAnims/mPropKeys -> operator[] crash + GetKeys hang). The merge is
      // in flight while TheFileMergerOrganizer(*0x82f5ef44)->mActiveOrg(+0x38)
      // is non-null. We canNOT pump LoadMgr ourselves (races the main thread ->
      // crash); we just HOLD the transition so the main thread can finish, then
      // release. (mActiveOrg per src/system/char/FileMergerOrganizer.h:67.)
      bool merge_busy = false;
      {
        uint32_t org = is_guest_readable(0x82f5ef44, 4)
                           ? xe::load_and_swap<uint32_t>(
                                 memory->TranslateVirtual<uint8_t*>(0x82f5ef44))
                           : 0;
        if (org && is_guest_readable(org + 0x38, 4)) {
          merge_busy = xe::load_and_swap<uint32_t>(
                           memory->TranslateVirtual<uint8_t*>(org + 0x38)) != 0;
        }
        static bool s_merge_seen_busy = false;
        if (merge_busy) s_merge_seen_busy = true;
        // Only gate once we've actually observed a merge start (avoids holding
        // forever if the organizer is idle for unrelated reasons before the
        // song load is even queued).
        if (!s_merge_seen_busy) merge_busy = false;
        if ((s_skel_calls % 120) == 0) {
          XELOGI("DC3: merge_busy={} org={:08X} seenBusy={}", merge_busy ? 1 : 0,
                 org, s_merge_seen_busy ? 1 : 0);
        }
      }

      auto read_guest_name = [&](uint32_t name_ptr, bool strict_scan_range)
          -> std::string {
        if (!name_ptr || name_ptr >= 0xF0000000) {
          return "";
        }
        if (strict_scan_range &&
            (name_ptr < s_scan_name_min || name_ptr >= s_scan_name_max)) {
          return "";
        }
        std::string result;
        result.reserve(32);
        for (uint32_t i = 0; i < 64; ++i) {
          if (!is_guest_readable(name_ptr + i, 1)) {
            return "";
          }
          auto* ch_ptr = memory->TranslateVirtual<uint8_t*>(name_ptr + i);
          char ch = static_cast<char>(*ch_ptr);
          if (!ch) {
            return result;
          }
          unsigned char uch = static_cast<unsigned char>(ch);
          if (!(std::isalnum(uch) || ch == '_')) {
            return "";
          }
          result.push_back(ch);
        }
        return "";
      };

      auto read_name_ptr_at = [&](uint32_t screen_addr, uint32_t offset,
                                  bool strict_scan_range) -> uint32_t {
        if (!screen_addr || screen_addr >= 0xF0000000) {
          return 0;
        }
        if (!is_guest_readable(screen_addr + offset, 4)) {
          return 0;
        }
        auto* screen = memory->TranslateVirtual<uint8_t*>(screen_addr);
        uint32_t name_ptr = xe::load_and_swap<uint32_t>(screen + offset);
        if (read_guest_name(name_ptr, strict_scan_range).empty()) {
          return 0;
        }
        return name_ptr;
      };

      auto read_name_at = [&](uint32_t screen_addr, uint32_t offset,
                              bool strict_scan_range) -> std::string {
        return read_guest_name(
            read_name_ptr_at(screen_addr, offset, strict_scan_range),
            strict_scan_range);
      };
      auto find_name_literal_ptr = [&](const std::string& target_name)
          -> uint32_t {
        auto it = s_name_literal_cache.find(target_name);
        if (it != s_name_literal_cache.end() &&
            read_guest_name(it->second, false) == target_name) {
          return it->second;
        }

        auto scan_for_literal = [&](uint32_t range_min,
                                    uint32_t range_max) -> uint32_t {
          if (!range_min || range_min >= range_max) {
            return 0;
          }
          const size_t target_len = target_name.size();
          for (uint32_t addr = range_min; addr + target_len < range_max;
               ++addr) {
            if (!is_guest_readable(addr, static_cast<uint32_t>(target_len + 1))) {
              continue;
            }
            auto* candidate = memory->TranslateVirtual<uint8_t*>(addr);
            if (!candidate) {
              continue;
            }
            if (std::memcmp(candidate, target_name.data(), target_len) == 0 &&
                candidate[target_len] == 0) {
              s_name_literal_cache[target_name] = addr;
              return addr;
            }
          }
          return 0;
        };

        uint32_t literal_ptr = 0;
        if (s_scan_name_min && s_scan_name_max && s_scan_name_min < s_scan_name_max) {
          literal_ptr = scan_for_literal(s_scan_name_min, s_scan_name_max);
        }
        if (!literal_ptr) {
          literal_ptr = scan_for_literal(0x82000000, 0x82400000);
        }
        return literal_ptr;
      };

      std::string raw_name = read_name_at(cur_screen_h, 0x1C, false);
      if (raw_name.empty()) {
        raw_name = read_name_at(cur_screen_h, 0x20, false);
      }
      std::string raw_trans_name = read_name_at(trans_screen_h, 0x1C, false);
      if (raw_trans_name.empty()) {
        raw_trans_name = read_name_at(trans_screen_h, 0x20, false);
      }
      std::string cur_name = raw_name;
      if (cur_name.empty()) {
        cur_name = "attract_screen";
      }
      std::string trans_name = raw_trans_name;

      auto* processor = kernel_state->processor();
      auto* thread_state = ppc_context->thread_state;
      auto exec_guest_bool = [&](uint32_t fn, uint64_t arg0,
                                 uint64_t arg1 = 0) -> bool {
        if (!processor || !thread_state || !fn) {
          return false;
        }
        uint64_t args[2] = {arg0, arg1};
        return static_cast<uint32_t>(processor->Execute(thread_state, fn, args,
                                                        arg1 ? 2 : 1)) != 0;
      };

      if (trans_state_h != 0 && trans_screen_h) {
        if (cur_screen_h == s_last_stuck_cur_screen &&
            trans_screen_h == s_last_stuck_trans_screen &&
            trans_state_h == s_last_stuck_trans_state) {
          ++s_stuck_transition_count;
        } else {
          s_last_stuck_cur_screen = cur_screen_h;
          s_last_stuck_trans_screen = trans_screen_h;
          s_last_stuck_trans_state = trans_state_h;
          s_stuck_transition_count = 1;
        }
      } else {
        s_last_stuck_cur_screen = 0;
        s_last_stuck_trans_screen = 0;
        s_last_stuck_trans_state = 0;
        s_stuck_transition_count = 0;
      }

      if (trans_state_h != 0 && trans_screen_h && processor && thread_state &&
          (s_stuck_transition_count == 1 ||
           (s_stuck_transition_count % 60) == 0)) {
        constexpr uint32_t kUIScreenEntering = 0x827A34F8;
        constexpr uint32_t kUIScreenExiting = 0x827A35C0;
        constexpr uint32_t kUIScreenCheckIsLoaded = 0x827A3A00;
        int trans_loaded = -1;
        int cur_exiting = -1;
        int cur_entering = -1;
        if (trans_state_h == 1) {
          trans_loaded =
              exec_guest_bool(kUIScreenCheckIsLoaded, trans_screen_h) ? 1 : 0;
          cur_exiting = cur_screen_h
                            ? (exec_guest_bool(kUIScreenExiting, cur_screen_h) ? 1
                                                                                : 0)
                            : 0;
          cur_entering =
              exec_guest_bool(kUIScreenEntering, trans_screen_h) ? 1 : 0;
        } else if (trans_state_h == 2) {
          cur_entering = cur_screen_h
                             ? (exec_guest_bool(kUIScreenEntering, cur_screen_h) ? 1
                                                                                 : 0)
                             : 0;
        }
        XELOGI(
            "DC3: Transition diag cur='{}' trans='{}' state={} stable={} "
            "loaded={} curExiting={} curEntering={}",
            cur_name, trans_name, trans_state_h, s_stuck_transition_count,
            trans_loaded, cur_exiting, cur_entering);
      }

      if (trans_state_h == 1 && trans_screen_h && processor && thread_state &&
          s_stuck_transition_count >= 120) {
        constexpr uint32_t kUIScreenExiting = 0x827A35C0;
        constexpr uint32_t kUIScreenCheckIsLoaded = 0x827A3A00;
        constexpr uint32_t kUIScreenEnter = 0x827A51E0;
        bool trans_loaded = exec_guest_bool(kUIScreenCheckIsLoaded, trans_screen_h);
        bool cur_exiting =
            cur_screen_h ? exec_guest_bool(kUIScreenExiting, cur_screen_h) : false;
        bool allow_force_enter =
            trans_loaded && (!cur_exiting || s_stuck_transition_count >= 180) &&
            !(trans_name == "game_screen" && merge_busy);
        if (allow_force_enter) {
          uint32_t old_cur_screen = cur_screen_h;
          xe::store_and_swap<uint32_t>(ui_obj + 0x2C, 2);
          xe::store_and_swap<uint32_t>(ui_obj + 0x48, trans_screen_h);
          xe::store_and_swap<uint32_t>(ui_obj + 0x4C, old_cur_screen);
          uint64_t enter_args[2] = {trans_screen_h, old_cur_screen};
          processor->Execute(thread_state, kUIScreenEnter, enter_args, 2);
          XELOGI(
              "DC3: Force-entered stuck transition '{}' -> '{}' after {} NUI frames "
              "(loaded={} curExiting={})",
              cur_name, trans_name, s_stuck_transition_count,
              trans_loaded ? 1 : 0, cur_exiting ? 1 : 0);
          s_last_screen = trans_screen_h;
          s_screen_stable_count = 0;
          s_last_stuck_cur_screen = 0;
          s_last_stuck_trans_screen = 0;
          s_last_stuck_trans_state = 0;
          s_stuck_transition_count = 0;
          cur_screen_h = trans_screen_h;
          trans_state_h = 2;
          trans_screen_h = old_cur_screen;
          raw_name = raw_trans_name;
          raw_trans_name = read_name_at(old_cur_screen, 0x1C, false);
          if (raw_trans_name.empty()) {
            raw_trans_name = read_name_at(old_cur_screen, 0x20, false);
          }
          cur_name = trans_name;
          trans_name = raw_trans_name;
        } else if (!trans_loaded && s_stuck_transition_count >= 120 &&
                   (trans_name != "game_screen" ||
                    (cvars::dc3_ik_telemetry && !merge_busy))) {
          xe::store_and_swap<uint32_t>(ui_obj + 0x48, trans_screen_h);
          xe::store_and_swap<uint32_t>(ui_obj + 0x4C, 0);
          xe::store_and_swap<uint32_t>(ui_obj + 0x2C, 0);
          XELOGI(
              "DC3: Force-completed unloaded menu transition '{}' -> '{}' after {} NUI frames",
              cur_name, trans_name, s_stuck_transition_count);
          s_last_screen = trans_screen_h;
          s_screen_stable_count = 0;
          s_last_stuck_cur_screen = 0;
          s_last_stuck_trans_screen = 0;
          s_last_stuck_trans_state = 0;
          s_stuck_transition_count = 0;
          cur_screen_h = trans_screen_h;
          trans_state_h = 0;
          trans_screen_h = 0;
          raw_name = raw_trans_name;
          raw_trans_name.clear();
          cur_name = trans_name;
          trans_name.clear();
        }
      }

      if (trans_state_h == 2 && cur_screen_h && processor && thread_state &&
          s_stuck_transition_count >= 120) {
        constexpr uint32_t kUIScreenEntering = 0x827A34F8;
        bool cur_entering = exec_guest_bool(kUIScreenEntering, cur_screen_h);
        if (cur_entering || s_stuck_transition_count >= 240) {
          // Force-complete the entering phase.  If curEntering is false but
          // we've been stuck for 240+ NUI frames, the enter animation
          // already finished but transState was never cleared (common after
          // Force-entered transitions).
          xe::store_and_swap<uint32_t>(ui_obj + 0x2C, 0);
          xe::store_and_swap<uint32_t>(ui_obj + 0x4C, 0);
          XELOGI(
              "DC3: Force-completed stuck enter for '{}' after {} NUI frames"
              " (curEntering={})",
              cur_name, s_stuck_transition_count, cur_entering ? 1 : 0);
          s_last_screen = cur_screen_h;
          s_screen_stable_count = 0;
          s_last_stuck_cur_screen = 0;
          s_last_stuck_trans_screen = 0;
          s_last_stuck_trans_state = 0;
          s_stuck_transition_count = 0;
          trans_state_h = 0;
          trans_screen_h = 0;
          raw_trans_name.clear();
          trans_name.clear();
        }
      }

      // (try_bootstrap_gameplay lambda deleted -- retired experiment;
      // see the NOTE below about GamePanel::CreateGame blocking. fork-cleanup C.)

      if ((s_skel_calls % 60) == 0) {
        XELOGI("DC3: Nav diag: scr={:08X} raw='{}' name='{}' stable={} nui={}",
               cur_screen_h, raw_name, cur_name, s_screen_stable_count,
               s_skel_calls);
      }

      // GATE PROBE (diagnostic): when at game_screen but the GamePanel state
      // machine hasn't entered intro/playing (mState==0), poll the three
      // PollForLoading gates from the host so we can see which one blocks
      // mPollLoadState from reaching 4 (which gates GamePanel::Poll's
      // StartIntro/StartGame). Pure-read guest fns, fired a few times only.
      static int s_gate_probe_count = 0;
      if (cvars::dc3_gameplay_probe && cur_name == "game_screen" &&
          (s_skel_calls % 120) == 0 && s_gate_probe_count < 64) {
        auto* gp_probe = kernel_state->processor();
        auto* ts_probe = ppc_context->thread_state;
        if (gp_probe && ts_probe) {
          constexpr uint32_t kTheGamePanelPtr = 0x83117410;
          constexpr uint32_t kTheHamDirectorPtr = 0x82F603A0;
          constexpr uint32_t kTheHamWardrobePtr = 0x82F60110;
          constexpr uint32_t kIsWorldLoaded = 0x82468838;
          constexpr uint32_t kAllCharsLoaded = 0x824553A0;
          constexpr uint32_t kGameIsReady = 0x82867F80;
          constexpr uint32_t kGamePanelIsLoaded = 0x8287AD78;
          constexpr uint32_t kUIPanelIsLoaded = 0x827A7190;
          auto rd = [&](uint32_t a) -> uint32_t {
            return is_guest_readable(a, 4)
                       ? xe::load_and_swap<uint32_t>(
                             memory->TranslateVirtual<uint8_t*>(a))
                       : 0;
          };
          uint32_t gp = rd(kTheGamePanelPtr);
          uint32_t hd = rd(kTheHamDirectorPtr);
          uint32_t hw = rd(kTheHamWardrobePtr);
          uint32_t game = (gp && is_guest_readable(gp + 0x38, 4))
                              ? rd(gp + 0x38)
                              : 0;
          uint32_t world_loaded = 0, chars_loaded = 0, game_ready = 0;
          if (hd) {
            uint64_t a[1] = {hd};
            world_loaded = static_cast<uint32_t>(
                gp_probe->Execute(ts_probe, kIsWorldLoaded, a, 1));
          }
          if (hw) {
            uint64_t a[1] = {hw};
            chars_loaded = static_cast<uint32_t>(
                gp_probe->Execute(ts_probe, kAllCharsLoaded, a, 1));
          }
          if (game) {
            uint64_t a[1] = {game};
            game_ready = static_cast<uint32_t>(
                gp_probe->Execute(ts_probe, kGameIsReady, a, 1));
          }
          uint32_t panel_loaded = 0, uipanel_loaded = 0, mstate = 0xFFFF;
          if (gp) {
            uint64_t a[1] = {gp};
            panel_loaded = static_cast<uint32_t>(
                gp_probe->Execute(ts_probe, kGamePanelIsLoaded, a, 1));
            uipanel_loaded = static_cast<uint32_t>(
                gp_probe->Execute(ts_probe, kUIPanelIsLoaded, a, 1));
            mstate = (gp && is_guest_readable(gp + 0x80, 4)) ? rd(gp + 0x80)
                                                             : 0xFFFF;
          }
          XELOGI("DC3: GATE PROBE gp={:08X} game={:08X} hd={:08X} hw={:08X} "
                 "worldLoaded={} charsLoaded={} gameReady={} panelLoaded={} "
                 "uiPanelLoaded={} mState={} transState={}",
                 gp, game, hd, hw, world_loaded & 0xFF, chars_loaded & 0xFF,
                 game_ready & 0xFF, panel_loaded & 0xFF, uipanel_loaded & 0xFF,
                 mstate, trans_state_h);

          // ---- PROPKEYS PROBE (diagnostic, pure memory reads) ------------
          // Goal: characterize the corrupt std::list<PropKeys*> mPropKeys that
          // RndPropAnim::GetKeys spins forever in. Walk HamDirector.mSongAnims
          // (std::map<Difficulty,RndPropAnim*> @ hd+0x5c, STLport _Rb_tree),
          // find diff 0's RndPropAnim, read mPropKeys (@ RndPropAnim+0x10,
          // STLport _List), and walk the node ring (capped at 64). Logs each
          // node's addr/next/prev/value(PropKeys*) and each PropKeys' vptr +
          // target(+0x10)/prop(+0x18)/exceptionID(+0x24). NEVER calls a guest
          // fn (GetKeys would hang); reading memory is safe.
          if (hd) {
            // STLport _Rb_tree base @ hd+0x5c:
            //   header._M_data = {color@+0, parent@+4, left@+8, right@+c},
            //   _M_node_count @ +0x10.  Map node: base{c,par,left,right}@0..c,
            //   then pair<const Difficulty,AnimPtr>: key(int)@+0x10, value=
            //   AnimPtr(=ObjRefConcrete<RndPropAnim>) @ +0x14, whose layout is
            //   {vptr@+0, next@+4, prev@+8, mObject(RndPropAnim*)@+0xc}.  So
            //   the REAL RndPropAnim* is at node+0x14+0xc = node+0x20.  Header
            //   is the end()/sentinel.
            const uint32_t map_base = hd + 0x5c;
            const uint32_t map_header = map_base;       // &header._M_data
            const uint32_t map_root = rd(map_base + 4); // header._M_parent
            const uint32_t map_count = rd(map_base + 0x10);
            XELOGI("DC3: PKPROBE mSongAnims hdr={:08X} root={:08X} count={}",
                   map_header, map_root, map_count);
            uint32_t song_anims[3] = {};
            uint32_t stack[16] = {};
            int stack_count = 0;
            if (map_root && map_root != map_header) {
              stack[stack_count++] = map_root;
            }
            for (int i = 0; i < 16 && stack_count > 0; i++) {
              uint32_t node = stack[--stack_count];
              if (!node || node == map_header || !is_guest_readable(node, 0x24)) {
                continue;
              }
              int32_t key = static_cast<int32_t>(rd(node + 0x10));
              uint32_t aptr_vptr = rd(node + 0x14);   // AnimPtr vptr
              uint32_t aptr_obj = rd(node + 0x20);    // AnimPtr.mObject
              uint32_t left = rd(node + 8);
              uint32_t right = rd(node + 0xc);
              XELOGI("DC3: PKPROBE   mapnode {:08X} key={} aptrVptr={:08X} "
                     "RndPropAnim*={:08X} L={:08X} R={:08X}",
                     node, key, aptr_vptr, aptr_obj, left, right);
              if (key >= 0 && key < 3) {
                song_anims[key] = aptr_obj;
              }
              if (right && right != map_header && stack_count < 16) {
                stack[stack_count++] = right;
              }
              if (left && left != map_header && stack_count < 16) {
                stack[stack_count++] = left;
              }
            }

            auto dump_prop_anim = [&](int diff, uint32_t song_anim) {
              XELOGI("DC3: PKPROBE diff{} RndPropAnim={:08X}", diff,
                     song_anim);
              if (!song_anim || !is_guest_readable(song_anim, 0x30)) {
                XELOGI("DC3: PKPROBE diff{} anim unreadable/NULL", diff);
                return;
              }
              uint32_t anim_vptr = rd(song_anim);
              // mPropKeys @ song_anim+0x10 is the embedded list sentinel head:
              //   head._M_next @ +0x10, head._M_prev @ +0x14.
              const uint32_t list_head = song_anim + 0x10;
              uint32_t first = rd(list_head);       // head._M_next
              uint32_t last = rd(list_head + 4);    // head._M_prev
              uint32_t word18 = rd(song_anim + 0x18);
              uint32_t word1c = rd(song_anim + 0x1c);
              XELOGI("DC3: PKPROBE diff{} anim={:08X} vptr={:08X} "
                     "listHead={:08X} head.next={:08X} head.prev={:08X} "
                     "word18={:08X} word1c={:08X}",
                     diff, song_anim, anim_vptr, list_head, first, last,
                     word18, word1c);
              // Walk the node ring. Node: next@+0, prev@+4, value(PropKeys*)@+8.
              // Proper termination: cur == list_head (returned to sentinel).
              uint32_t cur = first;
              int n = 0;
              bool reached_sentinel = false;
              uint32_t prev_seen = list_head;
              for (; n < 64; n++) {
                if (cur == list_head) {
                  reached_sentinel = true;
                  break;
                }
                if (!is_guest_readable(cur, 0xc)) {
                  XELOGI("DC3: PKPROBE   diff{} node[{}] {:08X} UNREADABLE "
                         "(prev node was {:08X}) -> CORRUPT",
                         diff, n, cur, prev_seen);
                  break;
                }
                uint32_t nxt = rd(cur);
                uint32_t prv = rd(cur + 4);
                uint32_t pk = rd(cur + 8);
                // For the PropKeys, read vptr + target(+0x10) prop(+0x18)
                // excID(+0x24).
                uint32_t pk_vptr = 0, pk_tgt = 0, pk_prop = 0, pk_exc = 0;
                bool pk_ok = (pk && is_guest_readable(pk, 0x28));
                if (pk_ok) {
                  pk_vptr = rd(pk);
                  pk_tgt = rd(pk + 0x10);
                  pk_prop = rd(pk + 0x18);
                  pk_exc = rd(pk + 0x24);
                }
                XELOGI("DC3: PKPROBE   diff{} node[{}] @{:08X} next={:08X} "
                       "prev={:08X} PropKeys={:08X} | pk_ok={} vptr={:08X} "
                       "tgt={:08X} prop={:08X} excID={}",
                       diff, n, cur, nxt, prv, pk, pk_ok ? 1 : 0, pk_vptr,
                       pk_tgt, pk_prop, pk_exc);
                prev_seen = cur;
                cur = nxt;
              }
              XELOGI("DC3: PKPROBE diff{} WALK DONE nodes={} reachedSentinel={} "
                     "(64=hit cap => RING NEVER RETURNS TO SENTINEL = the "
                     "GetKeys infinite loop)",
                     diff, n, reached_sentinel ? 1 : 0);
            };

            for (int diff = 0; diff < 3; diff++) {
              dump_prop_anim(diff, song_anims[diff]);
            }

            // Also walk mDancerFaceAnims @ hd+0x74 (same map layout). The
            // GetKeys hang is on the MAIN thread; the corrupt list may be a
            // face anim rather than a song anim, so dump those too. Tag with
            // diff+10 so the log lines are distinguishable (diff10/11/12).
            {
              const uint32_t fmap_base = hd + 0x74;
              const uint32_t fmap_header = fmap_base;
              const uint32_t fmap_root = rd(fmap_base + 4);
              const uint32_t fmap_count = rd(fmap_base + 0x10);
              XELOGI("DC3: PKPROBE mDancerFaceAnims hdr={:08X} root={:08X} "
                     "count={}",
                     fmap_header, fmap_root, fmap_count);
              uint32_t face_anims[3] = {};
              uint32_t fstack[16] = {};
              int fstack_count = 0;
              if (fmap_root && fmap_root != fmap_header) {
                fstack[fstack_count++] = fmap_root;
              }
              for (int i = 0; i < 16 && fstack_count > 0; i++) {
                uint32_t node = fstack[--fstack_count];
                if (!node || node == fmap_header ||
                    !is_guest_readable(node, 0x24)) {
                  continue;
                }
                int32_t key = static_cast<int32_t>(rd(node + 0x10));
                uint32_t aptr_obj = rd(node + 0x20);  // AnimPtr.mObject
                uint32_t left = rd(node + 8);
                uint32_t right = rd(node + 0xc);
                if (key >= 0 && key < 3) {
                  face_anims[key] = aptr_obj;
                }
                if (right && right != fmap_header && fstack_count < 16) {
                  fstack[fstack_count++] = right;
                }
                if (left && left != fmap_header && fstack_count < 16) {
                  fstack[fstack_count++] = left;
                }
              }
              for (int diff = 0; diff < 3; diff++) {
                dump_prop_anim(diff + 10, face_anims[diff]);
              }
            }

            // The GetKeys hang's r3(this)=HamDirector (0x406E8DA8/0x406E8D78),
            // so the RndPropAnim* being dereferenced is a WILD pointer aliasing
            // the HamDirector. The likely sources are the non-map anim ObjPtrs:
            //   mMasterClipAnim @ hd+0x8c  (ObjPtr<RndPropAnim>, .mObject@+0xc)
            //   mPlayer1RoutineBuilderAnim @ hd+0xa0
            //   mPlayer2RoutineBuilderAnim @ hd+0xb4
            // Dump their .mObject + each one's list. tags 20/21/22.
            {
              uint32_t master = rd(hd + 0x8c + 0xc);
              uint32_t rb1 = rd(hd + 0xa0 + 0xc);
              uint32_t rb2 = rd(hd + 0xb4 + 0xc);
              XELOGI("DC3: PKPROBE masterClipAnim={:08X} rbAnim1={:08X} "
                     "rbAnim2={:08X} (hd={:08X} -> if any == hd-region these "
                     "are the wild GetKeys 'this')",
                     master, rb1, rb2, hd);
              dump_prop_anim(20, master);
              dump_prop_anim(21, rb1);
              dump_prop_anim(22, rb2);
            }
          }
          // ---- end PROPKEYS PROBE ---------------------------------------

          s_gate_probe_count++;
        }
      }

      // NOTE (2026-06-02): tried pumping LoadMgr::Poll(&TheLoadMgr) here to drain
      // the song merge before game_screen — it RACES the main thread's own load
      // polling (confirmed: crash in LoadMgr::Poll->PollFrontLoader->
      // DataArray::Node @0x8259F758 with LoadMgr frames on the stack). The main
      // thread DOES drive LoadMgr::Poll, so this is a timing/merge-completeness
      // problem (game transitions before the merge finishes), NOT a missing
      // driver. The safe fix is to HOLD the loading->game_screen transition until
      // the song FileMerger merge completes (host-readable signal:
      // TheFileMergerOrganizer @0x82f5ef44 idle, or the song Merger::mLoaded
      // set), with NO concurrent polling. See task #21.

      int nav_stable_threshold = 20;
      // IK telemetry: use default thresholds, the nav bridge is needed
      // because scripted input A-presses don't trigger game transitions
      // (DC3 uses Kinect, not standard XInput for menu navigation).
      if (cur_name == "title_screen") {
        // Let the scripted A presses try first; only force past title after
        // it's been idle for a few seconds.
        nav_stable_threshold = 180;
      } else if (cur_name == "main_screen" ||
                 cur_name == "choose_mode_screen") {
        // Menu A-button input still flakes in the original-XEX path. Give the
        // scripted controller presses time to work before using the same
        // UIManager::GotoScreen bridge that gets us through the boot flow.
        nav_stable_threshold = 160;
      } else if (cur_name == "song_select_screen") {
        // Let the scripted DOWN/A sequence try to select a real song first.
        // If the screen never leaves song select, fall back to the same
        // headless bridge chain used by older boot probes.
        nav_stable_threshold = 260;
      } else if (cur_name == "multiuser_screen" ||
                 cur_name == "loading_screen" ||
                 cur_name == "preloading_screen" ||
                 cur_name == "real_loading_screen") {
        nav_stable_threshold = 120;
      } else if (cur_name == "wait_main_after_saveload_screen") {
        // Save/load is stubbed in the original-XEX headless path, so if the
        // screen settles without firing its completion handler, advance to the
        // real menu flow after a short grace period.
        nav_stable_threshold = 120;
      }

      if (s_screen_stable_count >= nav_stable_threshold && trans_state_h == 0) {
        std::string target_name;
        if (cur_screen_h && cur_name == "attract_screen") {
          target_name = "title_screen";
        } else if (cur_screen_h && cur_name == "title_screen") {
          target_name = "wait_main_after_saveload_screen";
        } else if (cur_screen_h && cur_name == "wait_main_after_saveload_screen") {
          target_name = "main_screen";
        } else if (cur_screen_h && cur_name == "main_screen") {
          target_name = "choose_mode_screen";
        } else if (cur_screen_h && cur_name == "choose_mode_screen") {
          target_name = "song_select_screen";
        } else if (cur_screen_h && cur_name == "song_select_screen") {
          target_name = "multiuser_screen";
        } else if (cur_screen_h && cur_name == "multiuser_screen") {
          target_name = "loading_screen";
        } else if (cur_screen_h &&
                   (cur_name == "loading_screen" ||
                    cur_name == "preloading_screen" ||
                    cur_name == "real_loading_screen")) {
          // Skip intermediate loading screens — go straight to game_screen,
          // but ONLY once the song FileMerger merge is done (merge_busy==false).
          // Holding here lets the main thread's LoadMgr::Poll finish the merge
          // so HamDirector::Enter sees a fully-built anim. (task #21)
          if (!merge_busy) {
            target_name = "game_screen";
          } else if ((s_skel_calls % 120) == 0) {
            XELOGI("DC3: HOLD at '{}' — song merge still busy, not advancing",
                   cur_name);
          }
        }

        if (!target_name.empty()) {
          uint32_t found_screen = 0;
          uint32_t found_name_ptr = 0;
          for (int scan_pass = 0; scan_pass < 2 && !found_screen; ++scan_pass) {
            bool strict_scan_range = scan_pass == 0;
            if (scan_pass == 1) {
              XELOGI("DC3: Nav scan retrying '{}' without .rdata fence",
                     target_name);
            }
            for (uint32_t s_base = 0x40C00000;
                 s_base < 0x41000000 && !found_screen; s_base += 0x10000) {
              if (!is_guest_readable(s_base, 1)) {
                continue;
              }
              for (uint32_t addr = s_base;
                   addr < s_base + 0x10000 && !found_screen; addr += 4) {
                if (!is_guest_readable(addr + 0x20, 4)) {
                  continue;
                }
                uint32_t candidate_name_ptr =
                    read_name_ptr_at(addr, 0x1C, strict_scan_range);
                std::string candidate =
                    read_guest_name(candidate_name_ptr, strict_scan_range);
                if (candidate.empty()) {
                  candidate_name_ptr =
                      read_name_ptr_at(addr, 0x20, strict_scan_range);
                  candidate =
                      read_guest_name(candidate_name_ptr, strict_scan_range);
                }
                if (candidate == target_name) {
                  found_screen = addr;
                  found_name_ptr = candidate_name_ptr;
                  break;
                }
              }
            }
          }
          if (!found_name_ptr) {
            found_name_ptr = find_name_literal_ptr(target_name);
            if (found_name_ptr) {
              XELOGI("DC3: Nav literal: {} -> {} (name={:08X})", cur_name,
                     target_name, found_name_ptr);
            }
          }
          if (found_screen && found_name_ptr) {
            if (processor && thread_state) {
              if (target_name == "game_screen" &&
                  cvars::dc3_game_screen_real_goto) {
                // Real path: drive the genuine UIManager::GotoScreen so
                // game_panel->Load() runs CreateGame() (new Game()) and the
                // per-frame GamePanel::Poll() -> Game::HandleWait() state machine
                // fires SetupAnims()/OnSongLoaded()/StartGame() -> animating
                // dancer. This previously blocked because the song FileMerger
                // merge wasn't done; we only reach here once merge_busy==false
                // (see the HOLD above), so GotoScreen can complete. (task #21)
                constexpr uint32_t kUIManagerGotoScreenByName = 0x8277B378;
                XELOGI("DC3: Nav goto (real, game_screen): {} -> {} "
                       "({:08X}, name={:08X})",
                       cur_name, target_name, found_screen, found_name_ptr);
                uint64_t args[4] = {ui_addr, found_name_ptr, 0, 0};
                processor->Execute(thread_state, kUIManagerGotoScreenByName,
                                   args, 4);
              } else if (target_name == "game_screen") {
                // Fallback (dc3_game_screen_real_goto=false): host force-set the
                // UIManager screen pointers directly. Reaches game_screen visually
                // but never creates the Game (no Load()/CreateGame()), so no
                // animating dancer. Kept for A/B comparison.
                xe::store_and_swap<uint32_t>(ui_obj + 0x48, found_screen);
                xe::store_and_swap<uint32_t>(ui_obj + 0x4C, 0);
                xe::store_and_swap<uint32_t>(ui_obj + 0x2C, 0);
                XELOGI("DC3: Nav force-set: {} -> {} ({:08X}, name={:08X})",
                       cur_name, target_name, found_screen, found_name_ptr);
                cur_screen_h = found_screen;
                trans_state_h = 0;
                trans_screen_h = 0;
                cur_name = target_name;
                raw_name = target_name;
                trans_name.clear();
                raw_trans_name.clear();
              } else {
                constexpr uint32_t kUIManagerGotoScreenByName = 0x8277B378;
                XELOGI("DC3: Nav goto: {} -> {} ({:08X}, name={:08X})",
                       cur_name, target_name, found_screen, found_name_ptr);
                uint64_t args[4] = {ui_addr, found_name_ptr, 0, 0};
                processor->Execute(thread_state, kUIManagerGotoScreenByName,
                                   args, 4);
              }
            } else {
              XELOGW(
                  "DC3: Nav goto skipped for '{}' (processor/thread_state missing)",
                  target_name);
            }
            s_screen_stable_count = 0;
          } else if (found_name_ptr) {
            if (processor && thread_state) {
              if (target_name == "game_screen" &&
                  cvars::dc3_game_screen_real_goto) {
                constexpr uint32_t kUIManagerGotoScreenByName = 0x8277B378;
                XELOGI("DC3: Nav goto by literal (real, game_screen): {} -> {} "
                       "(name={:08X})",
                       cur_name, target_name, found_name_ptr);
                uint64_t args[4] = {ui_addr, found_name_ptr, 0, 0};
                processor->Execute(thread_state, kUIManagerGotoScreenByName,
                                   args, 4);
              } else if (target_name == "game_screen") {
                XELOGI("DC3: Nav force-set by literal: {} -> {} (name={:08X})",
                       cur_name, target_name, found_name_ptr);
                // Can't force-set without found_screen, fall through
              } else {
                constexpr uint32_t kUIManagerGotoScreenByName = 0x8277B378;
                XELOGI("DC3: Nav goto by literal: {} -> {} (name={:08X})",
                       cur_name, target_name, found_name_ptr);
                uint64_t args[4] = {ui_addr, found_name_ptr, 0, 0};
                processor->Execute(thread_state, kUIManagerGotoScreenByName,
                                   args, 4);
              }
              s_screen_stable_count = 0;
            } else {
              XELOGW(
                  "DC3: Nav goto-by-literal skipped for '{}' (processor/thread_state missing)",
                  target_name);
            }
          } else if (found_screen) {
            XELOGW("DC3: Nav target '{}' found at {:08X} without usable name ptr",
                   target_name, found_screen);
          } else {
            XELOGW("DC3: Nav target '{}' unresolved from '{}'", target_name,
                   cur_name);
          }
        }
      }

      if (!s_loadsong_probe_logged && cur_name == "loading_screen") {
        auto* processor = kernel_state->processor();
        auto* thread_state = ppc_context->thread_state;
        if (processor && thread_state) {
          constexpr uint32_t kTheGameData = 0x82F60034;
          constexpr uint32_t kTheContentMgr = 0x82F123BC;
          constexpr uint32_t kTheHamProvider = 0x82F601B4;
          constexpr uint32_t kTheHamSongMgr = 0x83118C6C;
          constexpr uint32_t kTheMoveMgr = 0x82F60308;
          constexpr uint32_t kTheGameMode = 0x83117710;
          constexpr uint32_t kMetaPerformerCurrent = 0x828CB8A8;
          constexpr uint32_t kDataReadFile = 0x825C1AD0;
          constexpr uint32_t kHamSongMgrAddSongs = 0x828C5BC0;
          constexpr uint32_t kHamSongMgrData = 0x828C5AD8;
          constexpr uint32_t kHamSongMgrSongAudioData = 0x828C62D0;
          constexpr uint32_t kHamSongMgrGetShortNameFromSongID = 0x828C7CE8;
          constexpr uint32_t kHamSongMgrGetSongIDFromShortName = 0x828C7DE0;
          constexpr uint32_t kHamGameDataSetAssociatedPadNum = 0x82452268;
          constexpr uint32_t kHamGameDataPlayer = 0x82451CB8;
          constexpr uint32_t kSymbolCtor = 0x827D37C8;

          auto load_u32 = [&](uint32_t guest_addr) -> uint32_t {
            auto* ptr = is_guest_readable(guest_addr, 4)
                            ? memory->TranslateVirtual<uint8_t*>(guest_addr)
                            : nullptr;
            return ptr ? xe::load_and_swap<uint32_t>(ptr) : 0;
          };
          auto alloc_guest_cstr = [&](const char* text) -> uint32_t {
            if (!text) {
              return 0;
            }
            size_t len = std::strlen(text) + 1;
            uint32_t guest_addr =
                memory->SystemHeapAlloc(static_cast<uint32_t>(len), 4);
            if (!guest_addr) {
              return 0;
            }
            auto* dst = memory->TranslateVirtual<uint8_t*>(guest_addr);
            if (!dst) {
              memory->SystemHeapFree(guest_addr);
              return 0;
            }
            std::memcpy(dst, text, len);
            return guest_addr;
          };
          auto try_direct_song_catalog_load = [&]() -> bool {
            struct PathCandidate {
              const char* path;
              const char* label;
            };
            constexpr PathCandidate kCandidates[] = {
                {"d:\\songs\\songs.dta", "disc_root"},
                {"devkit:\\songs\\gen\\songs.dtb", "devkit_dtb"},
                {"devkit:\\songs\\songs.dta", "devkit_dta"},
            };
            for (const auto& candidate : kCandidates) {
              XELOGI(
                  "DC3: LoadSong repair: probing song catalog '{}' [{}]",
                  candidate.path, candidate.label);
              uint32_t path_addr = alloc_guest_cstr(candidate.path);
              if (!path_addr) {
                XELOGW("DC3: LoadSong repair: failed to allocate guest path "
                       "for {}", candidate.label);
                continue;
              }
              uint64_t read_args[2] = {path_addr, 1};
              uint32_t data_arr = static_cast<uint32_t>(
                  processor->Execute(thread_state, kDataReadFile, read_args, 2));
              XELOGI("DC3: LoadSong repair: DataReadFile('{}') [{}] -> {:08X}",
                     candidate.path, candidate.label, data_arr);
              memory->SystemHeapFree(path_addr);
              if (!data_arr) {
                continue;
              }
              uint64_t add_args[2] = {kTheHamSongMgr, data_arr};
              processor->Execute(thread_state, kHamSongMgrAddSongs, add_args, 2);
              XELOGI("DC3: LoadSong repair: HamSongMgr::AddSongs({:08X}) "
                     "completed via {}",
                     data_arr, candidate.label);
              return true;
            }
            return false;
          };

          uint32_t gd_addr = load_u32(kTheGameData);
          uint32_t content_mgr_addr = load_u32(kTheContentMgr);
          uint32_t hp_addr = load_u32(kTheHamProvider);
          uint32_t mm_addr = load_u32(kTheMoveMgr);
          uint32_t gm_addr = load_u32(kTheGameMode);

          uint64_t meta_ret =
              processor->Execute(thread_state, kMetaPerformerCurrent, nullptr, 0);
          uint32_t meta_addr = static_cast<uint32_t>(meta_ret);

          uint32_t p0_addr = 0;
          uint32_t p1_addr = 0;
          if (gd_addr && gd_addr < 0xF0000000) {
            uint64_t p0_args[2] = {gd_addr, 0};
            uint64_t p1_args[2] = {gd_addr, 1};
            p0_addr = static_cast<uint32_t>(
                processor->Execute(thread_state, kHamGameDataPlayer, p0_args, 2));
            p1_addr = static_cast<uint32_t>(
                processor->Execute(thread_state, kHamGameDataPlayer, p1_args, 2));
          }

          uint32_t song_sym =
              (gd_addr && is_guest_readable(gd_addr + 0x30, 4))
                  ? load_u32(gd_addr + 0x30)
                  : 0;
          std::string song_name = read_guest_name(song_sym, false);
          if ((song_name.empty()) && !s_loadsong_repair_attempted && gd_addr) {
            s_loadsong_repair_attempted = true;
            XELOGI(
                "DC3: LoadSong repair: trying direct song catalog load "
                "(gd={:08X} content={:08X} ham_song_mgr={:08X})",
                gd_addr, content_mgr_addr, kTheHamSongMgr);
            bool direct_song_load_ok = try_direct_song_catalog_load();
            if (!direct_song_load_ok && !s_content_refresh_forced) {
              s_content_refresh_forced = true;
              // NOTE: ContentMgr::RefreshSynchronously blocks forever under
              // Xenia because content enumeration never completes. Skip it
              // and let the nav bridge force-advance through the loading
              // screens instead.
              XELOGW("DC3: LoadSong repair: direct song catalog load failed; "
                     "skipping ContentMgr::RefreshSynchronously (blocks forever)");
            }
            constexpr uint32_t kYmcaSongId = 7011;
            uint64_t short_name_args[4] = {gd_addr + 0x30, kTheHamSongMgr,
                                           kYmcaSongId, 0};
            processor->Execute(
                thread_state, kHamSongMgrGetShortNameFromSongID,
                short_name_args, 4);
            if (is_guest_readable(gd_addr + 0x30, 4)) {
              auto* song_slot = memory->TranslateVirtual<uint8_t*>(gd_addr + 0x30);
              song_sym = xe::load_and_swap<uint32_t>(song_slot);
              song_name = read_guest_name(song_sym, false);
              XELOGI(
                  "DC3: LoadSong repair: canonical song id {} -> {:08X} '{}'",
                  kYmcaSongId, song_sym, song_name);
            }

            uint32_t song_name_ptr = 0;
            if (song_name.empty()) {
              song_name_ptr = find_name_literal_ptr("ymca");
            }
            if (song_name.empty()) {
              if (song_name_ptr) {
                XELOGI(
                    "DC3: LoadSong repair: constructing song symbol from {:08X}",
                    song_name_ptr);
                uint64_t ctor_args[2] = {gd_addr + 0x30, song_name_ptr};
                processor->Execute(thread_state, kSymbolCtor, ctor_args, 2);
                song_sym = load_u32(gd_addr + 0x30);
                song_name = read_guest_name(song_sym, false);
              } else {
                XELOGW("DC3: LoadSong repair: guest literal 'ymca' not found");
              }
            }
            if (!song_name.empty()) {
              XELOGI(
                  "DC3: LoadSong repair: injected song={:08X} '{}'", song_sym,
                  song_name);
              uint64_t pad0_args[3] = {gd_addr, 0, 0};
              uint64_t pad1_args[3] = {gd_addr, 1, 1};
              processor->Execute(thread_state, kHamGameDataSetAssociatedPadNum,
                                 pad0_args, 3);
              processor->Execute(thread_state, kHamGameDataSetAssociatedPadNum,
                                 pad1_args, 3);

              if (gd_addr && gd_addr < 0xF0000000) {
                uint64_t p0_args[2] = {gd_addr, 0};
                uint64_t p1_args[2] = {gd_addr, 1};
                p0_addr = static_cast<uint32_t>(processor->Execute(
                    thread_state, kHamGameDataPlayer, p0_args, 2));
                p1_addr = static_cast<uint32_t>(processor->Execute(
                    thread_state, kHamGameDataPlayer, p1_args, 2));
              }
            }
          }
          uint32_t p0_char =
              (p0_addr && is_guest_readable(p0_addr + 0x44, 4))
                  ? load_u32(p0_addr + 0x44)
                  : 0;
          uint32_t p1_char =
              (p1_addr && is_guest_readable(p1_addr + 0x44, 4))
                  ? load_u32(p1_addr + 0x44)
                  : 0;
          uint32_t p0_diff =
              (p0_addr && is_guest_readable(p0_addr + 0x58, 4))
                  ? load_u32(p0_addr + 0x58)
                  : 0;
          uint32_t p1_diff =
              (p1_addr && is_guest_readable(p1_addr + 0x58, 4))
                  ? load_u32(p1_addr + 0x58)
                  : 0;
          uint32_t p0_pad =
              (p0_addr && is_guest_readable(p0_addr + 0x7C, 4))
                  ? load_u32(p0_addr + 0x7C)
                  : 0;
          uint32_t p1_pad =
              (p1_addr && is_guest_readable(p1_addr + 0x7C, 4))
                  ? load_u32(p1_addr + 0x7C)
                  : 0;
          uint32_t song_id = 0;
          uint32_t song_data = 0;
          uint32_t song_audio = 0;
          uint32_t default_outfit = 0;
          uint32_t default_venue = 0;
          if (!song_name.empty()) {
            uint64_t song_id_args[3] = {kTheHamSongMgr, song_sym, 0};
            song_id = static_cast<uint32_t>(processor->Execute(
                thread_state, kHamSongMgrGetSongIDFromShortName, song_id_args,
                3));
            if (song_id) {
              uint64_t data_args[2] = {kTheHamSongMgr, song_id};
              song_data = static_cast<uint32_t>(processor->Execute(
                  thread_state, kHamSongMgrData, data_args, 2));
              song_audio = static_cast<uint32_t>(processor->Execute(
                  thread_state, kHamSongMgrSongAudioData, data_args, 2));
              if (song_data && is_guest_readable(song_data + 0xC0, 4)) {
                default_outfit = load_u32(song_data + 0xC0);
              }
              if (song_data && is_guest_readable(song_data + 0xD0, 4)) {
                default_venue = load_u32(song_data + 0xD0);
              }
            }
          }

          XELOGI(
              "DC3: LoadSong probe gd={:08X} gm={:08X} hp={:08X} mm={:08X} mp={:08X} "
              "cm={:08X} "
              "p0={:08X} char={:08X} '{}' diff={} pad={} "
              "p1={:08X} char={:08X} '{}' diff={} pad={} "
              "song={:08X} '{}' id={} data={:08X} audio={:08X} "
              "default_outfit={:08X} '{}' venue={:08X} '{}'",
              gd_addr, gm_addr, hp_addr, mm_addr, meta_addr, content_mgr_addr,
              p0_addr, p0_char, read_guest_name(p0_char, false), p0_diff, p0_pad,
              p1_addr, p1_char, read_guest_name(p1_char, false), p1_diff, p1_pad,
              song_sym, song_name, song_id, song_data, song_audio,
              default_outfit, read_guest_name(default_outfit, false), default_venue,
              read_guest_name(default_venue, false));
          s_loadsong_probe_logged = true;
        }
      }

      // Gameplay bootstrap is disabled — GamePanel::CreateGame blocks
      // because it tries to load song/character resources via async I/O
      // that depend on ARK file content not fully accessible in Xenia.
      // IK telemetry capture requires the full gameplay pipeline running,
      // which in turn requires working ARK loading for all game assets.

      if (cur_name == "game_screen") {
        // IK telemetry: the HolmesClientPoll override never fires (see
        // dc3_hack_pack.cc), but this NUI hook runs every frame.  Read the
        // always-fresh IK scratch slots + bone walk here, gated to ~2/sec so
        // the log stays readable.  ReadDc3IKTelemetry guards every guest read
        // and no-ops unless --dc3_ik_telemetry is set.
        if (cvars::dc3_ik_telemetry && (s_skel_calls % 30) == 0) {
          ReadDc3IKTelemetry(memory, static_cast<uint32_t>(s_skel_calls));
        }
        constexpr uint32_t kTheTaskMgr = 0x82F64A58;
        auto load_u32 = [&](uint32_t guest_addr) -> uint32_t {
          auto* ptr = is_guest_readable(guest_addr, 4)
                          ? memory->TranslateVirtual<uint8_t*>(guest_addr)
                          : nullptr;
          return ptr ? xe::load_and_swap<uint32_t>(ptr) : 0;
        };
        auto store_float = [&](uint32_t guest_addr, float value) {
          if (!is_guest_readable(guest_addr, 4)) {
            return false;
          }
          auto* ptr = memory->TranslateVirtual<uint8_t*>(guest_addr);
          if (!ptr) {
            return false;
          }
          xe::store_and_swap<float>(ptr, value);
          return true;
        };
        // Blocker 2 (gameplay crash): do not drive the song clock until the
        // Game has actually started playback. Game::PostWaitStart clears
        // mPaused (Game+0x5E) once its load/wait state machine completes;
        // driving earlier runs the gameplay pipeline over a not-ready audio
        // stream -> host SIGSEGV (the HamAudio resync / Voice path).
        // TheGamePanel(0x83117410)->mGame(+0x38)->mPaused(+0x5E).
        constexpr uint32_t kTheGamePanelGate = 0x83117410;
        uint32_t gp_gate = load_u32(kTheGamePanelGate);
        uint32_t game_gate =
            (gp_gate && is_guest_readable(gp_gate + 0x38, 4))
                ? load_u32(gp_gate + 0x38)
                : 0;
        bool beat_gate_ok = false;
        if (game_gate && is_guest_readable(game_gate + 0x5E, 1)) {
          auto* pp = memory->TranslateVirtual<uint8_t*>(game_gate + 0x5E);
          beat_gate_ok = pp ? (*pp == 0) : false;  // mPaused==0 -> playing
        }
        if (!beat_gate_ok && s_host_beat_drive_active) {
          XELOGI(
              "DC3: Beat gate closed (Game paused/not ready) -> suspend beat "
              "drive");
          s_host_beat_drive_active = false;
        }
        uint32_t timelines_addr = load_u32(kTheTaskMgr + 0x2C);
        if (beat_gate_ok && timelines_addr &&
            is_guest_readable(timelines_addr + 0x54, 4) &&
            is_guest_readable(kTheTaskMgr + 0x48, 1)) {
          auto* auto_ptr =
              memory->TranslateVirtual<uint8_t*>(kTheTaskMgr + 0x48);
          if (auto_ptr) {
            *auto_ptr = 0;
          }

          constexpr float kSecondsPerFrame = 1.0f / 30.0f;
          constexpr float kBeatPerFrame = 120.0f / 60.0f * kSecondsPerFrame;
          constexpr uint32_t kTimelineStride = 0x1C;
          constexpr uint32_t kTimeOff = 0x10;
          constexpr uint32_t kLastTimeOff = 0x14;

          uint32_t seconds_time_addr = timelines_addr + 0 * kTimelineStride + kTimeOff;
          uint32_t seconds_last_addr =
              timelines_addr + 0 * kTimelineStride + kLastTimeOff;
          uint32_t beats_time_addr = timelines_addr + 1 * kTimelineStride + kTimeOff;
          uint32_t beats_last_addr =
              timelines_addr + 1 * kTimelineStride + kLastTimeOff;
          uint32_t ui_time_addr = timelines_addr + 2 * kTimelineStride + kTimeOff;
          uint32_t ui_last_addr = timelines_addr + 2 * kTimelineStride + kLastTimeOff;

          auto load_float = [&](uint32_t guest_addr) -> float {
            auto* ptr = is_guest_readable(guest_addr, 4)
                            ? memory->TranslateVirtual<uint8_t*>(guest_addr)
                            : nullptr;
            return ptr ? xe::load_and_swap<float>(ptr) : 0.0f;
          };

          float old_seconds = load_float(seconds_time_addr);
          float old_beats = load_float(beats_time_addr);
          float old_ui = load_float(ui_time_addr);
          if (!s_host_beat_drive_active) {
            s_host_song_seconds = old_seconds;
            s_host_song_beat = old_beats;
            XELOGI(
                "DC3: Host-driven beat activated taskmgr={:08X} timelines={:08X} "
                "sec={:.3f} beat={:.3f}",
                kTheTaskMgr, timelines_addr, s_host_song_seconds,
                s_host_song_beat);
            s_host_beat_drive_active = true;
          }

          s_host_song_seconds += kSecondsPerFrame;
          s_host_song_beat += kBeatPerFrame;

          store_float(seconds_last_addr, old_seconds);
          store_float(seconds_time_addr, s_host_song_seconds);
          store_float(beats_last_addr, old_beats);
          store_float(beats_time_addr, s_host_song_beat);
          store_float(ui_last_addr, old_ui);
          store_float(ui_time_addr, s_host_song_seconds);

          if ((s_skel_calls % 120) == 0) {
            XELOGI("DC3: Beat drive sec={:.3f} beat={:.3f} nui={}",
                   s_host_song_seconds, s_host_song_beat, s_skel_calls);
          }
        }
      } else if (s_host_beat_drive_active) {
        XELOGI("DC3: Host-driven beat deactivated on '{}'", cur_name);
        s_host_beat_drive_active = false;
      }
    }
  }
}


}  // namespace

Emulator::GameConfigLoadCallback::GameConfigLoadCallback(Emulator& emulator)
    : emulator_(emulator) {
  emulator_.AddGameConfigLoadCallback(this);
}

Emulator::GameConfigLoadCallback::~GameConfigLoadCallback() {
  emulator_.RemoveGameConfigLoadCallback(this);
}

Emulator::Emulator(const std::filesystem::path& command_line,
                   const std::filesystem::path& storage_root,
                   const std::filesystem::path& content_root,
                   const std::filesystem::path& cache_root)
    : on_launch(),
      on_terminate(),
      on_exit(),
      command_line_(command_line),
      storage_root_(storage_root),
      content_root_(content_root),
      cache_root_(cache_root),
      title_name_(),
      title_version_(),
      display_window_(nullptr),
      memory_(),
      audio_system_(),
      graphics_system_(),
      input_system_(),
      export_resolver_(),
      file_system_(),
      kernel_state_(),
      main_thread_(),
      title_id_(std::nullopt),
      paused_(false),
      restoring_(false),
      restore_fence_() {}

Emulator::~Emulator() {
  // Note that we delete things in the reverse order they were initialized.

  // The probe samplers capture memory_.get(); join them before anything is
  // torn down (fork-cleanup-review.md C10). No-op if TerminateTitle already
  // joined or no probes were armed.
  Rb3dxJoinProbeThreads();

  // Give the systems time to shutdown before we delete them.
  if (graphics_system_) {
    graphics_system_->Shutdown();
  }
  if (audio_system_) {
    audio_system_->Shutdown();
  }

  input_system_.reset();
  graphics_system_.reset();
  audio_system_.reset();

  kernel_state_.reset();
  file_system_.reset();

  processor_.reset();

  export_resolver_.reset();

  ExceptionHandler::Uninstall(Emulator::ExceptionCallbackThunk, this);
}

X_STATUS Emulator::Setup(
    ui::Window* display_window, ui::ImGuiDrawer* imgui_drawer,
    bool require_cpu_backend,
    std::function<std::unique_ptr<apu::AudioSystem>(cpu::Processor*)>
        audio_system_factory,
    std::function<std::unique_ptr<gpu::GraphicsSystem>()>
        graphics_system_factory,
    std::function<std::vector<std::unique_ptr<hid::InputDriver>>(ui::Window*)>
        input_driver_factory) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  display_window_ = display_window;
  imgui_drawer_ = imgui_drawer;

  // Initialize clock.
  // 360 uses a 50MHz clock.
  Clock::set_guest_tick_frequency(50000000);
  // We could reset this with save state data/constant value to help replays.
  Clock::set_guest_system_time_base(Clock::QueryHostSystemTime());
  // This can be adjusted dynamically, as well.
  Clock::set_guest_time_scalar(cvars::time_scalar);

  // Before we can set thread affinity we must enable the process to use all
  // logical processors.
  xe::threading::EnableAffinityConfiguration();

  // Create memory system first, as it is required for other systems.
  memory_ = std::make_unique<Memory>();
  if (!memory_->Initialize()) {
    return false;
  }

  // Shared export resolver used to attach and query for HLE exports.
  export_resolver_ = std::make_unique<xe::cpu::ExportResolver>();

  std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
  if (cvars::cpu == "x64") {
    backend.reset(new xe::cpu::backend::x64::X64Backend());
  }
#endif  // XE_ARCH
  if (cvars::cpu == "any") {
    if (!backend) {
#if XE_ARCH_AMD64
      backend.reset(new xe::cpu::backend::x64::X64Backend());
#endif  // XE_ARCH
    }
  }
  if (!backend && !require_cpu_backend) {
    backend.reset(new xe::cpu::backend::NullBackend());
  }

  // Initialize the CPU.
  processor_ = std::make_unique<xe::cpu::Processor>(memory_.get(),
                                                    export_resolver_.get());
  if (!processor_->Setup(std::move(backend))) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Initialize the APU.
  if (audio_system_factory) {
    audio_system_ = audio_system_factory(processor_.get());
    if (!audio_system_) {
      return X_STATUS_NOT_IMPLEMENTED;
    }
  }

  // Initialize the GPU.
  graphics_system_ = graphics_system_factory();
  if (!graphics_system_) {
    return X_STATUS_NOT_IMPLEMENTED;
  }

  // Initialize the HID.
  input_system_ = std::make_unique<xe::hid::InputSystem>(display_window_);
  if (!input_system_) {
    return X_STATUS_NOT_IMPLEMENTED;
  }
  if (input_driver_factory) {
    auto input_drivers = input_driver_factory(display_window_);
    for (size_t i = 0; i < input_drivers.size(); ++i) {
      auto& input_driver = input_drivers[i];
      input_driver->set_is_active_callback(
          []() -> bool { return !xe::kernel::xam::xeXamIsUIActive(); });
      input_system_->AddDriver(std::move(input_driver));
    }
  }

  result = input_system_->Setup();
  if (result) {
    return result;
  }

  // Bring up the virtual filesystem used by the kernel.
  file_system_ = std::make_unique<xe::vfs::VirtualFileSystem>();

  // Shared kernel state.
  kernel_state_ = std::make_unique<xe::kernel::KernelState>(this);

  // Setup the core components.
  result = graphics_system_->Setup(
      processor_.get(), kernel_state_.get(),
      display_window_ ? &display_window_->app_context() : nullptr,
      display_window_ != nullptr);
  if (result) {
    return result;
  }

  if (audio_system_) {
    result = audio_system_->Setup(kernel_state_.get());
    if (result) {
      return result;
    }
  }

#define LOAD_KERNEL_MODULE(t) \
  static_cast<void>(kernel_state_->LoadKernelModule<kernel::t>())
  // HLE kernel modules.
  LOAD_KERNEL_MODULE(xboxkrnl::XboxkrnlModule);
  LOAD_KERNEL_MODULE(xam::XamModule);
  LOAD_KERNEL_MODULE(xbdm::XbdmModule);
#undef LOAD_KERNEL_MODULE

  // Initialize emulator fallback exception handling last.
  ExceptionHandler::Install(Emulator::ExceptionCallbackThunk, this);

  return result;
}

X_STATUS Emulator::TerminateTitle() {
  if (!is_title_open()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Stop and join the RB3DX/DC3 probe sampler threads before the title (and
  // later memory_) goes away under them (fork-cleanup-review.md C10).
  Rb3dxJoinProbeThreads();

  if (processor_) {
    processor_->ClearGuestFunctionOverrides();
  }
  Dc3RuntimeTelemetryEndSession("terminate_title");
  cpu::MiloTraceEnd("terminate_title");
  kernel_state_->TerminateTitle();
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  on_terminate();
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::LaunchPath(const std::filesystem::path& path) {
  Dc3RuntimeTelemetryEndSession("launch_path_reset");
  cpu::MiloTraceEnd("launch_path_reset");
  if (processor_) {
    processor_->ClearGuestFunctionOverrides();
  }
  // Launch based on file type.
  // This is a silly guess based on file extension.
  if (!path.has_extension()) {
    // Likely an STFS container.
    return LaunchStfsContainer(path);
  };
  auto extension = xe::utf8::lower_ascii(xe::path_to_utf8(path.extension()));
  if (extension == ".xex" || extension == ".elf" || extension == ".exe") {
    // Treat as a naked xex file.
    return LaunchXexFile(path);
  } else {
    // Assume a disc image.
    return LaunchDiscImage(path);
  }
}

X_STATUS Emulator::LaunchXexFile(const std::filesystem::path& path) {
  // We create a virtual filesystem pointing to its directory and symlink
  // that to the game filesystem.
  // e.g., /my/files/foo.xex will get a local fs at:
  // \\Device\\Harddisk0\\Partition1
  // and then get that symlinked to game:\, so
  // -> game:\foo.xex

  auto mount_path = "\\Device\\Harddisk0\\Partition1";

  // Register the local directory in the virtual filesystem.
  auto parent_path = path.parent_path();
  auto device =
      std::make_unique<vfs::HostPathDevice>(mount_path, parent_path, true);
  if (!device->Initialize()) {
    XELOGE("Unable to scan host path");
    return X_STATUS_NO_SUCH_FILE;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    XELOGE("Unable to register host path");
    return X_STATUS_NO_SUCH_FILE;
  }

  // Create symlinks to the device.
  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);
  // RB3 boot-to-menu experiment (cvar default OFF -> inert for DC3 and normal
  // runs): optionally mount update: at the disc dir so RB3 finds
  // update:\gen\patch_xbox.hdr instead of failing the open. Tests whether the
  // post-load clean shutdown on clean-TU5 _nodd is the title-update gate.
  if (cvars::rb3_mount_update) {
    file_system_->RegisterSymbolicLink("update:", mount_path);
    XELOGI("rb3_mount_update: update: -> {} (patch_xbox.* now visible)",
           mount_path);
  }
  // NOTE: intentionally do NOT symlink "update:" to the game mount. RB3 probes
  // update:\gen\patch_xbox.hdr for title-update content; if that content is
  // absent the game falls back to the base arks and boots further (retail
  // reaches the ESRB screen). Pointing update: at the disc dir exposes the
  // bundled patch_xbox.* which, in this content set, is a mismatched/placeholder
  // header ("LOLZ" magic, not the base's encrypted form) that the game rejects
  // with a "dirty disc" bail-out on BOTH retail and clean TU5. A real update:
  // mount belongs to a separate, matching title-update package, not the disc.

  // Get just the filename (foo.xex).
  auto file_name = path.filename();

  // Launch the game.
  auto fs_path = "game:\\" + xe::path_to_utf8(file_name);
  return CompleteLaunch(path, fs_path);
}

X_STATUS Emulator::LaunchDiscImage(const std::filesystem::path& path) {
  auto mount_path = "\\Device\\Cdrom0";

  // Register the disc image in the virtual filesystem.
  auto device = std::make_unique<vfs::DiscImageDevice>(mount_path, path);
  if (!device->Initialize()) {
    xe::FatalError("Unable to mount disc image; file not found or corrupt.");
    return X_STATUS_NO_SUCH_FILE;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    xe::FatalError("Unable to register disc image.");
    return X_STATUS_NO_SUCH_FILE;
  }

  // Create symlinks to the device.
  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);

  // Launch the game.
  auto module_path(FindLaunchModule());
  return CompleteLaunch(path, module_path);
}

X_STATUS Emulator::LaunchStfsContainer(const std::filesystem::path& path) {
  auto mount_path = "\\Device\\Cdrom0";

  // Register the container in the virtual filesystem.
  auto device = std::make_unique<vfs::StfsContainerDevice>(mount_path, path);
  if (!device->Initialize()) {
    xe::FatalError(
        "Unable to mount STFS container; file not found or corrupt.");
    return X_STATUS_NO_SUCH_FILE;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    xe::FatalError("Unable to register STFS container.");
    return X_STATUS_NO_SUCH_FILE;
  }

  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);

  // Launch the game.
  auto module_path(FindLaunchModule());
  return CompleteLaunch(path, module_path);
}

void Emulator::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Don't hold the lock on this (so any waits follow through)
  graphics_system_->Pause();
  audio_system_->Pause();

  auto lock = global_critical_region::AcquireDirect();
  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  auto current_thread = kernel::XThread::IsInThread()
                            ? kernel::XThread::GetCurrentThread()
                            : nullptr;
  for (auto thread : threads) {
    // Don't pause ourself or host threads.
    if (thread == current_thread || !thread->can_debugger_suspend()) {
      continue;
    }

    if (thread->is_running()) {
      thread->thread()->Suspend(nullptr);
    }
  }

  XELOGD("! EMULATOR PAUSED !");
}

void Emulator::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;
  XELOGD("! EMULATOR RESUMED !");

  graphics_system_->Resume();
  audio_system_->Resume();

  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  for (auto thread : threads) {
    if (!thread->can_debugger_suspend()) {
      // Don't pause host threads.
      continue;
    }

    if (thread->is_running()) {
      thread->thread()->Resume(nullptr);
    }
  }
}

bool Emulator::SaveToFile(const std::filesystem::path& path) {
  Pause();

  filesystem::CreateEmptyFile(path);
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite, 0, 2_GiB);
  if (!map) {
    return false;
  }

  // Save the emulator state to a file
  ByteStream stream(map->data(), map->size());
  stream.Write(kEmulatorSaveSignature);
  stream.Write(title_id_.has_value());
  if (title_id_.has_value()) {
    stream.Write(title_id_.value());
  }

  // It's important we don't hold the global lock here! XThreads need to step
  // forward (possibly through guarded regions) without worry!
  processor_->Save(&stream);
  graphics_system_->Save(&stream);
  audio_system_->Save(&stream);
  kernel_state_->Save(&stream);
  memory_->Save(&stream);
  map->Close(stream.offset());

  Resume();
  return true;
}

bool Emulator::RestoreFromFile(const std::filesystem::path& path) {
  // Restore the emulator state from a file
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite);
  if (!map) {
    return false;
  }

  restoring_ = true;

  // Terminate any loaded titles.
  Pause();
  kernel_state_->TerminateTitle();

  auto lock = global_critical_region::AcquireDirect();
  ByteStream stream(map->data(), map->size());
  if (stream.Read<uint32_t>() != kEmulatorSaveSignature) {
    return false;
  }

  auto has_title_id = stream.Read<bool>();
  std::optional<uint32_t> title_id;
  if (!has_title_id) {
    title_id = {};
  } else {
    title_id = stream.Read<uint32_t>();
  }
  if (title_id_.has_value() != title_id.has_value() ||
      title_id_.value() != title_id.value()) {
    // Swapping between titles is unsupported at the moment.
    assert_always();
    return false;
  }

  if (!processor_->Restore(&stream)) {
    XELOGE("Could not restore processor!");
    return false;
  }
  if (!graphics_system_->Restore(&stream)) {
    XELOGE("Could not restore graphics system!");
    return false;
  }
  if (!audio_system_->Restore(&stream)) {
    XELOGE("Could not restore audio system!");
    return false;
  }
  if (!kernel_state_->Restore(&stream)) {
    XELOGE("Could not restore kernel state!");
    return false;
  }
  if (!memory_->Restore(&stream)) {
    XELOGE("Could not restore memory!");
    return false;
  }

  // Update the main thread.
  auto threads =
      kernel_state_->object_table()->GetObjectsByType<kernel::XThread>();
  for (auto thread : threads) {
    if (thread->main_thread()) {
      main_thread_ = thread;
      break;
    }
  }

  Resume();

  restore_fence_.Signal();
  restoring_ = false;

  return true;
}

bool Emulator::TitleRequested() {
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");
  return xam->loader_data().launch_data_present;
}

void Emulator::LaunchNextTitle() {
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");
  auto next_title = xam->loader_data().launch_path;

  CompleteLaunch("", next_title);
}

bool Emulator::ExceptionCallbackThunk(Exception* ex, void* data) {
  return reinterpret_cast<Emulator*>(data)->ExceptionCallback(ex);
}

bool Emulator::ExceptionCallback(Exception* ex) {
  // Check to see if the exception occurred in guest code.
  auto code_cache = processor()->backend()->code_cache();
  auto code_base = code_cache->execute_base_address();
  auto code_end = code_base + code_cache->total_size();

  if (!processor()->is_debugger_attached() && debugging::IsDebuggerAttached()) {
    // If Xenia's debugger isn't attached but another one is, pass it to that
    // debugger.
    return false;
  } else if (processor()->is_debugger_attached()) {
    // Let the debugger handle this exception. It may decide to continue past it
    // (if it was a stepping breakpoint, etc).
    return processor()->OnUnhandledException(ex);
  }

  if (!(ex->pc() >= code_base && ex->pc() < code_end)) {
    // Didn't occur in guest code. Let it pass.
    return false;
  }

  // Within range. Pause the emulator and eat the exception.
  Pause();

  // Dump information into the log.
  auto current_thread = kernel::XThread::GetCurrentThread();
  assert_not_null(current_thread);

  auto guest_function = code_cache->LookupFunction(ex->pc());

  auto context = current_thread->thread_state()->context();

  XELOGE("==== CRASH DUMP ====");
  XELOGE("Thread ID (Host: 0x{:08X} / Guest: 0x{:08X})",
         current_thread->thread()->system_id(), current_thread->thread_id());
  XELOGE("Thread Handle: 0x{:08X}", current_thread->handle());
  if (guest_function) {
    XELOGE("PC: 0x{:08X}",
           guest_function->MapMachineCodeToGuestAddress(ex->pc()));
  } else {
    XELOGE("PC: <unknown guest function, host PC=0x{:016X}>", ex->pc());
  }
  XELOGE("Guest lr: 0x{:08X}", static_cast<uint32_t>(context->lr));
  XELOGE("Guest ctr: 0x{:08X}  CR=0x{:08X}  XER[CA/OV/SO]={}/{}/{}",
         static_cast<uint32_t>(context->ctr),
         static_cast<uint32_t>(context->cr()),
         static_cast<uint32_t>(context->xer_ca),
         static_cast<uint32_t>(context->xer_ov),
         static_cast<uint32_t>(context->xer_so));
  XELOGE("Registers:");
  for (int i = 0; i < 32; i++) {
    XELOGE(" r{:<3} = {:016X}", i, context->r[i]);
  }
  for (int i = 0; i < 32; i++) {
    XELOGE(" f{:<3} = {:016X} = (double){} = (float){}", i,
           *reinterpret_cast<uint64_t*>(&context->f[i]), context->f[i],
           *(float*)&context->f[i]);
  }
  for (int i = 0; i < 128; i++) {
    XELOGE(" v{:<3} = [0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}]", i,
           context->v[i].u32[0], context->v[i].u32[1], context->v[i].u32[2],
           context->v[i].u32[3]);
  }

  MaybeWriteDc3CrashSnapshotJson(this, ex, current_thread, guest_function, context);

  // Dump fault address details for access violations.
  if (ex->code() == Exception::Code::kAccessViolation) {
    uint64_t host_fault = ex->fault_address();
    uint64_t membase =
        reinterpret_cast<uintptr_t>(memory_->virtual_membase());
    uint32_t guest_fault = static_cast<uint32_t>(host_fault - membase);
    XELOGE("Fault address: host=0x{:016X} guest=0x{:08X} ({})", host_fault,
           guest_fault,
           ex->access_violation_operation() ==
                   Exception::AccessViolationOperation::kWrite
               ? "WRITE"
               : "READ");
  }

  // Dump guest function info and code around crash PC.
  if (guest_function) {
    uint32_t guest_pc = guest_function->MapMachineCodeToGuestAddress(ex->pc());
    XELOGE("Guest function: {} (0x{:08X})", guest_function->name(),
           guest_function->address());
    if (guest_pc >= 0x82000000 && guest_pc < 0x90000000) {
      uint32_t dump_start = (guest_pc > 0x40) ? (guest_pc - 0x40) : guest_pc;
      uint32_t dump_end = guest_pc + 0x40;
      XELOGE("Guest code near PC 0x{:08X}:", guest_pc);
      for (uint32_t addr = dump_start; addr < dump_end; addr += 4) {
        auto* mem_ptr = memory_->TranslateVirtual<uint8_t*>(addr);
        if (!mem_ptr) break;
        uint32_t instr = xe::load_and_swap<uint32_t>(mem_ptr);
        XELOGE("  0x{:08X}: {:08X}{}", addr, instr,
               addr == guest_pc ? "  <-- CRASH PC" : "");
      }
    }
  }

  // Walk the PPC stack to identify call chain (helps diagnose recursion).
  // fork-cleanup-review C9: this runs INSIDE the exception handler for a
  // guest fault. TranslateVirtual is membase+addr and can NEVER return null
  // (the old `if (!ptr) break` guards were dead), so every read must first
  // prove the page is committed or a decommitted stack page turns a
  // diagnosable guest crash into a nested host SIGSEGV with no output.
  {
    auto stack_word_readable = [&](uint32_t addr) -> bool {
      if (addr < 0x70000000 || addr >= 0x78000000) return false;
      auto* heap = memory_->LookupHeap(addr);
      if (!heap) return false;
      HeapAllocationInfo info = {};
      if (!heap->QueryRegionInfo(addr, &info)) return false;
      return (info.state & kMemoryAllocationCommit) != 0;
    };
    uint32_t sp = static_cast<uint32_t>(context->r[1]);
    XELOGE("==== STACK WALK (SP=0x{:08X}) ====", sp);
    int frame = 0;
    uint32_t last_lr = 0;
    int repeat_count = 0;
    constexpr int kMaxWalkFrames = 512;
    for (; frame < kMaxWalkFrames && sp >= 0x70000000 && sp < 0x78000000;
         frame++) {
      if (!stack_word_readable(sp) || !stack_word_readable(sp + 8)) {
        XELOGE("  [{}] sp=0x{:08X} not committed -- stopping walk", frame, sp);
        break;
      }
      auto* host_ptr = memory_->TranslateVirtual<uint8_t*>(sp);
      uint32_t back_chain = xe::load_and_swap<uint32_t>(host_ptr);
      // Try multiple LR save locations:
      uint32_t lr_sp4 = xe::load_and_swap<uint32_t>(host_ptr + 4);
      uint32_t lr_sp8 = xe::load_and_swap<uint32_t>(host_ptr + 8);
      // Also try __savegprlr convention: LR at back_chain - 8
      uint32_t lr_bc8 = 0;
      if (back_chain >= 0x70000008 && back_chain < 0x78000000 &&
          stack_word_readable(back_chain - 8)) {
        auto* bc_ptr = memory_->TranslateVirtual<uint8_t*>(back_chain - 8);
        lr_bc8 = xe::load_and_swap<uint32_t>(bc_ptr);
      }
      // Pick the most likely LR (first non-BEBEBEBE, non-zero, in code range)
      uint32_t best_lr = 0;
      if (lr_bc8 >= 0x82000000 && lr_bc8 < 0x8A000000) best_lr = lr_bc8;
      else if (lr_sp4 >= 0x82000000 && lr_sp4 < 0x8A000000) best_lr = lr_sp4;
      else if (lr_sp8 >= 0x82000000 && lr_sp8 < 0x8A000000) best_lr = lr_sp8;
      else best_lr = lr_sp4;  // fallback
      uint32_t frame_size = (back_chain > sp) ? (back_chain - sp) : 0;
      // Log with sampling: first 30 frames, then every 100th, last 10
      bool should_log = (frame < 30) || (frame % 100 == 0) ||
                         (best_lr != last_lr && best_lr != 0xBEBEBEBE);
      if (should_log) {
        if (repeat_count > 0) {
          XELOGE("  ... ({} identical frames skipped, lr=0x{:08X})",
                 repeat_count, last_lr);
          repeat_count = 0;
        }
        XELOGE("  [{}] sp=0x{:08X} sz={} lr_sp4=0x{:08X} lr_bc8=0x{:08X}",
               frame, sp, frame_size, lr_sp4, lr_bc8);
      } else {
        repeat_count++;
      }
      last_lr = best_lr;
      if (back_chain == 0 || back_chain == sp || back_chain < 0x70000000 ||
          back_chain >= 0x78000000)
        break;
      sp = back_chain;
    }
    if (repeat_count > 0) {
      XELOGE("  ... ({} identical frames skipped, lr=0x{:08X})",
             repeat_count, last_lr);
    }
    XELOGE("==== END STACK WALK ({} frames) ====", frame);
  }

  // Display a dialog telling the user the guest has crashed.
#ifndef XE_HEADLESS_BUILD
  if (display_window_ && imgui_drawer_) {
    display_window_->app_context().CallInUIThreadSynchronous([this]() {
      xe::ui::ImGuiDialog::ShowMessageBox(
          imgui_drawer_, "Uh-oh!",
          "The guest has crashed.\n\n"
          ""
          "Xenia has now paused itself.\n"
          "A crash dump has been written into the log.");
    });
  }
#else
  if (guest_function) {
    XELOGE("Guest crashed! PC: 0x{:08X}",
           guest_function->MapMachineCodeToGuestAddress(ex->pc()));
  } else {
    XELOGE("Guest crashed! host PC=0x{:016X} (guest function unknown)", ex->pc());
  }
#endif

  // Now suspend ourself (we should be a guest thread).
  current_thread->Suspend(nullptr);

  // We should not arrive here!
  assert_always();
  return false;
}

void Emulator::WaitUntilExit() {
  while (true) {
    if (main_thread_) {
      xe::threading::Wait(main_thread_->thread(), false);
    }

    if (restoring_) {
      restore_fence_.Wait();
    } else {
      // Not restoring and the thread exited. We're finished.
      break;
    }
  }

  on_exit();
}

void Emulator::AddGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  // Check if already added.
  if (std::find(game_config_load_callbacks_.cbegin(),
                game_config_load_callbacks_.cend(),
                callback) != game_config_load_callbacks_.cend()) {
    return;
  }
  game_config_load_callbacks_.push_back(callback);
}

void Emulator::RemoveGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  auto it = std::find(game_config_load_callbacks_.cbegin(),
                      game_config_load_callbacks_.cend(), callback);
  if (it == game_config_load_callbacks_.cend()) {
    return;
  }
  if (game_config_load_callback_loop_next_index_ != SIZE_MAX) {
    // Actualize the next callback index after the erasure from the vector.
    size_t existing_index =
        size_t(std::distance(game_config_load_callbacks_.cbegin(), it));
    if (game_config_load_callback_loop_next_index_ > existing_index) {
      --game_config_load_callback_loop_next_index_;
    }
  }
  game_config_load_callbacks_.erase(it);
}

std::string Emulator::FindLaunchModule() {
  std::string path("game:\\");

  if (!cvars::launch_module.empty()) {
    return path + cvars::launch_module;
  }

  std::string default_module("default.xex");

  auto gameinfo_entry(file_system_->ResolvePath(path + "GameInfo.bin"));
  if (gameinfo_entry) {
    vfs::File* file = nullptr;
    X_STATUS result =
        gameinfo_entry->Open(vfs::FileAccess::kGenericRead, &file);
    if (XSUCCEEDED(result)) {
      std::vector<uint8_t> buffer(gameinfo_entry->size());
      size_t bytes_read = 0;
      result = file->ReadSync(buffer.data(), buffer.size(), 0, &bytes_read);
      if (XSUCCEEDED(result)) {
        kernel::util::GameInfo info(buffer);
        if (info.is_valid()) {
          XELOGI("Found virtual title {}", info.virtual_title_id());

          const std::string xna_id("584E07D1");
          auto xna_id_entry(file_system_->ResolvePath(path + xna_id));
          if (xna_id_entry) {
            default_module = xna_id + "\\" + info.module_name();
          } else {
            XELOGE("Could not find fixed XNA path {}", xna_id);
          }
        }
      }
    }
  }

  return path + default_module;
}

static std::string format_version(xex2_version version) {
  // fmt::format doesn't like bit fields
  uint32_t major, minor, build, qfe;
  major = version.major;
  minor = version.minor;
  build = version.build;
  qfe = version.qfe;
  if (qfe) {
    return fmt::format("{}.{}.{}.{}", major, minor, build, qfe);
  }
  if (build) {
    return fmt::format("{}.{}.{}", major, minor, build);
  }
  return fmt::format("{}.{}", major, minor);
}

X_STATUS Emulator::CompleteLaunch(const std::filesystem::path& path,
                                  const std::string_view module_path) {
#ifndef XE_HEADLESS_BUILD
  // Making changes to the UI (setting the icon) and executing game config load
  // callbacks which expect to be called from the UI thread.
  assert_true(display_window_->app_context().IsInUIThread());
#else
  // Headless mode: no display window
  assert_true(display_window_ == nullptr);
#endif

  // Setup NullDevices for raw HDD partition accesses
  // Cache/STFC code baked into games tries reading/writing to these
  // By using a NullDevice that just returns success to all IO requests it
  // should allow games to believe cache/raw disk was accessed successfully

  // NOTE: this should probably be moved to xenia_main.cc, but right now we need
  // to register the \Device\Harddisk0\ NullDevice _after_ the
  // \Device\Harddisk0\Partition1 HostPathDevice, otherwise requests to
  // Partition1 will go to this. Registering during CompleteLaunch allows us to
  // make sure any HostPathDevices are ready beforehand.
  // (see comment above cache:\ device registration for more info about why)
  auto null_paths = {std::string("\\Partition0"), std::string("\\Cache0"),
                     std::string("\\Cache1")};
  auto null_device =
      std::make_unique<vfs::NullDevice>("\\Device\\Harddisk0", null_paths);
  if (null_device->Initialize()) {
    file_system_->RegisterDevice(std::move(null_device));
  }

  // Reset state.
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
#ifndef XE_HEADLESS_BUILD
  display_window_->SetIcon(nullptr, 0);
#endif

  // Allow xam to request module loads.
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");

  XELOGI("Launching module {}", module_path);
  auto module = kernel_state_->LoadUserModule(module_path);
  if (!module) {
    XELOGE("Failed to load user module {}", xe::path_to_utf8(path));
    return X_STATUS_NOT_FOUND;
  }

  // Grab the current title ID.
  xex2_opt_execution_info* info = nullptr;
  module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &info);

  if (!info) {
    title_id_ = 0;
  } else {
    title_id_ = info->title_id;
    auto title_version = info->version();
    if (title_version.value != 0) {
      title_version_ = format_version(title_version);
    }
  }

  // Try and load the resource database (xex only).
  if (module->title_id()) {
    auto title_id = fmt::format("{:08X}", module->title_id());

    // Load the per-game configuration file and make sure updates are handled by
    // the callbacks.
    config::LoadGameConfig(title_id);
    assert_true(game_config_load_callback_loop_next_index_ == SIZE_MAX);
    game_config_load_callback_loop_next_index_ = 0;
    while (game_config_load_callback_loop_next_index_ <
           game_config_load_callbacks_.size()) {
      game_config_load_callbacks_[game_config_load_callback_loop_next_index_++]
          ->PostGameConfigLoad();
    }
    game_config_load_callback_loop_next_index_ = SIZE_MAX;

    const kernel::util::XdbfGameData db = kernel_state_->module_xdbf(module);
    if (db.is_valid()) {
      XLanguage language =
          db.GetExistingLanguage(static_cast<XLanguage>(cvars::user_language));
      title_name_ = db.title(language);

      XELOGI("-------------------- ACHIEVEMENTS --------------------");
      const std::vector<kernel::util::XdbfAchievementTableEntry>
          achievement_list = db.GetAchievements();
      for (const kernel::util::XdbfAchievementTableEntry& entry :
           achievement_list) {
        std::string label = db.GetStringTableEntry(language, entry.label_id);
        std::string desc =
            db.GetStringTableEntry(language, entry.description_id);

        XELOGI("{} - {} - {} - {}", entry.id, label, desc, entry.gamerscore);
      }
      XELOGI("----------------- END OF ACHIEVEMENTS ----------------");

      auto icon_block = db.icon();
      if (icon_block) {
#ifndef XE_HEADLESS_BUILD
        display_window_->SetIcon(icon_block.buffer, icon_block.size);
#endif
      }
    }
  }

  // milo-trace (X-track) capture-session activation (X5). Driven purely by the
  // --milo_trace_* cvars so it works for ANY title (dc3 validation ground truth
  // AND rb3-xenon discovery), independent of the DC3-specific NUI-patch block
  // below. Opened here — after the module is loaded and title_id_ is known, but
  // before the title's main guest code runs — so the GuestFunction::Call
  // override-wrap hook (X2) is live for every traced call. MiloTraceBegin is a
  // no-op (leaves IsActive()==false) when out_path is empty, so the common
  // no-flag path costs nothing.
  if (cvars::milo_trace_enable && !cvars::milo_trace_out.empty()) {
    cpu::MiloTraceConfig milo_config;
    milo_config.title_id =
        title_id_.has_value() ? fmt::format("{:08X}", title_id_.value()) : "";
    milo_config.out_path = cvars::milo_trace_out;
    milo_config.manifest_path = cvars::milo_trace_manifest;
    milo_config.target_sha1 = "";  // provenance; filled in a later task
    milo_config.arch = 2;          // ppc-xenon
    milo_config.capture_method = 3;  // xenia_override (the X2 Call-hook path)
    milo_config.exact_mem = cvars::milo_trace_exact_mem;  // X6-capture-fix
    cpu::MiloTraceBegin(milo_config);
    if (cpu::MiloTraceIsActive()) {
      XELOGI("milo-trace: session active for title {} -> '{}'",
             milo_config.title_id, milo_config.out_path);
    } else {
      XELOGW("milo-trace: --milo_trace_enable set but session did not open "
             "(out='{}')", milo_config.out_path);
    }
  }

  // Initializing the shader storage in a blocking way so the user doesn't miss
  // the initial seconds - for instance, sound from an intro video may start
  // playing before the video can be seen if doing this in parallel with the
  // main thread.
  on_shader_storage_initialization(true);
  graphics_system_->InitializeShaderStorage(cache_root_, title_id_.value(),
                                            true);
  on_shader_storage_initialization(false);

  // Push --rb3dx_alloc_probe down into the MMIO fault handler. src/xenia/cpu
  // used to DECLARE_bool this cvar, which made the CPU library depend on a
  // symbol DEFINEd here; the setter keeps the dependency pointing downwards.
  // Unconditional (not title-gated) to match the previous cvar read.
  cpu::MMIOHandler::SetAllocProbeEnabled(cvars::rb3dx_alloc_probe);

  // RB3DX (0x45410914) DIAGNOSTIC: MemAlloc argument probe for the main_hub
  // corrupted-size OOM investigation. Default off (--rb3dx_alloc_probe).
  // No guest byte patches: overrides the pre-declared __savegprlr_23 helper
  // (0x82829244; declared by XexModule::FindSaveRest, not yet compiled at
  // this point) with an exact host emulation + a probe filtered to
  // MemAlloc's call site (lr == 0x827BCD40). DC3-inert: title-gated +
  // default-off cvar.
  if (title_id_.has_value() && title_id_.value() == 0x45410914 &&
      (cvars::rb3dx_alloc_probe || cvars::rb3dx_clamp_alloc ||
       cvars::si_selftest || cvars::si_load_dll ||
       !cvars::rb3dx_alloc_trace_path.empty())) {
    const uint32_t kSaveGprLr23 = 0x82829244;
    auto* mem = memory_->virtual_membase();
    uint32_t insn0 = xe::load_and_swap<uint32_t>(mem + kSaveGprLr23);
    if (insn0 != 0xFAE1FFB0) {  // std r23,-0x50(r1)
      XELOGW(
          "RB3DX: alloc probe NOT installed (unexpected __savegprlr_23 "
          "word 0x{:08X})",
          insn0);
    } else {
      processor_->RegisterGuestFunctionOverride(
          kSaveGprLr23, Rb3dxSaveGprLr23ProbeExtern,
          "RB3DX:__savegprlr_23(probe)");
      XELOGI(
          "RB3DX: MemAlloc probe installed via __savegprlr_23 override at "
          "0x{:08X} (filter lr=0x827BCD40)",
          kSaveGprLr23);
    }
  }

  // RB3 TU5 (0x45410914): no-op CharSync::UpdateCharCache to clear the
  // head-of-line char-cache stall that freezes the clean-TU5 boot at the
  // splash_screen transition (--rb3_no_char_preview). Register early here,
  // before the JIT compiles the direct `bl 0x82564698` in App::App (0x82271490),
  // so the override takes. Title-gated + default-off => DC3-inert.
  if (title_id_.has_value() && title_id_.value() == 0x45410914 &&
      cvars::rb3_no_char_preview) {
    const uint32_t kUpdateCharCache = 0x82564698;
    processor_->RegisterGuestFunctionOverride(
        kUpdateCharCache, Rb3NoCharPreviewExtern,
        "RB3:CharSync::UpdateCharCache(no-op)");
    XELOGI("RB3: UpdateCharCache no-op override installed at 0x{:08X}",
           kUpdateCharCache);
  }

  // RB3 TU5 (0x45410914) DIAGNOSTIC: hold main() from returning
  // (--rb3_tu5_hold_main). main_impl(0x82272E60) = App::_ct(0x82270E68);
  // reg(0x822703D0); broadcast(0x82270000); return 0 -- NONE is a frame loop,
  // so main returns ~15s in (right after the boot splash) and the guest CRT
  // exit path zombies every other guest thread (proven by the per-tid census,
  // wiki 8p). Byte-patch the terminal `blr` at 0x82272EB0 into `b .`
  // (0x48000000) so the entry thread spins there forever and the workers
  // survive. If a frontend/menu then appears, the game loop was on a worker
  // thread and main-return was killing it; if nothing changes, the frontend
  // never starts and needs an explicit kick (DC3-style GotoFirstScreen).
  // Title-gated + default-off => DC3-inert.
  if (title_id_.has_value() && title_id_.value() == 0x45410914 &&
      cvars::rb3_tu5_hold_main) {
    // Hold thr6 right BEFORE the App shutdown-broadcast call (bl 0x82270000 at
    // 0x82272E98). The per-tid census + per-sample tid6 chain proved thr6
    // exits DURING that broadcast (a subscriber handler tears the title down),
    // not via main's blr -- so spinning at the blr missed it. main_impl has
    // already run App::_ct(ctor) + reg(0x822703D0) by here, so ctor side
    // effects (workers spawned, UI transition kicked) are intact; only the
    // shutdown notification is suppressed. If the frontend then engages, the
    // teardown was broadcast-driven; if not, teardown is initiated elsewhere.
    const uint32_t kMainBcast = 0x82272E98;
    auto* heap = memory_->LookupHeap(kMainBcast);
    auto* mem = memory_->virtual_membase();
    uint32_t cur = xe::load_and_swap<uint32_t>(mem + kMainBcast);
    if (cur != 0x4BFFD169) {  // bl 0x82270000
      XELOGW("RB3: hold-main NOT installed (0x{:08X} is 0x{:08X}, not the "
             "broadcast bl)", kMainBcast, cur);
    } else if (heap && heap->Protect(kMainBcast & ~0xFFFu, 0x1000,
                                     kMemoryProtectRead | kMemoryProtectWrite)) {
      xe::store_and_swap<uint32_t>(mem + kMainBcast, 0x48000000u);  // b .
      XELOGI("RB3: hold-main installed -- main() broadcast bl @0x{:08X} -> `b .`",
             kMainBcast);
    } else {
      XELOGW("RB3: hold-main NOT installed (Protect failed @0x{:08X})",
             kMainBcast);
    }
  }

  // RB3 TU5 (0x45410914) EXPERIMENT: synthesize the missing frame loop
  // (--rb3_tu5_loop_main). main_impl's 3rd call is `bl 0x82270000` (poll
  // Pollables ONCE) at 0x82272E98; it does NOT pump TheLoadMgr, so the UI panel
  // loads never even start (mState=0). System::Poll (0x82510270, os/System.cpp)
  // IS the real per-frame body: it calls 0x827bf700 (Loader.cpp) on TheLoadMgr
  // (0x82E06E38) to pump the loader, polls the Pollables (0x8250f898), and
  // drives ~10 more subsystems. Two patches turn main's tail into a frame loop:
  //   (1) 0x82272E98: `bl 0x82270000` -> `bl 0x82510270` (call System::Poll)
  //   (2) 0x82272E9C: `li r3,0`       -> `b 0x82272E94` (loop back to the call)
  // so thr6 runs `addi r3,r31,0x50; System::Poll(...)` forever instead of
  // returning into the CRT's XamLoaderTerminateTitle. Title-gated + default-off
  // => DC3-inert.
  if (title_id_.has_value() && title_id_.value() == 0x45410914 &&
      cvars::rb3_tu5_loop_main) {
    const uint32_t kCall = 0x82272E98;   // bl 0x82270000
    const uint32_t kAfter = 0x82272E9C;  // li r3,0
    auto* heap = memory_->LookupHeap(kCall);
    auto* mem = memory_->virtual_membase();
    uint32_t c0 = xe::load_and_swap<uint32_t>(mem + kCall);
    uint32_t c1 = xe::load_and_swap<uint32_t>(mem + kAfter);
    if (c0 != 0x4BFFD169 || c1 != 0x38600000) {
      XELOGW("RB3: loop-main NOT installed (0x{:08X}=0x{:08X} 0x{:08X}=0x{:08X})",
             kCall, c0, kAfter, c1);
    } else if (heap && heap->Protect(kCall & ~0xFFFu, 0x1000,
                                     kMemoryProtectRead | kMemoryProtectWrite)) {
      // bl 0x82510270 from 0x82272E98: off = 0x82510270-0x82272E98 = 0x29D3D8.
      xe::store_and_swap<uint32_t>(mem + kCall, 0x4829D3D9u);   // bl 0x82510270
      xe::store_and_swap<uint32_t>(mem + kAfter, 0x4BFFFFF8u);  // b 0x82272E94
      XELOGI("RB3: loop-main installed -- main() -> `while(1) System::Poll()` "
             "(call@0x{:08X}=bl 0x82510270, loop@0x{:08X})", kCall, kAfter);
      // Also NOP LoadMgr::Poll's budget entry-gate (0x827BF720 `ble cr6,+0xA0`,
      // word 0x409900A0). Without a real per-frame load budget (this->0x10 at
      // 0x82E06E48, which the synthesized loop doesn't set), the gate makes Poll
      // return WITHOUT draining, so a front loader whose IsLoaded()==true is
      // never popped and head-of-line-blocks the FIFO (observed: the DONE
      // DirLoader for inline_help_center.milo stuck at front forever). NOPing
      // the gate makes every Poll enter the drain loop and pop loaded fronts.
      const uint32_t kGate = 0x827BF720;
      uint32_t g = xe::load_and_swap<uint32_t>(mem + kGate);
      if (g == 0x409900A0) {
        auto* gh = memory_->LookupHeap(kGate);
        if (gh && gh->Protect(kGate & ~0xFFFu, 0x1000,
                              kMemoryProtectRead | kMemoryProtectWrite)) {
          xe::store_and_swap<uint32_t>(mem + kGate, 0x60000000u);  // nop
          XELOGI("RB3: loop-main -- NOP'd LoadMgr::Poll budget gate @0x{:08X}",
                 kGate);
        }
      } else {
        XELOGW("RB3: loop-main -- gate @0x{:08X} is 0x{:08X}, not the ble; "
               "not NOP'd", kGate, g);
      }
    } else {
      XELOGW("RB3: loop-main NOT installed (Protect failed @0x{:08X})", kCall);
    }
  }

  // RB3DX (0x45410914): per-allocation binary trace for the heap-"main"
  // fragmentation attribution (--rb3dx_alloc_trace_path). Opens the sink and
  // registers the two extra overrides -- MemFree's prologue helper
  // __savegprlr_26 and MemAlloc's epilogue helper __restgprlr_23 -- both of
  // which are exact emulations of the pre-declared save/restore stubs, same
  // recipe as the alloc-entry probe above. Title-gated + empty-by-default =>
  // DC3-inert.
  if (title_id_.has_value() && title_id_.value() == 0x45410914 &&
      !cvars::rb3dx_alloc_trace_path.empty()) {
    rb3dx_alloc_trace_sink =
        std::make_unique<Rb3dxAllocTraceSink>(cvars::rb3dx_alloc_trace_path);
    if (!rb3dx_alloc_trace_sink->ok()) {
      XELOGE("RB3DX: alloc trace could NOT open '{}' -- tracing disabled",
             cvars::rb3dx_alloc_trace_path);
      rb3dx_alloc_trace_sink.reset();
    } else {
      XELOGI("RB3DX: alloc trace -> '{}' (32-byte records)",
             cvars::rb3dx_alloc_trace_path);
      auto* mem = memory_->virtual_membase();
      if (cvars::rb3dx_free_trace) {
        const uint32_t kSaveGprLr26 = 0x82829250;
        uint32_t w = xe::load_and_swap<uint32_t>(mem + kSaveGprLr26);
        if (w != 0xFB41FFC8) {  // std r26,-0x38(r1)
          XELOGW(
              "RB3DX: MemFree trace NOT installed (unexpected __savegprlr_26 "
              "word 0x{:08X})",
              w);
        } else {
          processor_->RegisterGuestFunctionOverride(
              kSaveGprLr26, Rb3dxSaveGprLr26ProbeExtern,
              "RB3DX:__savegprlr_26(free probe)");
          XELOGI(
              "RB3DX: MemFree probe installed via __savegprlr_26 override at "
              "0x{:08X} (filter lr=0x827BC438)",
              kSaveGprLr26);
        }
      }
      if (cvars::rb3dx_ret_trace) {
        const uint32_t kRestGprLr23 = 0x82829294;
        uint32_t w = xe::load_and_swap<uint32_t>(mem + kRestGprLr23);
        if (w != 0xEAE1FFB0) {  // ld r23,-0x50(r1)
          XELOGW(
              "RB3DX: MemAlloc return trace NOT installed (unexpected "
              "__restgprlr_23 word 0x{:08X})",
              w);
        } else {
          processor_->RegisterGuestFunctionOverride(
              kRestGprLr23, Rb3dxRestGprLr23TraceExtern,
              "RB3DX:__restgprlr_23(alloc return)");
          XELOGI(
              "RB3DX: MemAlloc return probe installed via __restgprlr_23 "
              "override at 0x{:08X}",
              kRestGprLr23);
        }
      }
      // Periodic flush + progress so a run that is killed on a wall-clock
      // deadline still leaves a complete-to-the-second trace on disk.
      Rb3dxSpawnProbeThread([]() {
        while (Rb3dxProbeSleep(10000)) {
          auto* sink = rb3dx_alloc_trace_sink.get();
          if (!sink) {
            return;
          }
          sink->Flush();
          XELOGI("RB3DX alloc trace: {} records ({} alloc returns matched)",
                 sink->written(),
                 rb3dx_ret_matches.load(std::memory_order_relaxed));
        }
      });
    }
  }

  // RB3DX (0x45410914) DIAGNOSTIC: passive UI-state sampler for the main_hub
  // load-stall investigation (--rb3dx_ui_probe). Read-only guest-memory
  // sampler on a detached host thread; no hooks, no patches. DC3-inert:
  // title-gated + default-off cvar.
  if (title_id_.has_value() && title_id_.value() == 0x45410914 &&
      cvars::rb3dx_ui_probe) {
    Memory* probe_mem = memory_.get();
    kernel::KernelState* probe_ks = kernel_state_.get();
    cpu::Processor* probe_proc = processor_.get();
    Rb3dxSpawnProbeThread([probe_mem, probe_ks, probe_proc]() {
      Rb3dxUiProbeThread(probe_mem, probe_ks, probe_proc);
    });
    XELOGI("RB3DX: UI probe sampler thread started (--rb3dx_ui_probe)");
  }

  // RB3DX / RB3 TU5 (0x45410914): same-instrument static-patch runtime observer
  // (--si_probe). Read-only host thread; title-gated + default-off => DC3-inert.
  if (title_id_.has_value() && title_id_.value() == 0x45410914 &&
      cvars::si_probe) {
    Memory* si_mem = memory_.get();
    Rb3dxSpawnProbeThread([si_mem]() { Rb3dxSiProbeThread(si_mem); });
    XELOGI("RB3DX: SI static-patch probe thread started (--si_probe)");
  }

  // RB3DX / RB3 TU5 (0x45410914): RB3Enhanced-DLL same-instrument GAMEPLAY-hook
  // install verifier (--si_hook_verify). Read-only host thread that decodes the
  // H1/H2 first-instruction words to confirm the runtime HookFunction detour
  // branches into DLL space at 0x84000000. Title-gated + default-off =>
  // DC3-inert.
  if (title_id_.has_value() && title_id_.value() == 0x45410914 &&
      cvars::si_hook_verify) {
    Memory* hv_mem = memory_.get();
    Rb3dxSpawnProbeThread([hv_mem]() { Rb3dxSiHookVerifyThread(hv_mem); });
    XELOGI("RB3DX: SI DLL-hook verify thread started (--si_hook_verify)");
  }

  // RB3DX (0x45410914): offline single-local-host join completion
  // (--rb3dx_offline_join). Title-gated + default-off => DC3-inert. Overrides
  // NetSession::IsHost() @0x823CECE0 so the offline overshell local-user join
  // takes the synchronous host-success path and splash_screen advances to
  // main_hub (see Rb3dxIsHostOfflineExtern). No guest byte patches.
  if (title_id_.has_value() && title_id_.value() == 0x45410914 &&
      cvars::rb3dx_offline_join) {
    // The running default.xex (retail RB3 + TU5 + RB3DX) is a DIFFERENT build
    // than the rb3-xenon decomp image, so guest code is relocated and a
    // hardcoded address is wrong. NetSession::IsHost()'s mState state-machine
    // body is byte-identical across builds though (same source/compiler), so
    // locate it by a relocation-independent instruction signature (branch
    // words are wildcarded to tolerate any layout shift), anchored on the
    // distinctive `lwz r11,0x68(r3)` (mState) + the 7/3/4/5/6 state compares:
    //   [+0x00] lwz   r11, 0x68(r3)   0x81630068  (mState)
    //   [+0x04] cmpwi cr6, r11, 7     0x2F0B0007  (kRequestingNewUser)
    //   [+0x0C] cmpwi cr6, r11, 3     0x2F0B0003
    //   [+0x14] cmpwi cr6, r11, 4     0x2F0B0004
    //   [+0x1C] cmpwi cr6, r11, 5     0x2F0B0005
    //   [+0x24] cmpwi cr6, r11, 6     0x2F0B0006
    // IsHost entry = anchor - 0xC (mflr r12 / stw r12,-8 / stwu r1,-0x60).
    Memory* mem = memory_.get();
    uint8_t* base = mem->virtual_membase();
    auto page_readable = [&](uint32_t addr) -> bool {
      auto* heap = mem->LookupHeap(addr);
      if (!heap) return false;
      uint32_t prot = 0;
      if (!heap->QueryProtect(addr, &prot)) return false;
      return (prot & kMemoryProtectRead) != 0;
    };
    auto w32 = [&](uint32_t a) -> uint32_t {
      return xe::load_and_swap<uint32_t>(base + a);
    };
    uint32_t found = 0;
    for (uint32_t addr = 0x82000000; addr < 0x83000000 && !found; addr += 0x1000) {
      if (!page_readable(addr)) continue;
      uint32_t page_end = addr + 0x1000;
      // Anchor may straddle a page; require the whole 0x28-byte window to be in
      // a readable page too (skip the last few words near a boundary — the
      // real function never sits across an unmapped gap).
      for (uint32_t a = addr; a + 0x28 <= page_end; a += 4) {
        if (w32(a) != 0x81630068) continue;
        if (w32(a + 0x04) != 0x2F0B0007) continue;
        if (w32(a + 0x0C) != 0x2F0B0003) continue;
        if (w32(a + 0x14) != 0x2F0B0004) continue;
        if (w32(a + 0x1C) != 0x2F0B0005) continue;
        if (w32(a + 0x24) != 0x2F0B0006) continue;
        // Confirm the prologue 0xC bytes before the anchor.
        uint32_t entry = a - 0xC;
        if (page_readable(entry) && w32(entry) == 0x7D8802A6 &&
            w32(entry + 0x4) == 0x9181FFF8) {
          found = entry;
        }
        break;
      }
    }
    if (!found) {
      XELOGW(
          "RB3DX: offline-join NOT installed (NetSession::IsHost signature not "
          "found in 0x82000000-0x83000000)");
    } else {
      // Diagnostic: count `bl found` sites in the code region. If >0, IsHost is
      // a real call target (not inlined) and 0 runtime hits => AddLocalUser was
      // never reached; if 0, IsHost was inlined and this override cannot see it.
      uint32_t bl_callers = 0;
      for (uint32_t a = 0x82000000; a < 0x83000000; a += 0x1000) {
        if (!page_readable(a)) continue;
        for (uint32_t p = a; p < a + 0x1000; p += 4) {
          uint32_t insn = w32(p);
          if ((insn >> 26) != 18 || (insn & 1) != 1) continue;  // bl only
          int32_t li = static_cast<int32_t>(insn & 0x03FFFFFC);
          if (li & 0x02000000) li -= 0x04000000;
          uint32_t tgt = (insn & 2) ? static_cast<uint32_t>(li)
                                    : (p + static_cast<uint32_t>(li));
          if (tgt == found) ++bl_callers;
        }
      }
      processor_->RegisterGuestFunctionOverride(
          found, Rb3dxIsHostOfflineExtern, "RB3DX:NetSession::IsHost(offline)");
      XELOGI(
          "RB3DX: offline single-host join enabled via NetSession::IsHost "
          "override at 0x{:08X} (signature-located, {} bl-callers)",
          found, bl_callers);
    }
  }

  // RB3DX / RB3 TU5 (0x45410914): first-boot calibration skip
  // (--rb3dx_skip_calibration). Title-gated + default-off => DC3-inert.
  // Spawns a host thread that resolves the guest `profile_mgr` singleton via
  // the main-dir name hash and writes mHasSeenFirstTimeCalibration@+0x54 = 1
  // repeatedly, so the splash's kSplashScreen_EndOvershell decision
  // `{! {profile_mgr get_has_seen_first_time_calibration}}` sees "already
  // calibrated" and routes to main_hub_screen instead of the uncompletable
  // headless cal_audio_screen (see Rb3dxSkipCalibrationPokeThread). The handler
  // reads the field inline, so a guest-function override cannot catch it — a
  // direct memory poke does. Single-byte guest write.
  if (title_id_.has_value() && title_id_.value() == 0x45410914 &&
      cvars::rb3dx_skip_calibration) {
    Memory* poke_mem = memory_.get();
    Rb3dxSpawnProbeThread(
        [poke_mem]() { Rb3dxSkipCalibrationPokeThread(poke_mem); });
    XELOGI(
        "RB3DX: first-boot calibration skip thread started "
        "(--rb3dx_skip_calibration)");
  }

  // DC3 title-specific guest code patches.
  // DC3 Title ID: 0x373307D9 (Dance Central 3)
  bool dc3_is_decomp_layout = false;
  std::optional<Dc3NuiPatchManifest> dc3_patch_manifest;
  // Title-gated only: dc3_is_decomp_layout is not known yet (layout detection
  // runs ~500 lines below), so gating on it here made this whole block dead
  // code -- the manifest never loaded and Dc3PopulateAddressesFromCatalog
  // never ran, leaving kAddr on compiled-in defaults for every boot.  Loading
  // the manifest for an original-layout image is harmless: every consumer
  // (hack pack, kAddr populate) is separately gated on the detected layout.
  if (title_id_.has_value() && title_id_.value() == 0x373307D9) {
    Dc3MaybeCleanStaleContentCache(content_root_);

    // Opt this title in to the MMIO write soft-fault (64KB-vs-4KB protect
    // granularity conflict in the XEX image data region). These two constants
    // track the DC3 debug build's data layout and used to be hardcoded inside
    // the shared fault handler, where they applied to every game; they are the
    // exact values that were in mmio_handler.cc, kept verbatim so DC3 boot
    // behavior is unchanged. Log the module's .data bounds next to them so
    // drift is visible after a relink (the range deliberately spans more than
    // .data alone, so it is not derived from the section table).
    cpu::MMIOHandler::SetSoftFaultWritableRange(0x83320000, 0x836C0000);
    if (auto* xex = module->xex_module()) {
      if (auto* data = xex->GetPESection(".data")) {
        XELOGI("DC3: module .data is [{:08X}, {:08X}) (soft-fault writable "
               "range is [83320000, 836C0000))",
               data->address, data->address + data->size);
      }
    }

    // Load the patch manifest early so it's available for both NUI patching
    // and the hack pack (which runs independently of --stub_nui_functions).
    std::filesystem::path early_manifest_path;
    if (!cvars::dc3_nui_patch_manifest_path.empty()) {
      early_manifest_path = cvars::dc3_nui_patch_manifest_path;
    } else if (auto auto_path = Dc3AutoProbePatchManifestPath()) {
      early_manifest_path = *auto_path;
    }
    if (!early_manifest_path.empty()) {
      dc3_patch_manifest = Dc3LoadNuiPatchManifest(early_manifest_path);
      if (dc3_patch_manifest) {
        XELOGI("DC3: Loaded patch manifest '{}' (build_label={} targets={} "
               "crt={} hack_pack_stubs={})",
               xe::path_to_utf8(early_manifest_path),
               dc3_patch_manifest->build_label,
               dc3_patch_manifest->targets.size(),
               dc3_patch_manifest->crt_sentinels.size(),
               dc3_patch_manifest->hack_pack_stubs.size());
      } else {
        XELOGW("DC3: Failed to load patch manifest '{}'",
               xe::path_to_utf8(early_manifest_path));
      }
    }
  }

  //
  // The DC3 debug build statically links the Xbox 360 Kinect SDK (NUI).
  // NuiInitialize and related functions are PPC code embedded in the XEX,
  // NOT kernel imports. They fail because Kinect hardware doesn't exist
  // in xenia. The debug build's MILO_ASSERT halts on these failures.
  //
  // We patch the NUI functions in guest memory with PPC stubs before the
  // JIT compiles them. Each stub is 2 instructions (8 bytes):
  //   li r3, 0    (0x38600000) - return S_OK
  //   blr         (0x4E800020) - return
  //
  // This is the standard emulator approach for HLE of statically-linked
  // SDK functions (similar to Dolphin's OS HLE patches).
  if (title_id_.has_value() && title_id_.value() == 0x373307D9 &&
      cvars::stub_nui_functions) {
    XELOGI("DC3: Stubbing NUI (Kinect SDK) functions in guest memory");

    // PPC instructions (big-endian)
    const uint32_t kLiR3_0 = 0x38600000;    // li r3, 0  (return S_OK / 0)
    const uint32_t kLiR3_1 = 0x38600001;    // li r3, 1
    const uint32_t kLiR3_Neg1 = 0x3860FFFF; // li r3, -1 (return E_UNEXPECTED)
    const uint32_t kBlr = 0x4E800020;       // blr

    // NUI function addresses from the DC3 debug XEX (ham_xbox_r.exe).
    // These are guest virtual addresses for the statically-linked NUI SDK.
    // The functions are PPC code from the Xbox 360 Kinect SDK libraries.
    //
    // Addresses sourced from: dc3-decomp/config/373307D9/symbols.txt
    //
    // TODO: These addresses are specific to the DC3 debug build. A future
    // enhancement could detect the build variant (debug vs retail) by
    // checking function prologues before patching, or use a title-specific
    // patch config file.
    //
    // TODO: Consider implementing proper NUI emulation in xenia's kernel
    // layer instead of guest memory patches. This would involve:
    //   1. Adding NUI kernel module exports (NuiInitialize, NuiShutdown, etc.)
    //      similar to xam_nui.cc's XamNui* exports
    //   2. Registering them in the export resolver so import thunks work
    //   3. But core NUI functions are statically linked, NOT imported, so
    //      this approach would require either:
    //      a) Modifying x64_emitter.cc's Call() to check kExtern behavior
    //         (currently only CallExtern for sc2 checks it), or
    //      b) Adding a guest function override mechanism to Processor that
    //         intercepts calls by address (using ExternHandler + patching
    //         the indirection table entry)
    //   4. A proper implementation could provide skeleton data, depth maps,
    //      etc. for automated testing of gesture/dance gameplay
    // Functions that MUST return S_OK (game asserts on failure):
    //   - NuiInitialize: LiveCameraInput ctor line 129 MILO_ASSERT_FMT
    //   - NuiSkeletonTrackingEnable: LiveCameraInput ctor line 141
    //   - NuiImageStreamOpen: LiveCameraInput ctor lines 146, 160
    //     Returns S_OK but does NOT write output handle -> handle stays NULL
    //     -> NuiImageStreamGetNextFrame guarded by if(handle) -> never called
    //
    // Functions that can return failure (game handles gracefully):
    //   - NuiAudioCreate: wrapped in if(SUCCEEDED(...)), no assert
    //   - NuiImageStreamGetNextFrame: no assert, just skips frame processing
    //
    // Functions called from destructor (NuiShutdown always, audio only if
    // NuiAudioCreate succeeded):
    //   - NuiShutdown: called unconditionally
    //   - NuiAudioRelease: only if unk11d4 set (NuiAudioCreate success)
    //
    // The NUI SDK has ~50 functions statically linked. We stub all known
    // entry points to return S_OK (0). Functions that produce frame data
    // (GetNextFrame) return E_UNEXPECTED (-1) to signal "no data available".
    //
    // TODO: For more complete Kinect emulation, these stubs could be
    // replaced with C++ handlers that provide:
    //   - Synthetic skeleton data for automated gameplay testing
    //   - Pre-recorded depth/color streams for regression testing
    //   - Scripted gesture sequences for CI/CD integration
    //   This would require the Processor-level ExternHandler approach
    //   described above, since bl-called functions don't go through
    //   the CallExtern codepath.

    Dc3NuiPatchSpec patches[] = {
        // Core lifecycle
        {0x829D1200, kLiR3_0, kBlr, "NuiInitialize"},
        {0x829CEDA0, kLiR3_0, kBlr, "NuiShutdown"},

        // Skeleton tracking
        {0x829C25F0, kLiR3_0, kBlr, "NuiSkeletonTrackingEnable"},
        {0x829C1E18, kLiR3_0, kBlr, "NuiSkeletonTrackingDisable"},
        {0x829C1F90, kLiR3_0, kBlr, "NuiSkeletonSetTrackedSkeletons"},
        {0x829C2790, kLiR3_Neg1, kBlr, "NuiSkeletonGetNextFrame"},

        // Image streams - return S_OK but don't write output handle.
        // The handle pointer (r8) stays NULL -> GetNextFrame never called
        // because LiveCameraInput::PollTracking guards with if(curBuf.unk0).
        {0x829C9330, kLiR3_0, kBlr, "NuiImageStreamOpen"},
        {0x829C86F0, kLiR3_Neg1, kBlr, "NuiImageStreamGetNextFrame"},
        {0x829C8A18, kLiR3_0, kBlr, "NuiImageStreamReleaseFrame"},
        {0x829C91C8, kLiR3_0, kBlr, "NuiImageGetColorPixelCoordinatesFromDepthPixel"},

        // Audio - NuiAudioCreate returning failure (E_UNEXPECTED) prevents
        // the game from calling NuiAudioRegisterCallbacks (no assert).
        // LiveCameraInput sets unk11d4=0, destructor skips audio cleanup.
        //
        // TODO: NuiAudioCreate internally accesses global NUI state via
        // RtlEnterCriticalSection on a NUI mutex, allocates buffers with
        // XMemAlloc, calls XamVoiceGetMicArrayAudioEx for Kinect mic array,
        // creates 2 audio threads. Full audio emulation would require
        // implementing the NUIAUDIO subsystem with:
        //   - MEC (Microphone Echo Cancellation) stub
        //   - XamVoiceGetMicArrayAudioEx returning dummy audio streams
        //   - Audio processing thread stubs
        {0x82A0E028, kLiR3_Neg1, kBlr, "NuiAudioCreate"},
        {0x82A0DA48, kLiR3_Neg1, kBlr, "NuiAudioCreatePrivate"},
        {0x82A0D928, kLiR3_0, kBlr, "NuiAudioRegisterCallbacks"},
        {0x82A0D9A0, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacks"},
        {0x82A0C0A0, kLiR3_0, kBlr, "NuiAudioRegisterCallbacksPrivate"},
        {0x82A0C108, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacksPrivate"},
        {0x82A0D440, kLiR3_0, kBlr, "NuiAudioRelease"},

        // Camera properties - called at end of LiveCameraInput ctor
        // (SetColorCameraProperty) and in diagnostic/debug draw code.
        // Must not crash; accessing uninitialized NUI global state would
        // segfault without this stub.
        //
        // TODO: Camera property stubs could track set values in a map
        // and return them from Get calls, enabling camera config testing
        // without Kinect hardware.
        {0x829C7F48, kLiR3_0, kBlr, "NuiCameraSetProperty"},
        {0x829C7058, kLiR3_0, kBlr, "NuiCameraGetProperty"},
        {0x829C7068, kLiR3_0, kBlr, "NuiCameraGetPropertyF"},
        {0x829C7FA0, kLiR3_0, kBlr, "NuiCameraSetExposureRegionOfInterest"},
        {0x829C6868, kLiR3_0, kBlr, "NuiCameraGetExposureRegionOfInterest"},
        {0x829C3FE0, kLiR3_0, kBlr, "NuiCameraElevationSetAngle"},
        {0x829C3EF8, kLiR3_0, kBlr, "NuiCameraElevationGetAngle"},
        {0x829C4940, kLiR3_0, kBlr, "NuiCameraAdjustTilt"},
        {0x829C4E38, kLiR3_0, kBlr, "NuiCameraGetNormalToGravity"},

        // Identity (used by Skeleton.cpp for player identification)
        //
        // TODO: NuiIdentityIdentify takes a tracking ID, flags, callback,
        // and user data. A proper stub could invoke the callback with a
        // "no match" result to simulate identity processing completing.
        {0x829C36B0, kLiR3_0, kBlr, "NuiIdentityEnroll"},
        {0x829C3870, kLiR3_0, kBlr, "NuiIdentityIdentify"},
        {0x829C3998, kLiR3_0, kBlr, "NuiIdentityGetEnrollmentInformation"},
        {0x829C3BB0, kLiR3_0, kBlr, "NuiIdentityAbort"},

        // Fitness tracking (FitnessFilter.cpp)
        // Only called during fitness gameplay mode. All use MILO_NOTIFY
        // on failure (not MILO_ASSERT), so failure is safe.
        {0x829D1B68, kLiR3_Neg1, kBlr, "NuiFitnessStartTracking"},
        {0x829D1E30, kLiR3_Neg1, kBlr, "NuiFitnessPauseTracking"},
        {0x829D1F00, kLiR3_Neg1, kBlr, "NuiFitnessResumeTracking"},
        {0x829D1FD0, kLiR3_Neg1, kBlr, "NuiFitnessStopTracking"},
        {0x82E61690, kLiR3_Neg1, kBlr, "NuiFitnessGetCurrentFitnessData"},

        // Wave gesture (WaveToTurnOnLight.cpp)
        {0x829D1758, kLiR3_Neg1, kBlr, "NuiWaveSetEnabled"},
        {0x829D1668, kLiR3_Neg1, kBlr, "NuiWaveGetGestureOwnerProgress"},

        // Head tracking
        {0x829DA0C8, kLiR3_0, kBlr, "NuiHeadOrientationDisable"},
        {0x829DA598, kLiR3_0, kBlr, "NuiHeadPositionDisable"},

        // Speech recognition (SpeechMgr.cpp) - many have MILO_ASSERT_FMT.
        // SpeechMgr is only created if kinect.speech.enabled=1 in config.
        // If speech IS enabled, these must return S_OK to avoid asserts
        // in NuiSpeechCreateGrammar, NuiSpeechCommitGrammar, etc.
        //
        // TODO: Speech emulation could accept pre-scripted voice commands
        // for automated testing of menu navigation and gameplay triggers.
        // Would need to implement:
        //   - Grammar state management (rule tree in host memory)
        //   - Event queue with synthetic recognition events
        //   - NuiSpeechGetEvents returning scripted results
        {0x82A24B88, kLiR3_0, kBlr, "NuiSpeechEnable"},
        {0x82A23B70, kLiR3_0, kBlr, "NuiSpeechDisable"},
        {0x82A23BB0, kLiR3_0, kBlr, "NuiSpeechCreateGrammar"},
        {0x82A23B80, kLiR3_0, kBlr, "NuiSpeechLoadGrammar"},
        {0x82A23BA0, kLiR3_0, kBlr, "NuiSpeechUnloadGrammar"},
        {0x82A22A48, kLiR3_0, kBlr, "NuiSpeechCommitGrammar"},
        {0x82A21068, kLiR3_0, kBlr, "NuiSpeechStartRecognition"},
        {0x82A22978, kLiR3_0, kBlr, "NuiSpeechStopRecognition"},
        {0x82A21090, kLiR3_0, kBlr, "NuiSpeechSetEventInterest"},
        {0x82A21078, kLiR3_0, kBlr, "NuiSpeechSetGrammarState"},
        {0x82A22998, kLiR3_0, kBlr, "NuiSpeechSetRuleState"},
        {0x82A229B8, kLiR3_0, kBlr, "NuiSpeechCreateRule"},
        {0x82A229E0, kLiR3_0, kBlr, "NuiSpeechCreateState"},
        {0x82A22A00, kLiR3_0, kBlr, "NuiSpeechAddWordTransition"},
        {0x82A210A0, kLiR3_Neg1, kBlr, "NuiSpeechGetEvents"},
        {0x82A22988, kLiR3_0, kBlr, "NuiSpeechDestroyEvent"},
        // NOTE: 0x82A24A98 was previously stubbed as "NuiSpeech__E_init"
        // but MAP file reveals it's actually Object::sFactories static
        // initializer (Object.obj) — a critical game engine function.
        // Stubbing it broke the object factory system and caused hangs
        // in downstream initializers (gPropPaths at 0x82A24B28).
        //
        // The original JIT boundary issue (blr in EmulateRecognition stub
        // at 0x82A24AB0 getting scanned into the initializer) is avoided
        // by also not stubbing NuiSpeechEmulateRecognition — the original
        // function prologue doesn't have an early blr, so the JIT
        // boundary detection works correctly with the original code.
        //
        // NuiSpeechEmulateRecognition (0x82A24AB0) is never called
        // because all speech API entry points are already stubbed above.

        // Misc
        {0x82B57560, kLiR3_0, kBlr, "NuiMetaCpuEvent"},

        // Xbox SmartGlass (XBC) SDK - statically linked from XBC.lib
        // SmartGlassInit() calls XbcInitialize(), which calls
        // CXbcImpl::Initialize(). If Initialize returns failure, the game
        // prints "Failed to initialize Xbox SmartGlass library." and crashes.
        // The thunk functions (XbcInitialize, XbcDoWork, XbcSendJSON) are
        // only 4 bytes each (branch instructions), so we stub the real
        // CXbcImpl implementations instead.
        //
        // TODO: SmartGlass could be used for controller input automation
        // (e.g., sending menu selections from a test harness). Would need:
        //   - JSON message parsing/generation
        //   - Client connection state management
        //   - XLRC (Xbox Live Real-time Communication) stub layer
        {0x82606078, kLiR3_0, kBlr, "CXbcImpl::Initialize"},
        {0x82605960, kLiR3_0, kBlr, "CXbcImpl::DoWork"},
        {0x82605DF8, kLiR3_0, kBlr, "CXbcImpl::SendJSON"},
    };

    // Decomp XEX patch table: NUI functions at decomp MAP public symbol
    // addresses. Used when the original-address patches don't match the
    // loaded XEX layout. Addresses sourced from manifest generator
    // (generate_xenia_dc3_patch_manifest.py) which reads the MAP file.
    // When the manifest JSON is available, the resolver prefers manifest
    // addresses over these catalog entries.
    Dc3NuiPatchSpec decomp_patches[] = {
        // Core lifecycle (nuiruntime.obj)
        {0x835D8B0C, kLiR3_0, kBlr, "NuiInitialize"},
        {0x835D66D0, kLiR3_0, kBlr, "NuiShutdown"},
        {0x8373F0E0, kLiR3_0, kBlr, "NuiMetaCpuEvent"},

        // Skeleton tracking (nuiskeleton.obj)
        {0x835CA2D8, kLiR3_0, kBlr, "NuiSkeletonTrackingEnable"},
        {0x835C9B28, kLiR3_0, kBlr, "NuiSkeletonTrackingDisable"},
        {0x835C9C98, kLiR3_0, kBlr, "NuiSkeletonSetTrackedSkeletons"},
        {0x835CA478, kLiR3_Neg1, kBlr, "NuiSkeletonGetNextFrame"},

        // Image streams (nuiimagecamera.obj)
        {0x835D0F7C, kLiR3_0, kBlr, "NuiImageStreamOpen"},
        {0x835D0354, kLiR3_Neg1, kBlr, "NuiImageStreamGetNextFrame"},
        {0x835D067C, kLiR3_0, kBlr, "NuiImageStreamReleaseFrame"},
        {0x835D0E18, kLiR3_0, kBlr,
         "NuiImageGetColorPixelCoordinatesFromDepthPixel"},

        // Audio (nuiaudio.obj)
        {0x8360B778, kLiR3_Neg1, kBlr, "NuiAudioCreate"},
        {0x8360B198, kLiR3_Neg1, kBlr, "NuiAudioCreatePrivate"},
        {0x8360B07C, kLiR3_0, kBlr, "NuiAudioRegisterCallbacks"},
        {0x8360B0F4, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacks"},
        {0x83609818, kLiR3_0, kBlr, "NuiAudioRegisterCallbacksPrivate"},
        {0x83609880, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacksPrivate"},
        {0x8360AB98, kLiR3_0, kBlr, "NuiAudioRelease"},

        // Camera properties (nuidetroit.obj, nuiimagecameraproperties.obj)
        {0x835CFBC4, kLiR3_0, kBlr, "NuiCameraSetProperty"},
        {0x835CBBB4, kLiR3_0, kBlr, "NuiCameraElevationGetAngle"},
        {0x835CBC94, kLiR3_0, kBlr, "NuiCameraElevationSetAngle"},
        {0x835CC5E0, kLiR3_0, kBlr, "NuiCameraAdjustTilt"},
        {0x835CCAD0, kLiR3_0, kBlr, "NuiCameraGetNormalToGravity"},
        {0x835CFC1C, kLiR3_0, kBlr, "NuiCameraSetExposureRegionOfInterest"},
        {0x835CE4DC, kLiR3_0, kBlr, "NuiCameraGetExposureRegionOfInterest"},
        {0x835CECE0, kLiR3_0, kBlr, "NuiCameraGetProperty"},
        {0x835CECF0, kLiR3_0, kBlr, "NuiCameraGetPropertyF"},

        // Identity (identityapi.obj)
        {0x835CB384, kLiR3_0, kBlr, "NuiIdentityEnroll"},
        {0x835CB540, kLiR3_0, kBlr, "NuiIdentityIdentify"},
        {0x835CB668, kLiR3_0, kBlr, "NuiIdentityGetEnrollmentInformation"},
        {0x835CB87C, kLiR3_0, kBlr, "NuiIdentityAbort"},

        // Fitness (nuifitnessapi.obj, nuifitnessxam.obj)
        {0x835D9460, kLiR3_Neg1, kBlr, "NuiFitnessStartTracking"},
        {0x835D9728, kLiR3_Neg1, kBlr, "NuiFitnessPauseTracking"},
        {0x835D97F8, kLiR3_Neg1, kBlr, "NuiFitnessResumeTracking"},
        {0x835D98C8, kLiR3_Neg1, kBlr, "NuiFitnessStopTracking"},
        {0x83901464, kLiR3_Neg1, kBlr, "NuiFitnessGetCurrentFitnessData"},

        // Wave gesture (nuiwave.obj)
        {0x835D905C, kLiR3_Neg1, kBlr, "NuiWaveSetEnabled"},
        {0x835D8F6C, kLiR3_Neg1, kBlr, "NuiWaveGetGestureOwnerProgress"},

        // Head tracking (nuiheadposition.obj, nuiheadorientation.obj)
        {0x83293DE0, kLiR3_0, kBlr, "NuiHeadPositionDisable"},
        {0x835E18FC, kLiR3_0, kBlr, "NuiHeadOrientationDisable"},

        // Speech (xspeechapi.obj)
        {0x832C6B88, kLiR3_0, kBlr, "NuiSpeechEnable"},
        {0x832C5B90, kLiR3_0, kBlr, "NuiSpeechDisable"},
        {0x832C5BCC, kLiR3_0, kBlr, "NuiSpeechCreateGrammar"},
        {0x832C5B9C, kLiR3_0, kBlr, "NuiSpeechLoadGrammar"},
        {0x832C5BBC, kLiR3_0, kBlr, "NuiSpeechUnloadGrammar"},
        {0x832C4A7C, kLiR3_0, kBlr, "NuiSpeechCommitGrammar"},
        {0x832C30E8, kLiR3_0, kBlr, "NuiSpeechStartRecognition"},
        {0x832C49B8, kLiR3_0, kBlr, "NuiSpeechStopRecognition"},
        {0x832C3110, kLiR3_0, kBlr, "NuiSpeechSetEventInterest"},
        {0x832C30F8, kLiR3_0, kBlr, "NuiSpeechSetGrammarState"},
        {0x832C49D8, kLiR3_0, kBlr, "NuiSpeechSetRuleState"},
        {0x832C49F4, kLiR3_0, kBlr, "NuiSpeechCreateRule"},
        {0x832C4A18, kLiR3_0, kBlr, "NuiSpeechCreateState"},
        {0x832C4A34, kLiR3_0, kBlr, "NuiSpeechAddWordTransition"},
        {0x832C3120, kLiR3_Neg1, kBlr, "NuiSpeechGetEvents"},
        {0x832C49C8, kLiR3_0, kBlr, "NuiSpeechDestroyEvent"},
        {0x832C6AB4, kLiR3_0, kBlr, "NuiSpeechEmulateRecognition"},

        // SmartGlass (XBC) - (xbcimpl.obj)
        {0x8352C7A4, kLiR3_0, kBlr, "CXbcImpl::Initialize"},
        {0x8352C0AC, kLiR3_0, kBlr, "CXbcImpl::DoWork"},
        {0x8352C530, kLiR3_0, kBlr, "CXbcImpl::SendJSON"},

        // D3D NUI (nui.obj)
        {0x837948A8, kLiR3_0, kBlr, "D3DDevice_NuiInitialize"},
        {0x8378DC78, kLiR3_0, kBlr, "D3DDevice_NuiMetaData"},
        {0x83794920, kLiR3_0, kBlr, "D3DDevice_NuiStart"},
        {0x83794964, kLiR3_0, kBlr, "D3DDevice_NuiStop"},

        // Internal NUI (Nuip*) from manifest
        {0x835E2FE4, kLiR3_0, kBlr, "NuipBuildXamNuiFrameData"},
        {0x835CDF3C, kLiR3_0, kBlr, "NuipCameraGetExposureRegionOfInterest"},
        {0x835CE548, kLiR3_0, kBlr, "NuipCameraGetProperty"},
        {0x835CE850, kLiR3_0, kBlr, "NuipCameraGetPropertyF"},
        {0x8363A7B4, kLiR3_0, kBlr, "NuipCreateInstance"},
        {0x835D92EC, kLiR3_0, kBlr, "NuipFitnessInitialize"},
        {0x835D9B98, kLiR3_0, kBlr, "NuipFitnessNewSkeletalFrame"},
        {0x835D93C8, kLiR3_0, kBlr, "NuipFitnessShutdown"},
        {0x835D8120, kLiR3_0, kBlr, "NuipInitialize"},
        {0x83641870, kLiR3_0, kBlr, "NuipLoadRegistry"},
        {0x8363A844, kLiR3_0, kBlr, "NuipModuleInit"},
        {0x8363A9C0, kLiR3_0, kBlr, "NuipModuleTerm"},
        {0x83641650, kLiR3_0, kBlr, "NuipRegCreateKeyExW"},
        {0x83640328, kLiR3_0, kBlr, "NuipRegEnumKeyExW"},
        {0x83640420, kLiR3_0, kBlr, "NuipRegEnumValueW"},
        {0x836413E0, kLiR3_0, kBlr, "NuipRegOpenKeyExW"},
        {0x83640A58, kLiR3_0, kBlr, "NuipRegQueryValueExW"},
        {0x83641020, kLiR3_0, kBlr, "NuipRegSetValueExW"},
        {0x83640964, kLiR3_0, kBlr, "NuipUnloadRegistry"},
        {0x835D8DF8, kLiR3_0, kBlr, "NuipWaveInit"},
        {0x835D8E60, kLiR3_0, kBlr, "NuipWaveUpdate"},
    };

    // Log a stable .text fingerprint to support future resolver matching.
    Dc3TextSectionInfo text_info;
    if (auto* xex = module->xex_module()) {
      if (auto* text = xex->GetPESection(".text")) {
        auto* text_mem = memory_->TranslateVirtual<uint8_t*>(text->address);
        text_info.start = text->address;
        text_info.end = text->address + text->size;
        text_info.have_range = text->size != 0;
        if (text_mem && text->size) {
          uint64_t hash = UINT64_C(1469598103934665603);
          for (uint32_t i = 0; i < text->size; ++i) {
            hash ^= text_mem[i];
            hash *= UINT64_C(1099511628211);
          }
          text_info.fingerprint = hash;
          text_info.have_fingerprint = true;
          XELOGI("DC3: .text fingerprint addr={:08X} size=0x{:X} fnv1a64={:016X}",
                 text->address, text->size, hash);
        }
      }
    }

    // Two-pass approach: first check how many patch targets have zero-padding.
    // If many do, we're running a decomp/rebuilt XEX with different function
    // layout, and ALL patches use wrong addresses. In the original retail XEX,
    // no NUI function address would be zero-filled.
    int total_patches = static_cast<int>(sizeof(patches) / sizeof(patches[0]));
    int zero_count = 0;
    for (const auto& patch : patches) {
      auto* mem = memory_->TranslateVirtual<uint8_t*>(patch.address);
      if (mem && xe::load_and_swap<uint32_t>(mem) == 0x00000000) {
        zero_count++;
      }
    }

    bool is_decomp_layout = (zero_count > total_patches / 4);
    std::string_view layout_reason = "zero-padding heuristic";
    // Reuse manifest loaded earlier (before NUI block).
    auto& patch_manifest = dc3_patch_manifest;
    const bool explicit_patch_manifest_path =
        !cvars::dc3_nui_patch_manifest_path.empty();
    std::optional<Dc3FingerprintCache> fingerprint_cache;
    std::filesystem::path fingerprint_cache_path;
    if (!cvars::dc3_nui_layout_fingerprint_cache_path.empty()) {
      fingerprint_cache_path = cvars::dc3_nui_layout_fingerprint_cache_path;
    } else if (auto auto_cache_path = Dc3AutoProbeFingerprintCachePath()) {
      fingerprint_cache_path = *auto_cache_path;
    }
    if (!fingerprint_cache_path.empty()) {
      fingerprint_cache = Dc3LoadFingerprintCacheFile(fingerprint_cache_path);
      if (!fingerprint_cache) {
        XELOGW("DC3: Failed to load fingerprint cache file '{}'",
               xe::path_to_utf8(fingerprint_cache_path));
      } else {
        XELOGI("DC3: Loaded fingerprint cache '{}'",
               xe::path_to_utf8(fingerprint_cache_path));
      }
    }
    if (cvars::dc3_nui_patch_layout == "original") {
      is_decomp_layout = false;
      layout_reason = "forced by --dc3_nui_patch_layout=original";
    } else if (cvars::dc3_nui_patch_layout == "decomp") {
      is_decomp_layout = true;
      layout_reason = "forced by --dc3_nui_patch_layout=decomp";
    } else if (cvars::dc3_nui_patch_layout == "auto") {
      bool matched_manifest_layout = false;
      if (patch_manifest &&
          (patch_manifest->build_label == "decomp" ||
           patch_manifest->build_label == "original")) {
        const std::optional<uint64_t> manifest_runtime_fp =
            patch_manifest->runtime_text_fingerprint;
        const std::optional<uint64_t> manifest_compare_fp =
            manifest_runtime_fp.has_value() ? manifest_runtime_fp
                                            : patch_manifest->text_fingerprint;
        if (text_info.have_fingerprint && manifest_compare_fp.has_value()) {
          if (*manifest_compare_fp == text_info.fingerprint) {
            if (patch_manifest->build_label == "decomp") {
              is_decomp_layout = true;
              layout_reason =
                  "matched patch manifest fingerprint/build_label=decomp";
            } else {
              is_decomp_layout = false;
              layout_reason =
                  "matched patch manifest fingerprint/build_label=original";
            }
            matched_manifest_layout = true;
          } else if (explicit_patch_manifest_path) {
            XELOGW(
                "DC3: Patch manifest fingerprint {:016X} != runtime .text "
                "fingerprint {:016X}; trusting explicit manifest build_label={}",
                *manifest_compare_fp, text_info.fingerprint,
                patch_manifest->build_label);
            is_decomp_layout = patch_manifest->build_label == "decomp";
            layout_reason =
                "trusted explicit patch manifest build_label (fingerprint mismatch)";
            matched_manifest_layout = true;
          }
        } else if (explicit_patch_manifest_path) {
          XELOGW(
              "DC3: Patch manifest missing comparable fingerprint; trusting "
              "explicit manifest build_label={}",
              patch_manifest->build_label);
          is_decomp_layout = patch_manifest->build_label == "decomp";
          layout_reason =
              "trusted explicit patch manifest build_label (no fingerprint)";
          matched_manifest_layout = true;
        }
      }
      uint64_t fp_original = 0;
      uint64_t fp_decomp = 0;
      bool have_fp_original = Dc3TryParseHexU64(
          cvars::dc3_nui_layout_fingerprint_original, &fp_original);
      bool have_fp_decomp = Dc3TryParseHexU64(
          cvars::dc3_nui_layout_fingerprint_decomp, &fp_decomp);
      if (!have_fp_original && fingerprint_cache &&
          fingerprint_cache->original.has_value()) {
        fp_original = *fingerprint_cache->original;
        have_fp_original = true;
      }
      if (!have_fp_decomp && fingerprint_cache &&
          fingerprint_cache->decomp.has_value()) {
        fp_decomp = *fingerprint_cache->decomp;
        have_fp_decomp = true;
      }
      if (!matched_manifest_layout && text_info.have_fingerprint && have_fp_original &&
          text_info.fingerprint == fp_original) {
        is_decomp_layout = false;
        layout_reason = "matched --dc3_nui_layout_fingerprint_original";
      } else if (!matched_manifest_layout && text_info.have_fingerprint &&
                 have_fp_decomp &&
                 text_info.fingerprint == fp_decomp) {
        is_decomp_layout = true;
        layout_reason = "matched --dc3_nui_layout_fingerprint_decomp";
      } else if (!matched_manifest_layout && text_info.have_fingerprint &&
                 (have_fp_original || have_fp_decomp)) {
        const std::string fp_original_str = have_fp_original
                                                ? fmt::format("{:016X}", fp_original)
                                                : std::string("<unset>");
        const std::string fp_decomp_str = have_fp_decomp
                                              ? fmt::format("{:016X}", fp_decomp)
                                              : std::string("<unset>");
        XELOGI(
            "DC3: .text fingerprint {:016X} did not match configured layout "
            "fingerprints (original={} decomp={})",
            text_info.fingerprint, fp_original_str, fp_decomp_str);
      }
    } else {
      XELOGW(
          "DC3: Invalid --dc3_nui_patch_layout='{}' (expected auto|original|decomp); "
          "falling back to auto heuristic",
          cvars::dc3_nui_patch_layout);
    }
    XELOGI(
        "DC3: NUI patch layout={} reason={} (zero-padding {}/{})",
        is_decomp_layout ? "decomp" : "original", layout_reason, zero_count,
        total_patches);
    dc3_is_decomp_layout = is_decomp_layout;

    // Select the appropriate patch table based on XEX layout.
    const Dc3NuiPatchSpec* active_patches =
        is_decomp_layout ? decomp_patches : patches;
    int active_count = is_decomp_layout
                           ? static_cast<int>(sizeof(decomp_patches) /
                                              sizeof(decomp_patches[0]))
                           : total_patches;

    std::optional<Dc3NuiSymbolManifest> symbol_manifest;
    std::filesystem::path symbol_manifest_path;
    if (!cvars::dc3_nui_symbol_map_path.empty()) {
      symbol_manifest_path = cvars::dc3_nui_symbol_map_path;
    } else if (auto auto_path = Dc3AutoProbeNuiSymbolMapPath()) {
      symbol_manifest_path = *auto_path;
    }
    if (!symbol_manifest_path.empty()) {
      symbol_manifest = Dc3LoadNuiSymbolManifest(symbol_manifest_path);
      if (symbol_manifest) {
        XELOGI("DC3: Loaded NUI symbol manifest '{}' ({} .text symbols)",
               xe::path_to_utf8(symbol_manifest_path),
               symbol_manifest->text_symbols.size());
      } else {
        XELOGW("DC3: Failed to load NUI symbol manifest '{}'",
               xe::path_to_utf8(symbol_manifest_path));
      }
    }

    bool use_patch_manifest_targets = patch_manifest.has_value();
    if (patch_manifest && text_info.have_fingerprint) {
      const std::optional<uint64_t> manifest_runtime_fp =
          patch_manifest->runtime_text_fingerprint;
      const std::optional<uint64_t> manifest_compare_fp =
          manifest_runtime_fp.has_value() ? manifest_runtime_fp
                                          : patch_manifest->text_fingerprint;
      if (manifest_compare_fp.has_value() &&
          *manifest_compare_fp != text_info.fingerprint) {
        XELOGW(
            "DC3: Disabling patch manifest target resolution due fingerprint "
            "mismatch (manifest {:016X} != runtime {:016X}); "
            "falling back to symbol/signature/catalog",
            *manifest_compare_fp, text_info.fingerprint);
        use_patch_manifest_targets = false;
      }
    }

    std::string resolver_mode = cvars::dc3_nui_patch_resolver_mode;
    if (resolver_mode == "legacy") {
      XELOGW("DC3: --dc3_nui_patch_resolver_mode=legacy has been removed; "
             "using hybrid");
      resolver_mode = "hybrid";
    } else if (resolver_mode != "hybrid" && resolver_mode != "strict") {
      XELOGW("DC3: Unknown --dc3_nui_patch_resolver_mode='{}'; using hybrid",
             resolver_mode);
      resolver_mode = "hybrid";
    }
    Dc3RuntimeTelemetryConfig telemetry_config;
    telemetry_config.title_id = "373307D9";
    telemetry_config.build_kind = is_decomp_layout ? "decomp" : "original";
    telemetry_config.resolver_mode = resolver_mode;
    telemetry_config.signature_resolver = cvars::dc3_nui_enable_signature_resolver;
    telemetry_config.guest_overrides = true;
    Dc3RuntimeTelemetryBeginSession(telemetry_config);
    Dc3RuntimeTelemetryRecordBootMilestone("dc3_nui_patch_block_begin");

    std::vector<Dc3ResolvedNuiPatch> resolved_patches;
    resolved_patches.reserve(active_count);
    int resolved_by_manifest = 0;
    int resolved_by_symbol = 0;
    int resolved_by_signature = 0;
    int resolved_by_catalog = 0;
    int resolver_strict_rejects = 0;
    for (int i = 0; i < active_count; ++i) {
      auto resolved = Dc3ResolveNuiPatchTarget(active_patches[i], text_info,
                                               use_patch_manifest_targets
                                                   ? &*patch_manifest
                                                   : nullptr,
                                               symbol_manifest ? &*symbol_manifest
                                                               : nullptr,
                                               resolver_mode,
                                               memory_.get(),
                                               cvars::dc3_nui_enable_signature_resolver);
      if (!resolved.resolved && resolved.strict_rejected) {
        resolver_strict_rejects++;
      } else if (resolved.resolved) {
        if (resolved.resolve_method == Dc3PatchResolveMethod::kPatchManifest) {
          resolved_by_manifest++;
        } else if (resolved.resolve_method == Dc3PatchResolveMethod::kSymbolMap) {
          resolved_by_symbol++;
        } else if (resolved.resolve_method ==
                   Dc3PatchResolveMethod::kSignatureStub) {
          resolved_by_signature++;
        } else if (resolved.resolve_method ==
                   Dc3PatchResolveMethod::kCatalogAddress) {
          resolved_by_catalog++;
        }
      }
      resolved_patches.push_back(resolved);
    }
    XELOGI(
        "DC3: NUI resolver summary mode={} manifest_hits={} symbol_hits={} "
        "signature_hits={} catalog_hits={} strict_rejects={} total={}",
        resolver_mode, resolved_by_manifest,
        resolved_by_symbol, resolved_by_signature,
        resolved_by_catalog, resolver_strict_rejects, active_count);
    Dc3RuntimeTelemetryRecordNuiResolverSummary(
        resolver_mode, resolved_by_manifest, resolved_by_symbol,
        resolved_by_signature, resolved_by_catalog, resolver_strict_rejects,
        active_count);
    if (cvars::dc3_nui_signature_trace) {
      auto should_trace_signature_target = [](std::string_view name) {
        return !name.empty();
      };
      auto log_words = [&](const char* label, uint32_t address) {
        if (!Dc3PatchTargetInText(text_info, address, 4)) {
          XELOGI("DC3: SignatureTrace {} {:08X} (outside .text)", label, address);
          return;
        }
        auto* mem = memory_->TranslateVirtual<uint8_t*>(address);
        if (!mem) {
          XELOGI("DC3: SignatureTrace {} {:08X} (unmapped)", label, address);
          return;
        }
        std::string words;
        for (int j = 0; j < 12; ++j) {
          if (!Dc3PatchTargetInText(text_info, address + j * 4, 4)) {
            break;
          }
          const uint32_t w = xe::load_and_swap<uint32_t>(mem + j * 4);
          if (!words.empty()) {
            words.push_back(' ');
          }
          words += fmt::format("{:08X}", w);
        }
        XELOGI("DC3: SignatureTrace {} {:08X}: {}", label, address, words);
      };
      for (const auto& resolved_patch : resolved_patches) {
        const auto& patch = resolved_patch.spec;
        if (!should_trace_signature_target(patch.name)) {
          continue;
        }
        XELOGI("DC3: SignatureTrace target={} resolver={} resolved={} "
               "catalog={:08X} resolved_addr={:08X}",
               patch.name, Dc3PatchResolveMethodName(resolved_patch.resolve_method),
               resolved_patch.resolved ? 1 : 0, patch.address,
               resolved_patch.resolved ? resolved_patch.resolved_address : 0);
        log_words("catalog", patch.address);
        if (resolved_patch.resolved && resolved_patch.resolved_address != patch.address) {
          log_words("resolved", resolved_patch.resolved_address);
        }
      }
    }

    const bool requested_guest_overrides = cvars::dc3_guest_overrides;
    const bool enable_guest_overrides = true;
    if (!requested_guest_overrides) {
      XELOGW(
          "DC3: DC3 NUI/XBC legacy byte-patch path has been removed; "
          "forcing guest overrides on (rollback by reverting commit)");
    }
    XELOGI("DC3: NUI/XBC apply path guest_overrides={} resolver_mode={} "
           "signature_resolver={}",
           enable_guest_overrides ? 1 : 0, resolver_mode,
           cvars::dc3_nui_enable_signature_resolver ? 1 : 0);

    processor_->ClearGuestFunctionOverrides();
    auto guest_extern_handler_for_patch =
        [&](const Dc3NuiPatchSpec& patch) -> cpu::GuestFunction::ExternHandler {
      if (!enable_guest_overrides) {
        return nullptr;
      }
      // Preserve the original-layout fake skeleton path when enabled.
      if (cvars::fake_kinect_data && !is_decomp_layout &&
          std::string_view(patch.name) == "NuiSkeletonGetNextFrame") {
        return Dc3NuiSequencerExtern;
      }
      if (patch.insn1 != kBlr) {
        return nullptr;
      }
      if (patch.insn0 == kLiR3_0) {
        return Dc3NuiReturnOkExtern;
      }
      if (patch.insn0 == kLiR3_Neg1) {
        return Dc3NuiReturnNeg1Extern;
      }
      if (patch.insn0 == kLiR3_1) {
        return Dc3NuiReturn1Extern;
      }
      return nullptr;
    };
    auto patch_target_in_text = [&](uint32_t address) -> bool {
      return Dc3PatchTargetInText(text_info, address);
    };
    int override_registered = 0;
    int override_unsupported = 0;
    int override_register_failed = 0;
    int override_register_non_text = 0;
    int override_register_unresolved = 0;
    for (int i = 0; i < active_count; i++) {
      const auto& resolved_patch = resolved_patches[i];
      const auto& patch = resolved_patch.spec;
      if (!resolved_patch.resolved) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(unresolved target; resolver mode={})",
            patch.address, patch.name, resolver_mode);
        override_register_unresolved++;
        override_register_failed++;
        continue;
      }
      const uint32_t patch_addr = resolved_patch.resolved_address;
            if (std::string_view(patch.name) == "NuiSkeletonGetNextFrame") {
        auto* h = guest_extern_handler_for_patch(patch);
        if (h) {
          processor_->RegisterGuestFunctionOverride(patch_addr, h, patch.name);
          XELOGI("DC3: ULTRA FORCED registration of NUI sequencer at {:08X}", patch_addr);
          override_registered++;
          continue;
        }
      }
      auto handler = guest_extern_handler_for_patch(patch);
      if (!handler) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(unsupported patch shape for override; legacy byte patch path "
            "removed)",
            patch_addr, patch.name);
        override_unsupported++;
        override_register_failed++;
        continue;
      }
      if (!patch_target_in_text(patch_addr)) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(outside .text range {:08X}-{:08X})",
            patch_addr, patch.name, text_info.start, text_info.end);
        override_register_non_text++;
        override_register_failed++;
        continue;
      }
      auto* heap = memory_->LookupHeap(patch_addr);
      auto* mem = memory_->TranslateVirtual<uint8_t*>(patch_addr);
      if (!heap || !mem) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(invalid guest address)",
            patch_addr, patch.name);
        override_register_failed++;
        continue;
      }
      uint32_t existing0 = xe::load_and_swap<uint32_t>(mem + 0);
      if (existing0 == 0x00000000) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(zero-filled target)",
            patch_addr, patch.name);
        override_register_failed++;
        continue;
      }
      processor_->RegisterGuestFunctionOverride(patch_addr, handler,
                                                std::string(patch.name));
      XELOGI("DC3: Registered guest extern override {:08X}: {} (resolver={})",
             patch_addr, patch.name,
             Dc3PatchResolveMethodName(resolved_patch.resolve_method));
      Dc3RuntimeTelemetryRecordNuiOverrideRegistered(
          patch.name, patch_addr,
          Dc3PatchResolveMethodName(resolved_patch.resolve_method));
      override_registered++;
    }
    XELOGI(
        "DC3: Registered {} guest extern overrides from NUI patch table "
        "({} entries not overridden, {} registration failures, "
        "{} outside .text, {} unresolved)",
        override_registered, active_count - override_registered,
        override_register_failed, override_register_non_text,
        override_register_unresolved);

    const int patched = 0;
    const int overridden = override_registered;
    const int skipped = active_count - overridden;
    XELOGI(
        "DC3: NUI patch/override summary: patched={} overridden={} skipped={} "
        "total={} layout={} unsupported_override_entries={} "
        "override_registration_failures={} "
        "override_registration_non_text={} skipped_unresolved={} "
        "legacy_byte_patching_removed=1",
        patched, overridden, skipped, active_count,
        is_decomp_layout ? "decomp" : "original", override_unsupported,
        override_register_failed, override_register_non_text,
        override_register_unresolved);
    Dc3RuntimeTelemetryRecordNuiPatchSummary(
        patched, overridden, skipped, active_count,
        is_decomp_layout ? "decomp" : "original");
    Dc3RuntimeTelemetryRecordBootMilestone("dc3_nui_patch_apply_complete");

    // Fake Kinect skeleton data injection / SkeletonUpdate patches
    // are extracted into dc3_hack_pack for the same reason as the non-NUI
    // workarounds: keep emulator.cc orchestration-focused and preserve an
    // explicit retirement path.
    {
      Dc3HackContext dc3_skeleton_ctx;
      dc3_skeleton_ctx.memory = memory_.get();
      dc3_skeleton_ctx.processor = processor_.get();
      dc3_skeleton_ctx.module = module.get();
      dc3_skeleton_ctx.is_decomp_layout = is_decomp_layout;
#ifdef XE_HEADLESS_BUILD
      dc3_skeleton_ctx.is_headless = true;
#else
      dc3_skeleton_ctx.is_headless = false;
#endif
      auto skel_result = ApplyDc3SkeletonHackPack(dc3_skeleton_ctx);
      XELOGD("DC3: hack-pack category={} applied={} skipped={} failed={}",
             Dc3HackCategoryName(skel_result.category), skel_result.applied,
             skel_result.skipped, skel_result.failed);
    }
  }

  //
  // Fix CRT XapiCallThreadNotifyRoutines hang for the DC3 decomp XEX.
  //
  // The decomp's CRT has an uninitialized LIST_ENTRY at 0x83B14C3C
  // (XapiThreadNotifyRoutineList). On a real Xbox 360, this would be
  // statically initialized to point to itself (empty circular list). In
  // the decomp build, it contains garbage, causing
  // XapiCallThreadNotifyRoutines (0x82F51108) to iterate a corrupt list
  // and spin forever trying to call null callback pointers.
  //
  // Non-NUI DC3 workarounds (CRT/imports/debug/decomp runtime stopgaps) are
  // extracted into the DC3 hack pack module to keep emulator.cc orchestration-
  // only and make retirement tracking manageable.
  if (title_id_.has_value() && title_id_.value() == 0x373307D9 &&
      dc3_is_decomp_layout) {
    Dc3HackContext dc3_hack_ctx;
    dc3_hack_ctx.memory = memory_.get();
    dc3_hack_ctx.processor = processor_.get();
    dc3_hack_ctx.module = module.get();
    dc3_hack_ctx.is_decomp_layout = dc3_is_decomp_layout;
    if (dc3_patch_manifest.has_value()) {
      if (!dc3_patch_manifest->hack_pack_stubs.empty()) {
        dc3_hack_ctx.hack_pack_stubs = &dc3_patch_manifest->hack_pack_stubs;
        XELOGI("DC3: Passing {} hack-pack stub addresses from manifest",
               dc3_patch_manifest->hack_pack_stubs.size());
      }
      if (!dc3_patch_manifest->crt_sentinels.empty()) {
        dc3_hack_ctx.crt_sentinels = &dc3_patch_manifest->crt_sentinels;
        XELOGI("DC3: Passing {} CRT sentinel addresses from manifest",
               dc3_patch_manifest->crt_sentinels.size());
      }
      if (!dc3_patch_manifest->xdk_overrides.empty()) {
        dc3_hack_ctx.xdk_overrides = &dc3_patch_manifest->xdk_overrides;
        XELOGI("DC3: Passing {} XDK override addresses from manifest",
               dc3_patch_manifest->xdk_overrides.size());
      }
      if (!dc3_patch_manifest->xdk_code_ranges.empty()) {
        // Reinterpret-cast is safe: both CodeRange structs have identical
        // layout {uint32_t start; uint32_t end;}.
        dc3_hack_ctx.xdk_code_ranges = reinterpret_cast<
            const std::vector<Dc3HackContext::CodeRange>*>(
            &dc3_patch_manifest->xdk_code_ranges);
        XELOGI("DC3: Passing {} XDK code ranges for prologue scanning",
               dc3_patch_manifest->xdk_code_ranges.size());
      }
    }
    // Populate kAddr from manifest address catalog (before hack pack applies).
    if (dc3_patch_manifest.has_value() &&
        !dc3_patch_manifest->address_catalog.empty()) {
      Dc3PopulateAddressesFromCatalog(dc3_patch_manifest->address_catalog,
                                      dc3_patch_manifest->crt_sentinels);
    }
#ifdef XE_HEADLESS_BUILD
    dc3_hack_ctx.is_headless = true;
#else
    dc3_hack_ctx.is_headless = false;
#endif
    auto dc3_hack_summary = ApplyDc3HackPack(dc3_hack_ctx);
    for (const auto& result : dc3_hack_summary.results) {
      XELOGD("DC3: hack-pack category={} applied={} skipped={} failed={}",
             Dc3HackCategoryName(result.category), result.applied,
             result.skipped, result.failed);
    }
    if (cvars::dc3_ik_telemetry) {
      auto ik_result = ApplyDc3IKTelemetry(dc3_hack_ctx);
      XELOGI("DC3: IK telemetry: applied={} skipped={} failed={}",
             ik_result.applied, ik_result.skipped, ik_result.failed);
    }
    Dc3RuntimeTelemetryRecordBootMilestone("dc3_hack_pack_apply_complete");
  } else if (title_id_.has_value() && title_id_.value() == 0x373307D9) {
    XELOGI(
        "DC3: Skipping hack pack for original XEX (NUI overrides already applied)");
    XELOGI("DC3: Original-XEX boot patch staging begins "
           "(fake_kinect_data={} decomp_layout={})",
           cvars::fake_kinect_data, dc3_is_decomp_layout);
    auto with_patch_target =
        [&](const char* label, uint32_t addr, size_t size, auto&& apply) {
          auto* ptr = memory_->TranslateVirtual<uint8_t*>(addr);
          if (!ptr) {
            XELOGW("DC3: Patch lookup failed: {} at {:08X} "
                   "(TranslateVirtual returned null)",
                   label, addr);
            return false;
          }
          auto* heap = memory_->LookupHeap(addr);
          if (!heap) {
            XELOGW("DC3: Patch lookup failed: {} at {:08X} "
                   "(LookupHeap returned null)",
                   label, addr);
            return false;
          }
          heap->Protect(addr, size, kMemoryProtectRead | kMemoryProtectWrite);
          apply(ptr);
          return true;
        };

    constexpr uint32_t kSaveLoadManagerActivate = 0x82894A10;
    with_patch_target("SaveLoadManager::Activate", kSaveLoadManagerActivate, 4,
                      [&](uint8_t* sla_ptr) {
                        xe::store_and_swap<uint32_t>(sla_ptr, 0x4E800020);
                        XELOGI(
                            "DC3: Stubbed SaveLoadManager::Activate at {:08X} to blr",
                            kSaveLoadManagerActivate);
                      });

    constexpr uint32_t kHamPanelFocusComponent = 0x828EFE90;
    constexpr uint32_t kUIPanelFocusComponent = 0x827A6310;
    with_patch_target("HamPanel::FocusComponent", kHamPanelFocusComponent, 4,
                      [&](uint8_t* ptr) {
                        constexpr uint32_t kBranchMask = 0x03FFFFFC;
                        uint32_t branch =
                            0x48000000 |
                            ((kUIPanelFocusComponent - kHamPanelFocusComponent) &
                             kBranchMask);
                        xe::store_and_swap<uint32_t>(ptr, branch);
                        XELOGI("DC3: UI fix: redirected HamPanel::FocusComponent "
                               "at {:08X} to UIPanel::FocusComponent {:08X}",
                               kHamPanelFocusComponent, kUIPanelFocusComponent);
                      });

    constexpr uint32_t kHamScreenIsEventDialogOnTop = 0x829626D8;
    with_patch_target("HamScreen::IsEventDialogOnTop",
                      kHamScreenIsEventDialogOnTop, 8, [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr + 0, 0x38600000);
                        xe::store_and_swap<uint32_t>(ptr + 4, 0x4E800020);
                        XELOGI("DC3: UI fix: stubbed HamScreen::IsEventDialogOnTop "
                               "at {:08X} to return false",
                               kHamScreenIsEventDialogOnTop);
                      });

    constexpr uint32_t kCDReadDone = 0x826026E0;
    with_patch_target("CDReadDone", kCDReadDone, 8, [&](uint8_t* cdr_ptr) {
      xe::store_and_swap<uint32_t>(cdr_ptr + 0, 0x38600001);
      xe::store_and_swap<uint32_t>(cdr_ptr + 4, 0x4E800020);
      XELOGI("DC3: Stubbed CDReadDone at {:08X} to return true",
             kCDReadDone);
    });

    constexpr uint32_t kContentMgrRefreshDone = 0x825FEB48;
    with_patch_target("ContentMgr::RefreshDone", kContentMgrRefreshDone, 8,
                      [&](uint8_t* crd_ptr) {
                        xe::store_and_swap<uint32_t>(crd_ptr + 0, 0x38600001);
                        xe::store_and_swap<uint32_t>(crd_ptr + 4, 0x4E800020);
                        XELOGI("DC3: Stubbed ContentMgr::RefreshDone at {:08X} "
                               "to return true",
                               kContentMgrRefreshDone);
                      });

    constexpr uint32_t kSplashPrepareNext = 0x82554388;
    with_patch_target("Splash::PrepareNext", kSplashPrepareNext, 8,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr + 0, 0x38600000);
                        xe::store_and_swap<uint32_t>(ptr + 4, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::PrepareNext "
                               "at {:08X} to return false",
                               kSplashPrepareNext);
                      });

    constexpr uint32_t kSplashBeginSplasher = 0x825554C8;
    with_patch_target("Splash::BeginSplasher", kSplashBeginSplasher, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::BeginSplasher "
                               "at {:08X} to blr",
                               kSplashBeginSplasher);
                      });

    constexpr uint32_t kSplashSuspend = 0x82553BE0;
    with_patch_target("Splash::Suspend", kSplashSuspend, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::Suspend "
                               "at {:08X} to blr",
                               kSplashSuspend);
                      });

    constexpr uint32_t kSplashResume = 0x82553D68;
    with_patch_target("Splash::Resume", kSplashResume, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::Resume "
                               "at {:08X} to blr",
                               kSplashResume);
                      });

    constexpr uint32_t kSpeechGrammarUnload = 0x82439F38;
    with_patch_target("SpeechMgr::Grammar::Unload", kSpeechGrammarUnload, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Speech fix: stubbed "
                               "SpeechMgr::Grammar::Unload at {:08X} to blr",
                               kSpeechGrammarUnload);
                      });

    constexpr uint32_t kMovieInit = 0x82555678;
    // Intentionally NOT stubbed. Movie::Init() is just `{ TheMovieSys.Init(); }`,
    // the boot's entry into movie-system init. The old `blr` stub here was the
    // root of the regression: it skipped TheMovieSys.Init() entirely, so
    // BinkMovieSys::Init never ran, isInitalized stayed false, and the guest's
    // MILO_ASSERT(TheMovieSys.IsInitialized()) in Movie::BeginFromFile (line 220)
    // fired fatally on the attract movie. Letting Movie::Init run now dispatches
    // to BinkMovieSys::Init (patched below to set isInitalized=1 and return
    // before the hanging BinkStartAsyncThread), which is the whole point.
    XELOGI("DC3: Movie bypass: leaving Movie::Init at {:08X} intact so it "
           "calls TheMovieSys.Init() (BinkMovieSys::Init patched to set flag)",
           kMovieInit);

    constexpr uint32_t kBinkMovieSysInit = 0x82E214A8;
    // Set TheMovieSys.isInitalized = true instead of a bare blr. The old blr
    // stub skipped MovieSys::Init() entirely, so isInitalized stayed false and
    // the guest's MILO_ASSERT(TheMovieSys.IsInitialized()) (Movie.cpp:220)
    // fired fatally at boot, freezing the whole game on the debug error screen.
    // MovieSys layout: vptr @0, isInitalized(bool) @4; r3 == this (BinkMovieSys
    // base coincides with the MovieSys base). We still skip BinkStartAsyncThread
    // (the part that hangs headless and the reason Init was stubbed at all).
    with_patch_target("BinkMovieSys::Init", kBinkMovieSysInit, 12,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr + 0, 0x38000001);  // li  r0, 1
                        xe::store_and_swap<uint32_t>(ptr + 4, 0x98030004);  // stb r0, 4(r3)
                        xe::store_and_swap<uint32_t>(ptr + 8, 0x4E800020);  // blr
                        XELOGI("DC3: Movie bypass: BinkMovieSys::Init at {:08X} "
                               "now sets isInitalized=1 (was blr)",
                               kBinkMovieSysInit);
                      });

    if (cvars::fake_kinect_data) {
      XELOGI("DC3: Entering original-XEX fake Kinect patch block");

      constexpr uint32_t kSetPlayerPresentGuard = 0x8290834C;
      constexpr uint32_t kExpectedInsn = 0x4800001D;
      auto* guard_ptr =
          memory_->TranslateVirtual<uint8_t*>(kSetPlayerPresentGuard);
      if (guard_ptr) {
        uint32_t actual = xe::load_and_swap<uint32_t>(guard_ptr);
        if (actual == kExpectedInsn) {
          auto* heap = memory_->LookupHeap(kSetPlayerPresentGuard);
          if (heap) {
            heap->Protect(kSetPlayerPresentGuard, 4,
                          kMemoryProtectRead | kMemoryProtectWrite);
            xe::store_and_swap<uint32_t>(guard_ptr, 0x60000000);
            XELOGI("DC3: Calibration bypass: NOP'd IsTrackingAllSkeletons "
                   "guard in SetPlayerPresent at {:08X}",
                   kSetPlayerPresentGuard);
          }
        } else {
          XELOGW("DC3: Calibration bypass: unexpected insn at {:08X}: "
                 "{:08X} (expected {:08X})",
                 kSetPlayerPresentGuard, actual, kExpectedInsn);
        }
      }

      constexpr uint32_t kChoosePlayerSides = 0x82909968;
      with_patch_target("ChoosePlayerSides", kChoosePlayerSides, 4,
                        [&](uint8_t* cps_ptr) {
                          xe::store_and_swap<uint32_t>(cps_ptr, 0x4E800020);
                          XELOGI("DC3: Calibration bypass: stubbed "
                                 "ChoosePlayerSides at {:08X} to blr",
                                 kChoosePlayerSides);
                        });

      constexpr uint32_t kSetPlayerSkeletonWarningData = 0x82907880;
      with_patch_target("SetPlayerSkeletonWarningData",
                        kSetPlayerSkeletonWarningData, 4,
                        [&](uint8_t* spw_ptr) {
                          xe::store_and_swap<uint32_t>(spw_ptr, 0x4E800020);
                          XELOGI("DC3: Calibration bypass: stubbed "
                                 "SetPlayerSkeletonWarningData at {:08X} to blr",
                                 kSetPlayerSkeletonWarningData);
                        });

      constexpr uint32_t kSetPlayerSkeletonNavData = 0x82909340;
      constexpr uint32_t kSetPlayerPresent = 0x82908320;
      auto* nav_ptr =
          memory_->TranslateVirtual<uint8_t*>(kSetPlayerSkeletonNavData);
      if (nav_ptr) {
        auto* heap = memory_->LookupHeap(kSetPlayerSkeletonNavData);
        if (heap) {
          heap->Protect(kSetPlayerSkeletonNavData, 64,
                        kMemoryProtectRead | kMemoryProtectWrite);
          auto w = [nav_ptr](int idx, uint32_t insn) {
            xe::store_and_swap<uint32_t>(nav_ptr + idx * 4, insn);
          };
          int i = 0;
          w(i++, 0x7C0802A6);
          w(i++, 0x90010004);
          w(i++, 0x9421FFC0);
          w(i++, 0x38600000);
          w(i++, 0x38800001);
          w(i++, 0x48000001 | (kSetPlayerPresent - 0x82909350));
          w(i++, 0x38600001);
          w(i++, 0x38800001);
          w(i++, 0x48000001 | (kSetPlayerPresent - 0x82909358));
          w(i++, 0x38210040);
          w(i++, 0x80010004);
          w(i++, 0x7C0803A6);
          w(i++, 0x4E800020);
          XELOGI("DC3: Calibration bypass: replaced SetPlayerSkeletonNavData "
                 "at {:08X} with SetPlayerPresent stub ({} instructions)",
                 kSetPlayerSkeletonNavData, i);
        }
      }

      constexpr uint32_t kShouldWaitForRecovery = 0x82904CD0;
      with_patch_target("ShouldWaitForRecovery", kShouldWaitForRecovery, 8,
                        [&](uint8_t* swr_ptr) {
                          xe::store_and_swap<uint32_t>(swr_ptr + 0, 0x38600000);
                          xe::store_and_swap<uint32_t>(swr_ptr + 4, 0x4E800020);
                          XELOGI("DC3: Calibration bypass: stubbed "
                                 "ShouldWaitForRecovery at {:08X} to return false",
                                 kShouldWaitForRecovery);
                        });

      constexpr uint32_t kExitControllerMode = 0x82902748;
      with_patch_target("ExitControllerMode", kExitControllerMode, 4,
                        [&](uint8_t* ecm_ptr) {
                          xe::store_and_swap<uint32_t>(ecm_ptr, 0x4E800020);
                          XELOGI("DC3: Controller bypass: stubbed "
                                 "ExitControllerMode at {:08X} to blr",
                                 kExitControllerMode);
                        });

      // Blocker A (early auto-pause during gameplay): with --fake_kinect_data,
      // the synthetic skeleton is never registered as a "playing" player, so
      // Game::CheckForSkeletonLoss() sees numPlaying(0) < threshold(1) every
      // SkeletonUpdate and calls Game::PauseForSkeletonLoss() ~2s into the song
      // -> Handle(pause_game) -> perform_pause_screen, killing playback.
      // Confirmed root cause: PAUSE-ONSET DIAG showed the UIEventMgr dialog
      // queue EMPTY at the pause onset (qsize=0), ruling out GamePanel::Poll's
      // HasActiveDialogEvent() branch and pinning it on the skeleton-loss path.
      // Real Kinect would mark the player present and this never fires; under
      // fake input it's a false positive. Stub the void Game::PauseForSkeletonLoss
      // (private, non-virtual; 0x82866D50) to a bare blr so the song keeps
      // playing. Scoped to the fake-Kinect block since that's the only case that
      // produces the false skeleton loss. The player-count computation in
      // CheckForSkeletonLoss is left intact; only the pause action is removed.
      constexpr uint32_t kPauseForSkeletonLoss = 0x82866D50;
      with_patch_target("Game::PauseForSkeletonLoss", kPauseForSkeletonLoss, 4,
                        [&](uint8_t* pfsl_ptr) {
                          xe::store_and_swap<uint32_t>(pfsl_ptr, 0x4E800020);
                          XELOGI("DC3: Gameplay fix: stubbed "
                                 "Game::PauseForSkeletonLoss at {:08X} to blr "
                                 "(suppress fake-Kinect false skeleton-loss "
                                 "auto-pause)",
                                 kPauseForSkeletonLoss);
                        });

      constexpr uint32_t kMoviePoll = 0x82555CB8;
      with_patch_target("Movie::Poll", kMoviePoll, 8,
                        [&](uint8_t* mp_ptr) {
                          xe::store_and_swap<uint32_t>(mp_ptr + 0, 0x38600000);
                          xe::store_and_swap<uint32_t>(mp_ptr + 4, 0x4E800020);
                          XELOGI("DC3: Movie bypass: stubbed Movie::Poll at "
                                 "{:08X} to return false",
                                 kMoviePoll);
                        });

      auto patch4 = [&](uint32_t addr, uint32_t val, const char* desc) {
        auto* p = memory_->TranslateVirtual<uint8_t*>(addr);
        if (!p) {
          return;
        }
        auto* h = memory_->LookupHeap(addr);
        if (!h) {
          return;
        }
        h->Protect(addr, 4, kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(p, val);
        XELOGI("DC3: Audio fix: {} at {:08X}", desc, addr);
      };

      constexpr uint32_t kXMAHALAlloc = 0x82E77250;
      with_patch_target("XMAHALAllocateContexts", kXMAHALAlloc, 8,
                        [&](uint8_t* p) {
                          xe::store_and_swap<uint32_t>(p + 0, 0x38600000);
                          xe::store_and_swap<uint32_t>(p + 4, 0x4E800020);
                          XELOGI("DC3: Audio fix: stubbed XMAHALAllocateContexts "
                                 "at {:08X} to return S_OK",
                                 kXMAHALAlloc);
                        });

      patch4(0x82867288 + 0x90, 0x48000024,
             "HandleWait+0x90: bne 40820024 -> b 48000024");
      patch4(0x8252B9E0 + 0x70, 0x38600001,
             "HamAudio::IsReady+0x70: bctrl -> li r3,1");

      constexpr uint32_t kHamDirectorSongAnim = 0x82475578;
      with_patch_target("HamDirector::SongAnim", kHamDirectorSongAnim, 8,
                        [&](uint8_t* p) {
                          // SongAnim(playerIndex): force the pre-authored EXPERT
                          // song.anim (which has baked clip keyframes) instead of
                          // the routine-builder anim (empty headless — the
                          // remixer never runs). Mirrors the #ifdef HX_NATIVE
                          // fallback compiled out of debug.xex.
                          //   li r4,2 (kDifficultyExpert)
                          //   b  0x82473E58  (HamDirector::SongAnimByDifficulty)
                          // NOTE: branch MUST target the function ENTRY 0x82473E58
                          // (0x4BFFE8DC), NOT 0x82473E5C/+4 (0x4BFFE8E0). The +4
                          // target landed on the SongAnimByDifficulty survival
                          // patch's `blr`, skipping `li r3,0`, so SongAnim returned
                          // r3 unchanged == TheHamDirector -> ClipPlayer::Init then
                          // called GetKeys with this==HamDirector -> infinite hang.
                          // (Survival patch now removed; SongAnimByDifficulty runs
                          // its real `return mSongAnims[diff]` on the healthy map.)
                          xe::store_and_swap<uint32_t>(p + 0, 0x38800002);
                          xe::store_and_swap<uint32_t>(p + 4, 0x4BFFE8DC);
                          XELOGI("DC3: Anim fix: patched HamDirector::SongAnim "
                                 "at {:08X} to tail-call SongAnimByDifficulty"
                                 "(expert)",
                                 kHamDirectorSongAnim);
                        });
    }

    // Apply IK telemetry instrumentation to the original XEX if requested.
    if (cvars::dc3_ik_telemetry) {
      Dc3HackContext ik_ctx;
      ik_ctx.memory = memory_.get();
      ik_ctx.processor = processor_.get();
      ik_ctx.module = module.get();
      ik_ctx.is_decomp_layout = false;
      auto ik_result = ApplyDc3IKTelemetry(ik_ctx);
      XELOGI("DC3: IK telemetry (original XEX): applied={} skipped={} failed={}",
             ik_result.applied, ik_result.skipped, ik_result.failed);
    }
  }
  // RB3DX / RB3 TU5 (0x45410914): load the from-source RB3Enhanced.dll for the
  // same-instrument gameplay hooks (--si_load_dll), here at the stable
  // pre-LaunchModule point (same site as the DC3 .text patches) rather than from
  // a mid-boot guest-thread callback -- loading a module while the guest's heaps
  // are live races and crashes. LoadUserModule(call_entry=false) maps the DLL at
  // its preferred base 0x84000000. The hardcoded old-DLL host-emulated kHooks
  // table is RETIRED: with the from-source DLL the detours are installed by the
  // DLL's own InitSameInstrument (guest-thread call at --si_init_va, in the
  // __savegprlr_23 override), so the targets come from the DLL link map, not
  // constants. Here we only (1) map the DLL and (2) -- for behavioral runs --
  // poke config.AllowSameInstrument=1 at --si_force_allow_va, because
  // call_entry=false skips DllMain/ini load so the flag would otherwise stay 0
  // and the installed hooks would run pass-through. Title-gated + default-off =>
  // DC3-inert. Verify with --si_hook_verify.
  if (title_id_.has_value() && title_id_.value() == 0x45410914 &&
      cvars::si_load_dll) {
    auto dll = kernel_state_->LoadUserModule("game:\\RB3Enhanced.dll",
                                             /*call_entry=*/false);
    if (!dll) {
      XELOGE("SI LOADDLL: LoadUserModule(game:\\RB3Enhanced.dll) FAILED");
    } else {
      uint8_t* mb = memory_->virtual_membase();
      uint32_t dll_head = xe::load_and_swap<uint32_t>(mb + 0x84000000u);
      XELOGW("SI LOADDLL: RB3Enhanced.dll loaded entry=0x{:08X} is_dll={} "
             "head@0x84000000=0x{:08X}",
             dll->entry_point(), dll->is_dll_module(), dll_head);
      // The four SI detours are installed later, mid-boot, from the
      // __savegprlr_23 override (approach (a) guest-thread InitSameInstrument if
      // --si_init_va is set, else approach (b) host-emulated from-source detour
      // writes). A pre-LaunchModule write here does NOT stick -- the loader's
      // lazy .text commit reverts it -- so only the DLL map + allow-flag poke
      // happen at this site.
      if (cvars::si_init_va != 0) {
        XELOGW(
            "SI LOADDLL: from-source path (a) armed -- InitSameInstrument "
            "@0x{:08X} will run mid-boot on the guest thread (--si_init_va).",
            static_cast<uint32_t>(cvars::si_init_va));
      } else {
        XELOGW(
            "SI LOADDLL: from-source path (b) armed -- host-emulated detours "
            "(map hookVAs) will be written mid-boot from the __savegprlr_23 "
            "override.");
      }
      // (2) Force AllowSameInstrument=1 so the installed hook BODIES are live
      // (char field; single-byte poke). DllMain/ini never ran (call_entry=false)
      // so the flag is 0 in DLL .bss. REQUIRED for behavioral (Phase-5) runs.
      if (cvars::si_force_allow_va != 0) {
        uint32_t allow_va = static_cast<uint32_t>(cvars::si_force_allow_va);
        uint8_t* host_addr = mb + allow_va;
        const size_t host_pg = xe::memory::page_size();
        uint8_t* host_page = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(host_addr) & ~(host_pg - 1));
        xe::memory::Protect(host_page, host_pg,
                            xe::memory::PageAccess::kReadWrite);
        uint8_t before = *host_addr;
        *host_addr = 1u;  // config.AllowSameInstrument (char) = 1
        XELOGW(
            "SI LOADDLL: poked config.AllowSameInstrument @0x{:08X} {} -> 1 "
            "(--si_force_allow_va; hook bodies armed)",
            allow_va, before);
      } else {
        XELOGW(
            "SI LOADDLL: --si_force_allow_va is 0 -- config.AllowSameInstrument "
            "stays 0; hooks install but run PASS-THROUGH (install-only "
            "verification is fine; behavioral runs REQUIRE this flag).");
      }
    }
  }

  auto main_thread = kernel_state_->LaunchModule(module);
  if (!main_thread) {
    return X_STATUS_UNSUCCESSFUL;
  }
  main_thread_ = main_thread;
  on_launch(title_id_.value(), title_name_);

  return X_STATUS_SUCCESS;
}

}  // namespace xe
