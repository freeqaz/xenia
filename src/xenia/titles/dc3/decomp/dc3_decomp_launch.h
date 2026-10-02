/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 decomp-layout launch pieces (NOT upstream).
 *
 * Applied only to a DC3 image detected as the decomp layout (the
 * 627-trap boot bar). Moved verbatim out of the DC3 launch block.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_DC3_DECOMP_DC3_DECOMP_LAUNCH_H_
#define XENIA_TITLES_DC3_DECOMP_DC3_DECOMP_LAUNCH_H_

#include <optional>

#include "xenia/titles/dc3/dc3_nui_patch_resolver.h"

namespace xe {
class Memory;
namespace cpu {
class Processor;
}  // namespace cpu
namespace kernel {
class UserModule;
}  // namespace kernel

// NUI/XBC patch table for the decomp layout (decomp MAP addresses).
extern const dc3::Dc3NuiPatchSpec kDc3DecompNuiPatches[];
extern const int kDc3DecompNuiPatchCount;

// The decomp hack pack + manifest address catalog + IK telemetry, i.e. the
// body of `if (DC3 && dc3_is_decomp_layout)` in ApplyDc3LaunchHooks.
void ApplyDc3DecompHackPack(
    Memory* memory, cpu::Processor* processor, kernel::UserModule* module,
    bool headless, bool dc3_is_decomp_layout,
    const std::optional<dc3::Dc3NuiPatchManifest>& dc3_patch_manifest);

}  // namespace xe

#endif  // XENIA_TITLES_DC3_DECOMP_DC3_DECOMP_LAUNCH_H_
