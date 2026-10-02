/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914) cvars (NOT upstream).
 *
 * Every cvar here is title-gated: it does nothing unless the running title is
 * 0x45410914. Defined in rb3_flags.cc.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_RB3_RB3_FLAGS_H_
#define XENIA_TITLES_RB3_RB3_FLAGS_H_

#include "xenia/base/cvar.h"

// Workarounds the documented RB3 recipes need (WORKSTREAM §8v-§8x).
DECLARE_bool(rb3_tu5_app_run_direct);
DECLARE_string(rb3_mogg_key_table);
DECLARE_bool(rb3_no_char_preview);
DECLARE_bool(rb3dx_offline_join);
DECLARE_bool(rb3dx_skip_calibration);

// Harness automation (injects pad presses; no guest writes).
DECLARE_bool(rb3dx_autoconfirm_parts);
DECLARE_int32(rb3dx_autoconfirm_p2_up);

// Read-only diagnostics.
DECLARE_bool(rb3dx_ui_probe);
DECLARE_bool(rb3_stream_census);
DECLARE_uint64(rb3dx_si_claim_anchor);
DECLARE_bool(si_hook_verify);

// MemAlloc/MemFree instrumentation (overrides of the __savegprlr_23/26 and
// __restgprlr_23 helpers; exact emulations, so guest-transparent).
DECLARE_bool(rb3dx_alloc_probe);
DECLARE_string(rb3dx_alloc_trace_path);
DECLARE_bool(rb3dx_free_trace);
DECLARE_bool(rb3dx_stack_trace);
DECLARE_bool(rb3dx_ret_trace);

// Same-instrument (RB3Enhanced.dll) harness.
DECLARE_bool(si_load_dll);
DECLARE_uint64(si_init_va);
DECLARE_uint64(si_force_allow_va);

#endif  // XENIA_TITLES_RB3_RB3_FLAGS_H_
