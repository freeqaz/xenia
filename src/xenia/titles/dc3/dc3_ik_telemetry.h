/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 IK telemetry caves (--dc3_ik_telemetry) (NOT upstream).
 *
 * Internal to the DC3 module. ReadDc3IKTelemetry and
 * ApplyDc3IKTelemetry are declared in dc3_hack_pack.h.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_DC3_DC3_IK_TELEMETRY_H_
#define XENIA_TITLES_DC3_DC3_IK_TELEMETRY_H_

#include "xenia/titles/dc3/dc3_hack_pack.h"

namespace xe {

// Installs the IK code caves. Called by ApplyDc3IKTelemetry (both layouts) and
// by the decomp hack pack's dispatcher.
void ApplyDc3IKInstrumentation(const Dc3HackContext& ctx,
                               Dc3HackApplyResult& result);

}  // namespace xe

#endif  // XENIA_TITLES_DC3_DC3_IK_TELEMETRY_H_
