/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 NuiSkeletonGetNextFrame sequencer override (NOT upstream).
 *
 * Dc3NuiSequencerExtern moved verbatim out of emulator.cc.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_DC3_DC3_NUI_SEQUENCER_H_
#define XENIA_TITLES_DC3_DC3_NUI_SEQUENCER_H_

#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel

void Dc3NuiSequencerExtern(cpu::ppc::PPCContext* ppc_context,
                           kernel::KernelState* kernel_state);

}  // namespace xe

#endif  // XENIA_TITLES_DC3_DC3_NUI_SEQUENCER_H_
