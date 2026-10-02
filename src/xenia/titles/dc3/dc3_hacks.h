/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 (title 373307D9) hack registry (NOT upstream).
 *
 * Every guest patch, guest-function override and runtime guest writer the DC3
 * module applies to the ORIGINAL debug.xex has a stable id here, so one run
 * can switch a single hack off and A/B it against a run with it on:
 *
 *   --dc3_disable_hacks=splash.begin_splasher,bink.sys_init
 *   --dc3_disable_hacks=splash.*            (prefix match)
 *
 * Each gate logs exactly one line when it is decided ("DC3 HACK on: <id>" or
 * "DC3 HACK off: <id>"), and runtime hacks log "DC3 HACK fired: <id>" the
 * first time they act. An id in --dc3_disable_hacks that names no hack is a
 * loud error at launch: a typo would otherwise make a removal A/B measure the
 * control twice.
 *
 * The decomp-layout pack (titles/dc3/decomp/) is NOT covered: it is a fixed
 * fingerprint for harness S3, not something to A/B piecewise.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_DC3_DC3_HACKS_H_
#define XENIA_TITLES_DC3_DC3_HACKS_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "xenia/base/cvar.h"
#include "xenia/cpu/ppc/ppc_context.h"

DECLARE_string(dc3_disable_hacks);

namespace xe {
namespace cpu {
class Processor;
}  // namespace cpu
namespace kernel {
class KernelState;
}  // namespace kernel

namespace dc3 {

// True unless --dc3_disable_hacks names `id` (exactly, or by a `prefix*`
// entry). Logs nothing.
bool HackEnabled(std::string_view id);

// HackEnabled() plus one log line naming the decision. Use at the point the
// hack is applied (launch time) or first considered (runtime hacks).
bool HackGate(std::string_view id, std::string_view what);

// For runtime hacks: logs "DC3 HACK fired: <id>" the first time per id.
void HackFired(std::string_view id);

// Validates --dc3_disable_hacks against the known ids (the static table in
// dc3_hacks.cc plus every id a gate was asked about so far). Call at the end
// of the launch hooks.
void HackValidateDisableList();

// Records a guest-function override the DC3 module registered, for the
// override audit (dc3_fail_tripwire.cc): per override, how often the host
// handler ran and whether the address was ever resolved through
// Processor::ResolveFunction (an indirect bctrl/vtable call or a host
// Execute), in which case the GUEST body ran instead of the handler.
void HackNoteOverride(uint32_t guest_address, std::string_view id);

// Counts one handler invocation for the override at `guest_address`.
void HackCountOverrideHit(uint32_t guest_address);

// Logs one line per noted override: hits and indirect resolution.
// `only_if_changed` suppresses the summary when nothing moved since the last
// call.
void HackLogOverrideAudit(cpu::Processor* processor, bool only_if_changed);

// Registers `handler` as a guest-function override of `guest_address`
// (Processor::RegisterGuestFunctionOverride) and notes it for the audit.
// The handler is responsible for calling HackCountOverrideHit.
void HackRegisterOverride(cpu::Processor* processor, uint32_t guest_address,
                          void (*handler)(cpu::ppc::PPCContext*,
                                          kernel::KernelState*),
                          std::string_view id, std::string name);

}  // namespace dc3
}  // namespace xe

#endif  // XENIA_TITLES_DC3_DC3_HACKS_H_
