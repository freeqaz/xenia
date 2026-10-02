/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// TRANSITIONAL -- see dc3_nui_flags_transitional.h. Help text unchanged from
// the gpu_flags.cc definitions these replace.

#include "xenia/gpu/dc3_nui_flags_transitional.h"

DEFINE_bool(stub_nui_functions, false,
            "Stub NUI (Kinect SDK) functions in guest memory for DC3 debug "
            "builds. Writes PPC return-S_OK stubs at known NUI function "
            "addresses so the game boots without Kinect hardware.",
            "Headless");

DEFINE_bool(fake_kinect_data, false,
            "Provide synthetic Kinect skeleton data (T-pose) for DC3. "
            "Requires --stub_nui_functions. Enables game to detect a "
            "player and progress past the Kinect player detection screen.",
            "Headless");
