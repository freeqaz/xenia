/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 headless autonav (NOT upstream).
 *
 * Harness automation for a Kinect-less headless DC3: the menus are gesture
 * driven, so the ymca flow needs the host to complete stuck transitions, call
 * UIManager::GotoScreen, inject the song and (until the audio clock runs)
 * drive the song clock. All of it runs on the guest MAIN thread through
 * dc3_main_thread.h, and only with --dc3_headless_autonav.
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
