/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_GUEST_EXCEPTIONS_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_GUEST_EXCEPTIONS_H_

#include <cstdint>

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Guest structured-exception dispatch for RtlRaiseException / RtlUnwind /
// RtlCaptureContext. See docs/fork/core/GUEST_EXCEPTIONS.md.

// Walks the guest frames from the RtlRaiseException call site and calls each
// frame's language handler. Returns only if no handler took the exception
// (false) or a handler asked to continue execution (true). A handler that
// catches never returns here: it unwinds with RtlUnwind and resumes the guest
// at its catch continuation.
bool DispatchGuestException(uint32_t record_ptr);

// RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue), called from
// the guest; returns to the guest normally (see the .cc for the one transfer
// form supported).
void UnwindGuestFrames(uint32_t target_frame, uint32_t target_ip,
                       uint32_t record_ptr, uint32_t return_value);

// RtlCaptureContext: fills the 0xA40-byte Xbox 360 CONTEXT at context_ptr
// with the caller's state at the call.
void CaptureGuestContext(uint32_t context_ptr);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_GUEST_EXCEPTIONS_H_
