/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Guest crash diagnostics written from Emulator::ExceptionCallback (NOT upstream).
 *
 * Moved out of emulator.cc. Title-agnostic despite the dc3_ cvar name
 * (renaming the cvar is a later lane).
 ******************************************************************************
 */

#ifndef XENIA_CRASH_SNAPSHOT_H_
#define XENIA_CRASH_SNAPSHOT_H_

#include "xenia/base/exception_handler.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {

class Emulator;
class Memory;
namespace kernel {
class XThread;
}  // namespace kernel

// Writes a JSON crash snapshot to --dc3_crash_snapshot_path (no-op if unset).
void MaybeWriteCrashSnapshotJson(const Emulator* emulator, Exception* ex,
                                 kernel::XThread* current_thread,
                                 const cpu::Function* guest_function,
                                 const cpu::ppc::PPCContext* context);

// Logs the fault address of an access violation, the guest code around the
// crash PC, and a validated walk of the guest stack.
void DumpGuestCrashDetails(Memory* memory, Exception* ex,
                           cpu::GuestFunction* guest_function,
                           const cpu::ppc::PPCContext* context);

}  // namespace xe

#endif  // XENIA_CRASH_SNAPSHOT_H_
