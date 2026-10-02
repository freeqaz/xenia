/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_GPU_FLAGS_H_
#define XENIA_GPU_GPU_FLAGS_H_
#include "xenia/base/cvar.h"

DECLARE_path(trace_gpu_prefix);
DECLARE_bool(trace_gpu_stream);

DECLARE_path(dump_shaders);

DECLARE_bool(vsync);

DECLARE_bool(gpu_allow_invalid_fetch_constants);

DECLARE_bool(non_seamless_cube_map);

DECLARE_bool(half_pixel_offset);

DECLARE_string(dump_frames_path);
DECLARE_int32(headless_capture_interval);

DECLARE_bool(force_all_draws);

DECLARE_bool(headless_verbose_diagnostics);

// TRANSITIONAL (fork cleanup, Lane E): stub_nui_functions / fake_kinect_data
// are DC3 cvars and leave the GPU layer. Lane B defines them in
// titles/dc3/dc3_flags.cc; when that lands, delete dc3_nui_flags_transitional.*
// and this include (the titles/dc3 TUs must then declare them themselves).
#include "xenia/gpu/dc3_nui_flags_transitional.h"

DECLARE_int32(query_occlusion_fake_sample_count);

#define XE_GPU_FINE_GRAINED_DRAW_SCOPES 1

#endif  // XENIA_GPU_GPU_FLAGS_H_
