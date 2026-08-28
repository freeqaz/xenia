/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

using xe::hid::X_INPUT_CAPABILITIES;
using xe::hid::X_INPUT_KEYSTROKE;
using xe::hid::X_INPUT_STATE;
using xe::hid::X_INPUT_VIBRATION;

constexpr uint32_t XINPUT_FLAG_GAMEPAD = 0x01;
constexpr uint32_t XINPUT_FLAG_ANY_USER = 1 << 30;

// TEMP DIAG (clean-TU5 join gate, s64): the RB3 per-pad connect gate
// (TU5 0x82531f08) calls XamInputGetCapabilities successfully but still never
// marks the pad connected -- log the first calls of every other input-adjacent
// export with the guest caller LR so we can see what the gate tries next and
// what we fail. TODO(rb3): remove with the rest of the DIAGs.
static void LogInputDiag(const char* name, uint32_t a, uint32_t b, uint32_t r) {
  static std::atomic<uint32_t> s_calls{0};
  uint32_t n = s_calls.fetch_add(1, std::memory_order_relaxed);
  if (n >= 40) return;
  uint32_t lr = 0;
  auto* thread = XThread::GetCurrentThread();
  if (thread) lr = static_cast<uint32_t>(thread->thread_state()->context()->lr);
  XELOGE("XamInput DIAG#{}: {}(0x{:X}, 0x{:X}) -> 0x{:08X} lr=0x{:08X}", n,
         name, a, b, r, lr);
}

void XamResetInactivity_entry() {
  // Do we need to do anything?
}
DECLARE_XAM_EXPORT1(XamResetInactivity, kInput, kStub);

dword_result_t XamEnableInactivityProcessing_entry(dword_t unk,
                                                   dword_t enable) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamEnableInactivityProcessing, kInput, kStub);

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetcapabilities(v=vs.85).aspx
dword_result_t XamInputGetCapabilities_entry(
    dword_t user_index, dword_t flags, pointer_t<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto input_system = kernel_state()->emulator()->input_system();
  X_RESULT r = input_system->GetCapabilities(actual_user_index, flags, caps);
  // TEMP DIAG (clean-TU5 join gate): the RB3 Xbox joypad reader thread polls
  // XInputGetCapabilities per pad each iteration; if it never loops we see this
  // fire ~once. Log first several calls with caller LR + result + subtype.
  {
    static std::atomic<uint32_t> s_caps_calls{0};
    uint32_t n = s_caps_calls.fetch_add(1, std::memory_order_relaxed);
    if (n < 12) {
      uint32_t lr = 0;
      auto* thread = XThread::GetCurrentThread();
      if (thread) lr = static_cast<uint32_t>(thread->thread_state()->context()->lr);
      XELOGE(
          "XamInputGetCapabilities DIAG: call#{} user={} flags=0x{:X} "
          "result=0x{:08X} subtype={} lr=0x{:08X}",
          n, static_cast<uint32_t>(user_index & 0xFF),
          static_cast<uint32_t>(flags), static_cast<uint32_t>(r),
          r == X_ERROR_SUCCESS ? static_cast<uint32_t>(caps->sub_type) : 0, lr);
    }
  }
  return r;
}
DECLARE_XAM_EXPORT1(XamInputGetCapabilities, kInput, kSketchy);

dword_result_t XamInputGetCapabilitiesEx_entry(
    dword_t unk, dword_t user_index, dword_t flags,
    pointer_t<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto input_system = kernel_state()->emulator()->input_system();
  X_RESULT r = input_system->GetCapabilities(actual_user_index, flags, caps);
  LogInputDiag("GetCapabilitiesEx", user_index, flags, r);
  return r;
}
DECLARE_XAM_EXPORT1(XamInputGetCapabilitiesEx, kInput, kSketchy);

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetstate(v=vs.85).aspx
dword_result_t XamInputGetState_entry(dword_t user_index, dword_t flags,
                                      pointer_t<X_INPUT_STATE> input_state) {
  // Games call this with a NULL state ptr, probably as a query.

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto input_system = kernel_state()->emulator()->input_system();
  X_RESULT input_result = input_system->GetState(user_index, input_state);
  // TEMP DIAGNOSTIC (RB3DX main_hub stall investigation): log the first polls
  // per user and any button-state transitions delivered to the guest so
  // headless scripted-input delivery is observable end-to-end. Only logs on
  // warmup + transitions. TODO(rb3dx): remove after investigation.
  {
    static std::atomic<uint32_t> s_polls[4] = {};
    static std::atomic<uint16_t> s_last_buttons[4] = {};
    uint32_t ui = user_index & 0xFF;
    if (ui >= 4) ui = 0;
    uint32_t n = s_polls[ui].fetch_add(1, std::memory_order_relaxed);
    uint16_t buttons = (input_result == X_ERROR_SUCCESS && input_state)
                           ? static_cast<uint16_t>(input_state->gamepad.buttons)
                           : 0;
    if (n < 3) {
      XELOGE("XamInputGetState DIAG: user={} poll#{} result=0x{:08X}",
             static_cast<uint32_t>(user_index & 0xFF), n,
             static_cast<uint32_t>(input_result));
    }
    if (buttons != s_last_buttons[ui].exchange(buttons)) {
      XELOGE("XamInputGetState DIAG: user={} buttons 0x{:04X}",
             static_cast<uint32_t>(user_index & 0xFF), buttons);
    }
  }
  return input_result;
}
DECLARE_XAM_EXPORT2(XamInputGetState, kInput, kImplemented, kHighFrequency);

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputsetstate(v=vs.85).aspx
dword_result_t XamInputSetState_entry(dword_t user_index, dword_t unk,
                                      pointer_t<X_INPUT_VIBRATION> vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  uint32_t actual_user_index = user_index;
  if ((user_index & 0xFF) == 0xFF) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto input_system = kernel_state()->emulator()->input_system();
  X_RESULT r = input_system->SetState(user_index, vibration);
  LogInputDiag("SetState", user_index, unk, r);
  return r;
}
DECLARE_XAM_EXPORT1(XamInputSetState, kInput, kImplemented);

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetkeystroke(v=vs.85).aspx
dword_result_t XamInputGetKeystroke_entry(
    dword_t user_index, dword_t flags, pointer_t<X_INPUT_KEYSTROKE> keystroke) {
  // https://github.com/CodeAsm/ffplay360/blob/master/Common/AtgXime.cpp
  // user index = index or XUSER_INDEX_ANY
  // flags = XINPUT_FLAG_GAMEPAD (| _ANYUSER | _ANYDEVICE)

  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto input_system = kernel_state()->emulator()->input_system();
  X_RESULT r = input_system->GetKeystroke(user_index, flags, keystroke);
  LogInputDiag("GetKeystroke", user_index, flags, r);
  return r;
}
DECLARE_XAM_EXPORT1(XamInputGetKeystroke, kInput, kImplemented);

// Same as non-ex, just takes a pointer to user index.
dword_result_t XamInputGetKeystrokeEx_entry(
    lpdword_t user_index_ptr, dword_t flags,
    pointer_t<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t user_index = *user_index_ptr;
  if ((user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    user_index = 0;
  }

  auto input_system = kernel_state()->emulator()->input_system();
  auto result = input_system->GetKeystroke(user_index, flags, keystroke);
  if (XSUCCEEDED(result)) {
    *user_index_ptr = keystroke->user_index;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamInputGetKeystrokeEx, kInput, kImplemented);

X_HRESULT_result_t XamUserGetDeviceContext_entry(dword_t user_index,
                                                 dword_t unk,
                                                 lpdword_t out_ptr) {
  // Games check the result - usually with some masking.
  // If this function fails they assume zero, so let's fail AND
  // set zero just to be safe.
  *out_ptr = 0;
  if (!user_index || (user_index & 0xFF) == 0xFF) {
    LogInputDiag("UserGetDeviceContext", user_index, unk, 0);
    return X_E_SUCCESS;
  } else {
    LogInputDiag("UserGetDeviceContext", user_index, unk,
                 static_cast<uint32_t>(X_E_DEVICE_NOT_CONNECTED));
    return X_E_DEVICE_NOT_CONNECTED;
  }
}
DECLARE_XAM_EXPORT1(XamUserGetDeviceContext, kInput, kStub);

dword_result_t XamInputSendStayAliveRequest_entry(unknown_t unk1,
                                                   unknown_t unk2) {
  LogInputDiag("SendStayAliveRequest", static_cast<uint32_t>(unk1.value()),
               static_cast<uint32_t>(unk2.value()), 0);
  return 0;
}
DECLARE_XAM_EXPORT1(XamInputSendStayAliveRequest, kInput, kStub);

dword_result_t XamInputControl_entry(unknown_t unk1, unknown_t unk2,
                                      unknown_t unk3) {
  LogInputDiag("InputControl", static_cast<uint32_t>(unk1.value()),
               static_cast<uint32_t>(unk2.value()), 0);
  return 0;
}
DECLARE_XAM_EXPORT1(XamInputControl, kInput, kStub);

dword_result_t XamInputRawState_entry(dword_t user_index, lpvoid_t state_ptr) {
  LogInputDiag("InputRawState", user_index, 0,
               static_cast<uint32_t>(X_E_FAIL));
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamInputRawState, kInput, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Input);
