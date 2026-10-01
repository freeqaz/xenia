/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_CPP_THROW_HOOK_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_CPP_THROW_HOOK_H_

#include <cstdint>

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Called from RtlRaiseException for a guest C++ throw (code 0xE06D7363) BEFORE
// the stock handling (which cannot unwind and only raises SIGTRAP), with the
// guest address of the thrown object. Xenia has no guest C++ EH unwinder, so a
// guest `throw` is otherwise unrecoverable.
//
// A hook that wants to recover (the DC3 DTA channel) never returns: it
// longjmps back to a host frame it armed on the SAME thread. A hook with
// nothing armed on this thread must simply return, leaving stock behaviour.
using CppThrowHook = void (*)(uint32_t thrown_object_ptr);
extern CppThrowHook g_cpp_throw_hook;

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_CPP_THROW_HOOK_H_
