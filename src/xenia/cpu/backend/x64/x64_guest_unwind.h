/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_X64_X64_GUEST_UNWIND_H_
#define XENIA_CPU_BACKEND_X64_X64_GUEST_UNWIND_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "xenia/cpu/backend/backend.h"

namespace xe {
namespace cpu {
namespace ppc {
struct PPCContext_s;
}
namespace backend {
namespace x64 {

class X64CodeCache;

// Pending host returns (see Backend::ArmGuestUnwindReturn).
//
// A guest C++ catch resumes through the guest CRT's _JumpToContinuation:
// r1 = the catching frame's stack pointer, then blr to the continuation.
// The JIT turns that blr into a tail call of a fresh function starting at the
// continuation, so the catching function's remaining code runs on top of the
// host frames of the whole throw (throw chain, kernel export, dispatcher,
// handler chain). Guest state is all in PPCContext, so that is correct -- but
// when the continuation finally returns to the catching function's caller,
// the return address no longer matches the JIT frame's, and the JIT would
// keep nesting (and a thread whose top frame returns would hit the Execute
// sentinel). A pending host return records, for the catching frame, the host
// stack slot holding its ORIGINAL host return address; the JIT's mismatched-
// return path consults it and, on an exact match (guest target and guest r1),
// resets rsp to that slot and returns, discarding the stale frames -- the
// same frames the DC3 DTA throw hook already discards with longjmp.

// Number of armed records across all threads; the JIT only calls into the
// slow path while it is non-zero.
extern std::atomic<uint32_t> g_pending_host_return_count;

// Called by the pending-return thunk from a mismatched guest return.
// Returns the host slot to `ret` from, or 0 to continue normally.
uint64_t TakePendingHostReturn(ppc::PPCContext_s* context, uint32_t target,
                               uint64_t jit_rsp);

bool ArmPendingHostReturn(X64CodeCache* code_cache, uint64_t host_scan_from,
                          uint32_t first_guest_pc,
                          const Backend::GuestUnwindFrame* frames,
                          size_t frame_count, size_t target_index);

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_X64_X64_GUEST_UNWIND_H_
