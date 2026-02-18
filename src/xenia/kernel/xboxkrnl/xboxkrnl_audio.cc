/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_system.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

dword_result_t XAudioGetSpeakerConfig_entry(lpdword_t config_ptr) {
  *config_ptr = 0x00010001;
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioGetSpeakerConfig, kAudio, kImplemented);

dword_result_t XAudioGetVoiceCategoryVolumeChangeMask_entry(
    lpunknown_t driver_ptr, lpdword_t out_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  xe::threading::MaybeYield();

  // Checking these bits to see if any voice volume changed.
  // I think.
  *out_ptr = 0;
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetVoiceCategoryVolumeChangeMask, kAudio, kStub,
                         kHighFrequency);

dword_result_t XAudioGetVoiceCategoryVolume_entry(dword_t unk,
                                                  lpfloat_t out_ptr) {
  // Expects a floating point single. Volume %?
  *out_ptr = 1.0f;

  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetVoiceCategoryVolume, kAudio, kStub,
                         kHighFrequency);

dword_result_t XAudioEnableDucker_entry(dword_t unk) { return X_ERROR_SUCCESS; }
DECLARE_XBOXKRNL_EXPORT1(XAudioEnableDucker, kAudio, kStub);

// Track whether we have a real audio driver or a dummy one
static bool g_audio_driver_is_dummy = false;

dword_result_t XAudioRegisterRenderDriverClient_entry(lpdword_t callback_ptr,
                                                      lpdword_t driver_ptr) {
  uint32_t callback = callback_ptr[0];
  uint32_t callback_arg = callback_ptr[1];

  auto audio_system = kernel_state()->emulator()->audio_system();

  size_t index;
  auto result = audio_system->RegisterClient(callback, callback_arg, &index);
  if (XFAILED(result)) {
    // If the audio backend can't create a driver (e.g., nop backend),
    // return a dummy handle so the XAudio2 library can still initialize.
    // Without this, CX2SourceVoice::Initialize will spin forever.
    XELOGI("XAudioRegisterRenderDriverClient: CreateDriver failed ({:08X}), returning dummy handle", result);
    g_audio_driver_is_dummy = true;
    *driver_ptr = 0x41550000;
    return X_ERROR_SUCCESS;
  }

  assert_true(!(index & ~0x0000FFFF));
  *driver_ptr = 0x41550000 | (static_cast<uint32_t>(index) & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioRegisterRenderDriverClient, kAudio,
                         kImplemented);

dword_result_t XAudioUnregisterRenderDriverClient_entry(
    lpunknown_t driver_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  // Skip for dummy handles (nop audio backend)
  if (g_audio_driver_is_dummy) {
    return X_ERROR_SUCCESS;
  }

  auto audio_system = kernel_state()->emulator()->audio_system();
  audio_system->UnregisterClient(driver_ptr.guest_address() & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioUnregisterRenderDriverClient, kAudio,
                         kImplemented);

dword_result_t XAudioSubmitRenderDriverFrame_entry(lpunknown_t driver_ptr,
                                                   lpunknown_t samples_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  // Skip submit for dummy handles (nop audio backend)
  if (g_audio_driver_is_dummy) {
    return X_ERROR_SUCCESS;
  }

  auto audio_system = kernel_state()->emulator()->audio_system();
  audio_system->SubmitFrame(driver_ptr.guest_address() & 0x0000FFFF,
                            samples_ptr);

  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioSubmitRenderDriverFrame, kAudio, kImplemented,
                         kHighFrequency);

// Audio stubs for DC3
dword_result_t XAudioCaptureRenderDriverFrame_entry(lpunknown_t driver_ptr) {
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioCaptureRenderDriverFrame, kAudio, kStub);

dword_result_t XAudioGetRenderDriverTic_entry(lpunknown_t driver_ptr,
                                               lpdword_t tic_ptr) {
  // XAudio2 render driver runs at ~5ms intervals (200Hz).
  // Return a tic counter based on elapsed time so XAudio2 init doesn't spin.
  // The tic is a monotonically increasing counter that advances every ~5ms.
  static uint64_t start_time = 0;
  if (!start_time) {
    start_time = Clock::QueryHostUptimeMillis();
  }
  uint64_t elapsed = Clock::QueryHostUptimeMillis() - start_time;
  *tic_ptr = static_cast<uint32_t>(elapsed / 5);  // ~200Hz tic rate
  return 0;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetRenderDriverTic, kAudio, kImplemented,
                          kHighFrequency);

dword_result_t XAudioUnregisterRenderDriverMECClient_entry(
    lpunknown_t driver_ptr) {
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioUnregisterRenderDriverMECClient, kAudio,
                          kStub);

dword_result_t XAudioRegisterRenderDriverMECClient_entry(
    lpunknown_t driver_ptr, lpdword_t out_ptr) {
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioRegisterRenderDriverMECClient, kAudio, kStub);

dword_result_t XAudioOverrideSpeakerConfig_entry(dword_t config) { return 0; }
DECLARE_XBOXKRNL_EXPORT1(XAudioOverrideSpeakerConfig, kAudio, kStub);

dword_result_t XAudioGetDuckerLevel_entry(lpdword_t level_ptr) {
  *level_ptr = 0;
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioGetDuckerLevel, kAudio, kStub);

dword_result_t XAudioGetDuckerReleaseTime_entry(lpdword_t time_ptr) {
  *time_ptr = 0;
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioGetDuckerReleaseTime, kAudio, kStub);

dword_result_t XAudioGetDuckerAttackTime_entry(lpdword_t time_ptr) {
  *time_ptr = 0;
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioGetDuckerAttackTime, kAudio, kStub);

dword_result_t XAudioGetDuckerHoldTime_entry(lpdword_t time_ptr) {
  *time_ptr = 0;
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioGetDuckerHoldTime, kAudio, kStub);

dword_result_t XAudioGetDuckerThreshold_entry(lpdword_t threshold_ptr) {
  *threshold_ptr = 0;
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioGetDuckerThreshold, kAudio, kStub);

dword_result_t MicDeviceRequest_entry(unknown_t unk1, unknown_t unk2,
                                       unknown_t unk3) {
  return X_E_FAIL;
}
DECLARE_XBOXKRNL_EXPORT1(MicDeviceRequest, kAudio, kStub);

dword_result_t RmcDeviceRequest_entry(unknown_t unk1, unknown_t unk2,
                                       unknown_t unk3) {
  return X_E_FAIL;
}
DECLARE_XBOXKRNL_EXPORT1(RmcDeviceRequest, kAudio, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Audio);
