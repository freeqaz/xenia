/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2022 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*/

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

#ifndef XE_HEADLESS_BUILD
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#endif

namespace xe {
namespace kernel {
namespace xam {

extern std::atomic<int> xam_dialogs_shown_;

struct X_NUI_DEVICE_STATUS {
  xe::be<uint32_t> unk0;
  xe::be<uint32_t> unk1;
  xe::be<uint32_t> unk2;
  xe::be<uint32_t> status;
  xe::be<uint32_t> unk4;
  xe::be<uint32_t> unk5;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 24, "Size matters");

void XamNuiGetDeviceStatus_entry(pointer_t<X_NUI_DEVICE_STATUS> status_ptr) {
  static uint32_t nui_call_count = 0;
  if (++nui_call_count <= 10 || (nui_call_count % 500) == 0)
    XELOGI("XamNuiGetDeviceStatus called (count={}) - reporting connected",
           nui_call_count);
  status_ptr.Zero();
  status_ptr->status = 1;  // Report connected for DC3 Kinect init.
}
DECLARE_XAM_EXPORT1(XamNuiGetDeviceStatus, kNone, kStub);

dword_result_t XamShowNuiTroubleshooterUI_entry(unknown_t unk1, unknown_t unk2,
                                                unknown_t unk3) {
  XELOGI("XamShowNuiTroubleshooterUI called!");
  // unk1 is 0xFF - possibly user index?
  // unk2, unk3 appear to always be zero.

#ifdef XE_HEADLESS_BUILD
  // Headless: just return success
  return 0;
#else
  if (cvars::headless) {
    return 0;
  }

  const Emulator* emulator = kernel_state()->emulator();
  ui::Window* display_window = emulator->display_window();
  ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  if (display_window && imgui_drawer) {
    xe::threading::Fence fence;
    if (display_window->app_context().CallInUIThreadSynchronous([&]() {
          xe::ui::ImGuiDialog::ShowMessageBox(
              imgui_drawer, "NUI Troubleshooter",
              "The game has indicated there is a problem with NUI (Kinect).")
              ->Then(&fence);
        })) {
      ++xam_dialogs_shown_;
      fence.Wait();
      --xam_dialogs_shown_;
    }
  }

  return 0;
#endif  // XE_HEADLESS_BUILD
}
DECLARE_XAM_EXPORT1(XamShowNuiTroubleshooterUI, kNone, kStub);

// NUI Camera/Tilt stubs
dword_result_t XamNuiCameraElevationSetAngle_entry(unknown_t angle) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationSetAngle, kNone, kStub);

dword_result_t XamNuiCameraElevationGetAngle_entry(lpdword_t angle_ptr) {
  *angle_ptr = 0;
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationGetAngle, kNone, kStub);

dword_result_t XamNuiCameraElevationStopMovement_entry() { return 0; }
DECLARE_XAM_EXPORT1(XamNuiCameraElevationStopMovement, kNone, kStub);

dword_result_t XamNuiCameraTiltGetStatus_entry(lpdword_t status_ptr) {
  *status_ptr = 0;
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltGetStatus, kNone, kStub);

dword_result_t XamNuiCameraTiltReportStatus_entry(unknown_t unk1,
                                                   unknown_t unk2) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltReportStatus, kNone, kStub);

dword_result_t XamNuiCameraTiltSetCallback_entry(unknown_t callback) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltSetCallback, kNone, kStub);

dword_result_t XamNuiCameraRememberFloor_entry(unknown_t unk1) { return 0; }
DECLARE_XAM_EXPORT1(XamNuiCameraRememberFloor, kNone, kStub);

// NUI Identity/User stubs
dword_result_t XamNuiGetDeviceSerialNumber_entry(lpvoid_t buffer,
                                                  lpdword_t size_ptr) {
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiGetDeviceSerialNumber, kNone, kStub);

dword_result_t XamNuiIdentityGetSessionId_entry(lpdword_t session_id_ptr) {
  *session_id_ptr = 0;
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetSessionId, kNone, kStub);

dword_result_t XamUserNuiGetUserIndex_entry(unknown_t unk1,
                                             lpdword_t index_ptr) {
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndex, kNone, kStub);

dword_result_t XamUserNuiGetEnrollmentIndex_entry(unknown_t unk1,
                                                   lpdword_t index_ptr) {
  *index_ptr = 0;
  return 0;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetEnrollmentIndex, kNone, kStub);

dword_result_t XamUserNuiEnableBiometric_entry(unknown_t unk1,
                                                unknown_t unk2) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamUserNuiEnableBiometric, kNone, kStub);

// NUI Biometric stubs
dword_result_t XamReadBiometricData_entry(unknown_t unk1, unknown_t unk2,
                                           lpvoid_t buffer) {
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamReadBiometricData, kNone, kStub);

dword_result_t XamWriteBiometricData_entry(unknown_t unk1, unknown_t unk2,
                                            lpvoid_t buffer) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamWriteBiometricData, kNone, kStub);

// Cache stubs
dword_result_t XamCacheOpenFile_entry(unknown_t unk1, lpstring_t path,
                                       lpdword_t handle_ptr) {
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamCacheOpenFile, kNone, kStub);

dword_result_t XamCacheCloseFile_entry(unknown_t handle) { return 0; }
DECLARE_XAM_EXPORT1(XamCacheCloseFile, kNone, kStub);

dword_result_t XamCacheReset_entry() { return 0; }
DECLARE_XAM_EXPORT1(XamCacheReset, kNone, kStub);

// LRC stubs
dword_result_t XamLrcSetTitlePort_entry(unknown_t port) { return 0; }
DECLARE_XAM_EXPORT1(XamLrcSetTitlePort, kNone, kStub);

dword_result_t XamLrcVerifyClientId_entry(unknown_t client_id) { return 0; }
DECLARE_XAM_EXPORT1(XamLrcVerifyClientId, kNone, kStub);

dword_result_t XamLrcEncryptDecryptTitleMessage_entry(lpvoid_t buffer,
                                                       dword_t length,
                                                       unknown_t unk3) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamLrcEncryptDecryptTitleMessage, kNone, kStub);

// XLFS stubs
dword_result_t XamXlfsInitializeUploadQueue_entry(unknown_t unk1) { return 0; }
DECLARE_XAM_EXPORT1(XamXlfsInitializeUploadQueue, kNone, kStub);

dword_result_t XamXlfsUninitializeUploadQueue_entry() { return 0; }
DECLARE_XAM_EXPORT1(XamXlfsUninitializeUploadQueue, kNone, kStub);

dword_result_t XamXlfsMountUploadQueueInstance_entry(unknown_t unk1) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamXlfsMountUploadQueueInstance, kNone, kStub);

dword_result_t XamXlfsUnmountUploadQueueInstance_entry(unknown_t unk1) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamXlfsUnmountUploadQueueInstance, kNone, kStub);

// Misc XAM stubs
dword_result_t XamBackgroundDownloadSetMode_entry(dword_t mode) { return 0; }
DECLARE_XAM_EXPORT1(XamBackgroundDownloadSetMode, kNone, kStub);

dword_result_t XamXStudioRequest_entry(unknown_t unk1, unknown_t unk2,
                                        unknown_t unk3) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamXStudioRequest, kNone, kStub);

dword_result_t XamGetActiveDashAppInfo_entry(lpvoid_t info_ptr) {
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamGetActiveDashAppInfo, kNone, kStub);

dword_result_t XNetLogonGetTitleID_entry(lpdword_t title_id_ptr) {
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XNetLogonGetTitleID, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(NUI);
