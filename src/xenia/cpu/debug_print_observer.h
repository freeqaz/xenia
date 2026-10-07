/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Observer seam for the guest's debug-print trap (NOT upstream).
 *
 * The XDK's RtlDebugPrintHelper is `twi 31, r0, 0x14` (r3 = text, r4 =
 * length); every OutputDebugString / DbgPrint-style print a title makes ends
 * there, and the x64 backend logs it at debug level ("(DebugPrint) ...").
 * A title module that needs one of those prints at the default log level
 * (titles/dc3/dc3_fail_tripwire.cc: Milo's "FAIL-MSG: <text>") installs
 * itself here; the CPU never depends on a title. The observer runs on the
 * printing guest thread, inside the trap, and must only read: it cannot
 * change what the guest sees. With no observer installed the trap is
 * unchanged.
 ******************************************************************************
 */

#ifndef XENIA_CPU_DEBUG_PRINT_OBSERVER_H_
#define XENIA_CPU_DEBUG_PRINT_OBSERVER_H_

#include <cstdint>
#include <string_view>

namespace xe {
namespace cpu {

// `text` is the printed bytes (bounded by the trap's length and a NUL);
// `thread_id` the printing guest thread's id.
using DebugPrintObserver = void (*)(uint32_t thread_id, std::string_view text);

// Installs (or, with nullptr, removes) the process-wide observer.
void SetDebugPrintObserver(DebugPrintObserver observer);
void NotifyDebugPrint(uint32_t thread_id, std::string_view text);

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_DEBUG_PRINT_OBSERVER_H_
