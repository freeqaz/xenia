/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Kinect (NUI) HLE: the SDK facade.
 *
 * Kinect titles statically link the NUI SDK (NUI, ST, NUISP, NUIAUD, FITNESS)
 * and drive the sensor through it; below the SDK there is only a raw camera
 * driver (PsCamDeviceRequest). The facade emulates the SDK's public API, the
 * way Cxbx-Reloaded HLEs statically linked XDK libraries:
 *   1. read the XEX static-library header; act only if it names NUI at a
 *      version this file has a signature table for (never a title id);
 *   2. find every API function in .text by a masked word signature (plus an
 *      XREF anchor for thunks), refusing zero or multiple matches;
 *   3. only if EVERY entry resolves, register a guest-function override for
 *      each: a partial HLE would let SDK internals run against a runtime
 *      that NuiInitialize never set up.
 * See docs/fork/nui/NUI_HLE_DESIGN.md section 3.3.
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_NUI_SDK_FACADE_H_
#define XENIA_KERNEL_NUI_NUI_SDK_FACADE_H_

#include <cstdint>

#include "xenia/kernel/nui/nui_signature.h"

namespace xe {
namespace cpu {
class Processor;
}  // namespace cpu
namespace kernel {
class KernelState;
class UserModule;
namespace nui {

// Emulator::CompleteLaunch, after the module is loaded and before the title
// hooks: detect the SDK, resolve, register, create the device.
void InstallNuiHle(KernelState* kernel_state, cpu::Processor* processor,
                   UserModule* module);
// Emulator::TerminateTitle / ~Emulator: stop the frame clock, drop the device.
void ShutdownNuiHle();

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_NUI_SDK_FACADE_H_
