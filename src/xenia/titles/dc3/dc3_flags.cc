/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 (title 373307D9) cvars moved out of emulator.cc (NOT upstream).
 *
 * Moved verbatim (Phase 1, FORK_CLEANUP_PLAN.md).
 ******************************************************************************
 */

#include "xenia/titles/dc3/dc3_flags.h"

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
