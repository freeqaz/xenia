/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 headless autonav (NOT upstream).
 *
 * Harness input for a headless DC3. Since lane B2 it is only the attract
 * A-press (input.attract_press in dc3_scripted_input.cc); the flow file drives
 * every other screen. --dc3_headless_autonav arms it.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_DC3_DC3_AUTONAV_H_
#define XENIA_TITLES_DC3_DC3_AUTONAV_H_

namespace xe {
class Memory;
namespace cpu {
class Processor;
}  // namespace cpu
namespace kernel {
class KernelState;
}  // namespace kernel

namespace dc3 {

bool AutonavEnabled();
void InstallAutonav(cpu::Processor* processor, Memory* memory,
                    kernel::KernelState* kernel_state);

}  // namespace dc3
}  // namespace xe

#endif  // XENIA_TITLES_DC3_DC3_AUTONAV_H_
