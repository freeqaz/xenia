/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914) cvars moved out of emulator.cc (NOT upstream).
 *
 * Defined in rb3_flags.cc. Values, defaults and help text are
 * unchanged from emulator.cc.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_RB3_RB3_FLAGS_H_
#define XENIA_TITLES_RB3_RB3_FLAGS_H_

#include "xenia/base/cvar.h"

DECLARE_bool(rb3_mount_update);
DECLARE_bool(rb3dx_alloc_probe);
DECLARE_string(rb3dx_alloc_trace_path);
DECLARE_bool(rb3dx_free_trace);
DECLARE_bool(rb3dx_stack_trace);
DECLARE_bool(rb3dx_ret_trace);
DECLARE_bool(rb3dx_ui_probe);
DECLARE_bool(rb3_loadmgr_unbudget);
DECLARE_bool(rb3_splash_unwedge);
DECLARE_bool(rb3_overlapped_scan);
DECLARE_string(rb3_mogg_key_table);
DECLARE_bool(rb3_stream_census);
DECLARE_bool(rb3_tu5_hash_poke);
DECLARE_bool(rb3_no_char_preview);
DECLARE_bool(rb3_tu5_app_run_direct);
DECLARE_uint64(rb3dx_si_claim_anchor);
DECLARE_bool(rb3dx_autoconfirm_parts);
DECLARE_int32(rb3dx_autoconfirm_p2_up);
DECLARE_bool(rb3dx_offline_join);
DECLARE_bool(rb3dx_skip_calibration);
DECLARE_bool(rb3dx_clamp_alloc);
DECLARE_bool(si_probe);
DECLARE_bool(si_selftest);
DECLARE_bool(si_hook_verify);
DECLARE_bool(si_load_dll);
DECLARE_uint64(si_init_va);
DECLARE_uint64(si_force_allow_va);
DECLARE_string(si_hook_vas);

#endif  // XENIA_TITLES_RB3_RB3_FLAGS_H_
