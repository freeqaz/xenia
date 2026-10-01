/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_EMIT_PRIVATE_H_
#define XENIA_CPU_PPC_PPC_EMIT_PRIVATE_H_

#include <atomic>

#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_decode_data.h"
#include "xenia/cpu/ppc/ppc_emit.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"

namespace xe {
namespace cpu {
namespace ppc {

#define XEREGISTERINSTR(name) \
  RegisterOpcodeEmitter(PPCOpcode::name, InstrEmit_##name);

// Reports that the enclosing instruction emitter could not translate (all or
// part of) the instruction it was handed.
//
// Upstream this was `XELOGE(...); assert_always(...)`. This fork keeps
// translating -- the affected instruction (or, for the partial cases such as
// `addo`, the affected side effect) simply becomes a no-op -- because the DC3
// (0x373307D9) and RB3DX (0x45410914) decomp targets depend on that tolerance
// to boot at all. Restoring the assert would fatally break them under the
// Checked config they are built with.
//
// What must NOT happen is the fork's previous `do {} while (false)`: a silent
// no-op meant no title, in any build, could ever be told that it hit an
// unimplemented opcode. See docs/fork-cleanup-review.md, finding C1.
//
// This fires at translation time (once per emitter invocation, i.e. once per
// instruction site per JIT compile), not per guest execution, so the volume is
// bounded -- but a hot recompile loop could still repeat it, hence the
// per-call-site rate limit. The static lives inside the macro body, so each
// expansion (each emitter function) gets its own independent budget.
//
// The DebugBreak for this condition stays where upstream put it, in
// PPCHIRBuilder::Emit behind cvars::break_on_unimplemented_instructions; the
// emitters signal it by returning non-zero.
#define XEINSTRNOTIMPLEMENTED()                                              \
  do {                                                                       \
    static constexpr uint32_t kNotImplementedLogLimit = 8;                   \
    static std::atomic<uint32_t> xe_not_implemented_count{0};                \
    uint32_t xe_not_implemented_n =                                          \
        xe_not_implemented_count.fetch_add(1, std::memory_order_relaxed);    \
    if (xe_not_implemented_n < kNotImplementedLogLimit) {                    \
      XELOGE("Unimplemented instruction: {}", __func__);                     \
    } else if (xe_not_implemented_n == kNotImplementedLogLimit) {            \
      XELOGE(                                                                \
          "Unimplemented instruction: {} (hit {} times; suppressing further " \
          "reports for this instruction)",                                   \
          __func__, kNotImplementedLogLimit + 1);                            \
    }                                                                        \
  } while (false)

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_EMIT_PRIVATE_H_
