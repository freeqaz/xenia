/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// TRANSITIONAL -- delete when titles/dc3/dc3_flags.cc defines these (see
// gpu_flags.h). Kept out of gpu_flags.{h,cc} so the hand-over is one `git rm`.

#ifndef XENIA_GPU_DC3_NUI_FLAGS_TRANSITIONAL_H_
#define XENIA_GPU_DC3_NUI_FLAGS_TRANSITIONAL_H_

#include "xenia/base/cvar.h"

DECLARE_bool(stub_nui_functions);
DECLARE_bool(fake_kinect_data);

#endif  // XENIA_GPU_DC3_NUI_FLAGS_TRANSITIONAL_H_
