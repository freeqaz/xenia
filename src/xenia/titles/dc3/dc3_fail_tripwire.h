/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 Debug::Fail tripwire (NOT upstream).
 *
 * A read-only host thread that watches the guest's TheDebug object and logs,
 * loudly and once per change, when Debug::mFailing latches or
 * Debug::mFailThreadMsg is set: the failure message and the guest return
 * addresses Debug::Fail captured (mFailThreadStack). While mFailing is latched
 * every later MILO_FAIL is silently a no-op, so a run that latches it is
 * TAINTED: any removal A/B measured on it is measured with asserts silenced.
 *
 * The same thread logs the DC3 override audit (dc3_hacks.h) every 30 s.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_DC3_DC3_FAIL_TRIPWIRE_H_
#define XENIA_TITLES_DC3_DC3_FAIL_TRIPWIRE_H_

namespace xe {
class Memory;
namespace cpu {
class Processor;
}  // namespace cpu
namespace kernel {
class KernelState;
}  // namespace kernel

namespace dc3 {

// Original debug.xex layout only (TheDebug is a fixed .data address there).
void StartFailTripwire(Memory* memory, cpu::Processor* processor,
                       kernel::KernelState* kernel_state);

}  // namespace dc3
}  // namespace xe

#endif  // XENIA_TITLES_DC3_DC3_FAIL_TRIPWIRE_H_
