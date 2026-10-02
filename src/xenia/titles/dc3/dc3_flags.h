/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 (title 373307D9) cvars moved out of emulator.cc (NOT upstream).
 *
 * Defined in dc3_flags.cc. Values, defaults and help text are
 * unchanged from emulator.cc.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_DC3_DC3_FLAGS_H_
#define XENIA_TITLES_DC3_DC3_FLAGS_H_

#include "xenia/base/cvar.h"

DECLARE_int32(dc3_crt_bisect_max);
DECLARE_string(dc3_crt_skip_indices);
DECLARE_bool(dc3_ik_telemetry);
DECLARE_bool(dc3_game_screen_real_goto);
DECLARE_bool(dc3_null_read_cache_stream);
DECLARE_bool(dc3_crt_skip_nui);
DECLARE_bool(dc3_debug_read_cache_stream_step_override);
DECLARE_bool(dc3_debug_mempool_alloc_probe);
DECLARE_string(dc3_debug_findarray_override_mode);
DECLARE_string(dc3_nui_patch_layout);
DECLARE_string(dc3_nui_layout_fingerprint_original);
DECLARE_string(dc3_nui_layout_fingerprint_decomp);
DECLARE_string(dc3_nui_layout_fingerprint_cache_path);
DECLARE_string(dc3_nui_symbol_map_path);
DECLARE_string(dc3_nui_patch_resolver_mode);
DECLARE_string(dc3_nui_patch_manifest_path);
DECLARE_bool(dc3_nui_enable_signature_resolver);
DECLARE_bool(dc3_nui_signature_trace);
DECLARE_bool(stub_nui_functions);
DECLARE_bool(fake_kinect_data);

#endif  // XENIA_TITLES_DC3_DC3_FLAGS_H_
