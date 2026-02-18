/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

void KeEnableFpuExceptions_entry(dword_t enabled) {
  // TODO(benvanik): can we do anything about exceptions?
}
DECLARE_XBOXKRNL_EXPORT1(KeEnableFpuExceptions, kNone, kStub);

// Event Tracing stubs (ETX)
dword_result_t EtxProducerRegister_entry(lpvoid_t provider_ptr,
                                          lpdword_t handle_ptr) {
  XELOGI("EtxProducerRegister(provider={:08X})", provider_ptr.guest_address());
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(EtxProducerRegister, kNone, kStub);

dword_result_t EtxProducerUnregister_entry(unknown_t handle) {
  XELOGI("EtxProducerUnregister");
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(EtxProducerUnregister, kNone, kStub);

void EtxProducerLog_entry(unknown_t handle, unknown_t unk2, unknown_t unk3) {}
DECLARE_XBOXKRNL_EXPORT1(EtxProducerLog, kNone, kStub);

// Camera device stub
dword_result_t PsCamDeviceRequest_entry(unknown_t unk1, unknown_t unk2,
                                         unknown_t unk3) {
  return X_E_FAIL;
}
DECLARE_XBOXKRNL_EXPORT1(PsCamDeviceRequest, kNone, kStub);

// FPU state stubs (no-op on emulator)
dword_result_t KeSaveFloatingPointState_entry(lpvoid_t state_ptr) { return 0; }
DECLARE_XBOXKRNL_EXPORT1(KeSaveFloatingPointState, kNone, kStub);

dword_result_t KeRestoreFloatingPointState_entry(lpvoid_t state_ptr) {
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeRestoreFloatingPointState, kNone, kStub);

// LDI decompression stubs
dword_result_t LDICreateDecompression_entry(unknown_t unk1, unknown_t unk2,
                                              unknown_t unk3, unknown_t unk4,
                                              unknown_t unk5,
                                              lpdword_t handle_ptr) {
  // TODO: LZX decompression support
  return X_E_FAIL;
}
DECLARE_XBOXKRNL_EXPORT1(LDICreateDecompression, kNone, kStub);

dword_result_t LDIDecompress_entry(unknown_t handle, lpvoid_t src,
                                    dword_t src_len, lpvoid_t dst,
                                    lpdword_t dst_len) {
  return X_E_FAIL;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDecompress, kNone, kStub);

dword_result_t LDIDestroyDecompression_entry(unknown_t handle) { return 0; }
DECLARE_XBOXKRNL_EXPORT1(LDIDestroyDecompression, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Misc);
