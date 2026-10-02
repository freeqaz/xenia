/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_guest_exceptions.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_unwind.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

// How the pieces fit (docs/fork/core/GUEST_EXCEPTIONS.md):
//
//   guest throw -> _CxxThrowException -> RtlRaiseException (here):
//     DispatchGuestException walks the guest frames with the .pdata unwinder
//     and calls each frame's language handler (__CxxFrameHandler) through
//     Processor::Execute, on this thread, below the throw point's stack.
//   the handler that catches -> CatchIt -> _UnwindNestedFrames ->
//     RtlUnwind(TargetFrame, TargetIp = the instruction after that call):
//     UnwindGuestFrames walks again -- through the handler's own frames,
//     across the Execute boundary back into the throw chain -- calling each
//     handler with EXCEPTION_UNWINDING (destructors) and the target frame's
//     with EXCEPTION_TARGET_UNWIND. The target handler copies the CONTEXT it
//     receives (the catching frame's registers) for the CRT and replaces it
//     with the context _UnwindNestedFrames captured, so "restore the context
//     at TargetIp" is just returning from RtlUnwind.
//   the CRT then runs the catch funclet and _JumpToContinuation, which loads
//   the catching frame's registers and branches into it -- guest code doing
//   its own transfer. The JIT runs the continuation as a fresh entry on top
//   of the host stack; the pending host return armed here
//   (Backend::ArmGuestUnwindReturn) folds those host frames away when the
//   catching function returns.

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

// Xbox 360 CONTEXT (0xA40 bytes). Offsets are pinned by the guest CRT:
// _UnwindNestedFrames stores TargetIp at +0x08 of a captured context and
// copies 0xA40 bytes; _JumpToContinuation loads Ctr +0x10, Gpr[n] +0x18+8n,
// Cr +0x118, Xer +0x11C, Fpscr +0x120, Fpr[n] +0x128+8n.
constexpr uint32_t kContextSize = 0xA40;
constexpr uint32_t kCtxIar = 0x08;
constexpr uint32_t kCtxLr = 0x0C;
constexpr uint32_t kCtxCtr = 0x10;
constexpr uint32_t kCtxGpr = 0x18;
constexpr uint32_t kCtxCr = 0x118;
constexpr uint32_t kCtxXer = 0x11C;
constexpr uint32_t kCtxFpscr = 0x120;
constexpr uint32_t kCtxFpr = 0x128;
constexpr uint32_t kCtxVr = 0x240;

// DISPATCHER_CONTEXT as the CRT reads it: [0] ControlPc, [1] FunctionEntry,
// [2] EstablisherFrame, [3] ContextRecord; [4]/[5] handler and its data.
constexpr uint32_t kDispatcherContextSize = 0x20;

constexpr uint32_t kExceptionUnwinding = 0x2;
constexpr uint32_t kExceptionExitUnwind = 0x4;
constexpr uint32_t kExceptionTargetUnwind = 0x20;
constexpr uint32_t kExecuteSentinel = 0xBCBCBCBC;  // Processor::Execute's LR
constexpr uint32_t kExecuteStackPad = 64 + 112;    // Processor::Execute's r1
constexpr int kMaxFrames = 256;

Memory* memory() { return kernel_state()->memory(); }

cpu::ppc::GuestMemoryReader Reader() {
  cpu::ppc::GuestMemoryReader r;
  r.read32 = [](uint32_t a, uint32_t* out) {
    auto heap = memory()->LookupHeap(a);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(a, &protect) ||
        !(protect & kMemoryProtectRead)) {
      return false;
    }
    *out = xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(a));
    return true;
  };
  r.read64 = [](uint32_t a, uint64_t* out) {
    auto heap = memory()->LookupHeap(a);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(a, &protect) ||
        !(protect & kMemoryProtectRead)) {
      return false;
    }
    *out = xe::load_and_swap<uint64_t>(memory()->TranslateVirtual(a));
    return true;
  };
  return r;
}

void W32(uint32_t a, uint32_t v) {
  xe::store_and_swap<uint32_t>(memory()->TranslateVirtual(a), v);
}
void W64(uint32_t a, uint64_t v) {
  xe::store_and_swap<uint64_t>(memory()->TranslateVirtual(a), v);
}
uint32_t R32(uint32_t a) {
  return xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(a));
}
uint64_t R64(uint32_t a) {
  return xe::load_and_swap<uint64_t>(memory()->TranslateVirtual(a));
}

uint64_t DoubleBits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  return u;
}
double BitsDouble(uint64_t u) {
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}

cpu::ppc::UnwindRegisters RegistersFrom(const cpu::ppc::PPCContext* ctx) {
  cpu::ppc::UnwindRegisters regs;
  for (int i = 0; i < 32; ++i) {
    regs.gpr[i] = ctx->r[i];
    regs.fpr[i] = DoubleBits(ctx->f[i]);
  }
  regs.lr = static_cast<uint32_t>(ctx->lr);
  return regs;
}

// Writes a CONTEXT for `regs`; registers the unwinder does not track (CR,
// XER, CTR, FPSCR, VMX) come from the live context.
void WriteContext(uint32_t p, const cpu::ppc::UnwindRegisters& regs,
                  uint32_t iar, const cpu::ppc::PPCContext* live) {
  std::memset(memory()->TranslateVirtual(p), 0, kContextSize);
  W32(p + kCtxIar, iar);
  W32(p + kCtxLr, regs.lr);
  W64(p + kCtxCtr, live->ctr);
  for (int i = 0; i < 32; ++i) {
    W64(p + kCtxGpr + 8 * i, regs.gpr[i]);
    W64(p + kCtxFpr + 8 * i, regs.fpr[i]);
  }
  W32(p + kCtxCr, static_cast<uint32_t>(live->cr()));
  W32(p + kCtxXer, (uint32_t(live->xer_so) << 31) |
                       (uint32_t(live->xer_ov) << 30) |
                       (uint32_t(live->xer_ca) << 29));
  W64(p + kCtxFpscr, live->fpscr.value);
  for (int i = 0; i < 128; ++i) {
    auto* dst = memory()->TranslateVirtual(p + kCtxVr + 16 * i);
    for (int lane = 0; lane < 4; ++lane) {
      xe::store_and_swap<uint32_t>(dst + 4 * lane, live->v[i].u32[lane]);
    }
  }
}

cpu::ppc::UnwindRegisters ReadContextRegisters(uint32_t p) {
  cpu::ppc::UnwindRegisters regs;
  for (int i = 0; i < 32; ++i) {
    regs.gpr[i] = R64(p + kCtxGpr + 8 * i);
    regs.fpr[i] = R64(p + kCtxFpr + 8 * i);
  }
  regs.lr = R32(p + kCtxLr);
  return regs;
}

// The .pdata entry of the XEX module containing pc.
bool LookupFunction(uint32_t pc, cpu::ppc::RuntimeFunction* out) {
  for (auto* module : kernel_state()->processor()->GetModules()) {
    auto* xex = dynamic_cast<cpu::XexModule*>(module);
    if (!xex || !xex->ContainsAddress(pc)) {
      continue;
    }
    auto* pdata = xex->GetPESection(".pdata");
    if (!pdata) {
      return false;
    }
    return cpu::ppc::LookupFunctionEntry(Reader(), pdata->address,
                                         pdata->size, pc, out);
  }
  return false;
}

// Calls a guest language handler on this thread. The guest scratch area
// (CONTEXT + DISPATCHER_CONTEXT) sits below the caller's r1; the handler
// runs below it. Everything but r3 is restored afterwards.
uint32_t CallHandler(cpu::ThreadState* thread_state, uint32_t handler,
                     uint32_t record_ptr, uint32_t establisher,
                     uint32_t context_ptr, uint32_t dc_ptr,
                     uint32_t handler_sp) {
  auto* ctx = thread_state->context();
  auto saved = std::make_unique<cpu::ppc::PPCContext>();
  std::memcpy(saved.get(), ctx, sizeof(cpu::ppc::PPCContext));
  ctx->r[1] = handler_sp;
  uint64_t args[] = {record_ptr, establisher, context_ptr, dc_ptr};
  uint64_t result = kernel_state()->processor()->Execute(thread_state, handler,
                                                         args, 4);
  std::memcpy(ctx, saved.get(), sizeof(cpu::ppc::PPCContext));
  return static_cast<uint32_t>(result);
}

// One frame of an in-progress dispatch, for RtlUnwind to continue through.
struct DispatchFrame {
  uint32_t establisher;
  uint32_t control_pc;
  cpu::backend::Backend::GuestUnwindFrame unwind;
};

// An in-progress RtlRaiseException on this thread. RtlUnwind, called from a
// handler, needs the throw point's registers to continue its walk past the
// Execute boundary, and the frame list to arm the host return.
struct ActiveDispatch {
  uint32_t handler_sp;
  uint32_t throw_lr;
  cpu::ppc::UnwindRegisters throw_regs;
  uint64_t host_scan_from;
  std::vector<DispatchFrame> frames;
};
thread_local std::vector<ActiveDispatch*> t_dispatches;

struct ScratchArea {
  uint32_t context_ptr;
  uint32_t dc_ptr;
  uint32_t handler_sp;
};

// [handler_sp ..) : back chain + Execute's pad; then DC, then CONTEXT, all
// below the current r1 and 16-byte aligned.
ScratchArea AllocateScratch(uint32_t r1) {
  ScratchArea s;
  uint32_t top = (r1 - 0x100) & ~0xFu;
  s.context_ptr = (top - kContextSize) & ~0xFu;
  s.dc_ptr = (s.context_ptr - kDispatcherContextSize) & ~0xFu;
  s.handler_sp = (s.dc_ptr - 0x100) & ~0xFu;
  W32(s.handler_sp, r1);  // back chain to the raising frame
  return s;
}

void WriteDispatcherContext(const ScratchArea& s, uint32_t control_pc,
                            const cpu::ppc::RuntimeFunction& fn,
                            uint32_t establisher, uint32_t handler,
                            uint32_t handler_data) {
  W32(s.dc_ptr + 0x00, control_pc);
  W32(s.dc_ptr + 0x04, fn.entry_address);
  W32(s.dc_ptr + 0x08, establisher);
  W32(s.dc_ptr + 0x0C, s.context_ptr);
  W32(s.dc_ptr + 0x10, handler);
  W32(s.dc_ptr + 0x14, handler_data);
}

}  // namespace

bool DispatchGuestException(uint32_t record_ptr) {
  auto* thread = XThread::GetCurrentThread();
  auto* thread_state = thread->thread_state();
  auto* ctx = thread_state->context();
  auto reader = Reader();

  if (!R32(record_ptr + 0x0C)) {
    W32(record_ptr + 0x0C, static_cast<uint32_t>(ctx->lr));  // address
  }
  uint32_t code = R32(record_ptr + 0x00);

  ActiveDispatch dispatch;
  int host_marker = 0;
  dispatch.host_scan_from = reinterpret_cast<uint64_t>(&host_marker);
  dispatch.throw_lr = static_cast<uint32_t>(ctx->lr);
  dispatch.throw_regs = RegistersFrom(ctx);
  ScratchArea scratch = AllocateScratch(static_cast<uint32_t>(ctx->r[1]));
  dispatch.handler_sp = scratch.handler_sp;
  t_dispatches.push_back(&dispatch);
  struct Pop {
    ActiveDispatch* d;
    ~Pop() {
      if (!t_dispatches.empty() && t_dispatches.back() == d) {
        t_dispatches.pop_back();
      }
    }
  } pop{&dispatch};

  auto regs = dispatch.throw_regs;
  uint32_t control_pc = dispatch.throw_lr - 4;
  uint32_t handlers_called = 0;
  for (int depth = 0; depth < kMaxFrames; ++depth) {
    cpu::ppc::RuntimeFunction fn;
    if (!LookupFunction(control_pc, &fn)) {
      XELOGW("GuestEH: no .pdata entry for {:08X}; exception {:08X} unhandled",
             control_pc, code);
      return false;
    }
    auto caller = regs;
    uint32_t establisher, ret;
    if (!cpu::ppc::VirtualUnwind(reader, fn, control_pc, &caller, &establisher,
                                 &ret)) {
      XELOGW("GuestEH: cannot unwind {:08X} (fn {:08X})", control_pc,
             fn.begin_address);
      return false;
    }
    dispatch.frames.push_back(
        {establisher, control_pc,
         {ret, static_cast<uint32_t>(caller.gpr[1])}});
    if (fn.exception_flag) {
      uint32_t handler = R32(fn.handler_address_slot());
      uint32_t handler_data = R32(fn.handler_data_slot());
      WriteContext(scratch.context_ptr, dispatch.throw_regs, dispatch.throw_lr,
                   ctx);
      WriteDispatcherContext(scratch, control_pc, fn, establisher, handler,
                             handler_data);
      ++handlers_called;
      uint32_t disposition =
          CallHandler(thread_state, handler, record_ptr, establisher,
                      scratch.context_ptr, scratch.dc_ptr, scratch.handler_sp);
      if (disposition == 0) {  // ExceptionContinueExecution
        XELOGI("GuestEH: handler {:08X} continued execution of {:08X}",
               handler, code);
        return true;
      }
      if (disposition != 1) {  // not ExceptionContinueSearch
        XELOGW("GuestEH: handler {:08X} returned disposition {}", handler,
               disposition);
        return false;
      }
    }
    if (ret == kExecuteSentinel || !ret) {
      break;  // the top of this host-entered guest call chain
    }
    regs = caller;
    control_pc = ret - 4;
  }
  XELOGW(
      "GuestEH: exception {:08X} raised at {:08X}: no handler took it ({} "
      "frames walked, {} language handlers called)",
      code, dispatch.throw_lr, dispatch.frames.size(), handlers_called);
  return false;
}

void UnwindGuestFrames(uint32_t target_frame, uint32_t target_ip,
                       uint32_t record_ptr, uint32_t return_value) {
  auto* thread = XThread::GetCurrentThread();
  auto* thread_state = thread->thread_state();
  auto* ctx = thread_state->context();
  auto reader = Reader();

  ActiveDispatch* dispatch = t_dispatches.empty() ? nullptr : t_dispatches.back();
  if (!dispatch) {
    // Not part of an exception dispatch (a longjmp, an exit unwind). The
    // only transfer built is the C++ catch path, so keep the old behaviour:
    // run nothing and return.
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true)) {
      XELOGW(
          "GuestEH: RtlUnwind({:08X}, {:08X}) outside an exception dispatch "
          "-- not unwinding (further calls not logged)",
          target_frame, target_ip);
    }
    return;
  }
  if (record_ptr) {
    uint32_t flags = R32(record_ptr + 0x04) | kExceptionUnwinding;
    if (!target_frame) flags |= kExceptionExitUnwind;
    W32(record_ptr + 0x04, flags);
  }

  ScratchArea scratch = AllocateScratch(static_cast<uint32_t>(ctx->r[1]));
  auto regs = RegistersFrom(ctx);
  uint32_t control_pc = static_cast<uint32_t>(ctx->lr) - 4;
  bool bridged = false;
  size_t throw_index = 0;  // index into dispatch->frames once bridged
  bool found = false;
  bool target_handled = false;
  for (int depth = 0; depth < kMaxFrames; ++depth) {
    cpu::ppc::RuntimeFunction fn;
    if (!LookupFunction(control_pc, &fn)) {
      XELOGW("GuestEH: RtlUnwind: no .pdata entry for {:08X}", control_pc);
      break;
    }
    auto caller = regs;
    uint32_t establisher, ret;
    if (!cpu::ppc::VirtualUnwind(reader, fn, control_pc, &caller, &establisher,
                                 &ret)) {
      XELOGW("GuestEH: RtlUnwind: cannot unwind {:08X}", control_pc);
      break;
    }
    bool is_target = establisher == target_frame;
    if (fn.exception_flag) {
      uint32_t handler = R32(fn.handler_address_slot());
      uint32_t handler_data = R32(fn.handler_data_slot());
      if (record_ptr) {
        uint32_t flags = R32(record_ptr + 0x04) & ~kExceptionTargetUnwind;
        W32(record_ptr + 0x04,
            flags | kExceptionUnwinding |
                (is_target ? kExceptionTargetUnwind : 0));
      }
      WriteContext(scratch.context_ptr, regs, control_pc + 4, ctx);
      WriteDispatcherContext(scratch, control_pc, fn, establisher, handler,
                             handler_data);
      CallHandler(thread_state, handler, record_ptr, establisher,
                  scratch.context_ptr, scratch.dc_ptr, scratch.handler_sp);
      target_handled = is_target;
    }
    if (is_target) {
      found = true;
      break;
    }
    if (bridged) {
      ++throw_index;
    }
    regs = caller;
    control_pc = ret - 4;
    if (ret == kExecuteSentinel) {
      // The top of the handler chain Processor::Execute started for a
      // dispatch: continue at the throw point, as NT continues through its
      // dispatcher's frame.
      if (!dispatch || bridged ||
          static_cast<uint32_t>(caller.gpr[1]) !=
              dispatch->handler_sp - kExecuteStackPad) {
        XELOGW("GuestEH: RtlUnwind reached the top of the guest stack");
        break;
      }
      bridged = true;
      throw_index = 0;
      regs = dispatch->throw_regs;
      control_pc = dispatch->throw_lr - 4;
    }
  }
  if (!found) {
    XELOGE("GuestEH: RtlUnwind: target frame {:08X} not found", target_frame);
    return;
  }

  // The context to resume: what the target frame's handler left in the
  // CONTEXT (MSVC's __CxxFrameHandler replaces it with the context
  // _UnwindNestedFrames captured), else the target frame's registers.
  auto resume = target_handled ? ReadContextRegisters(scratch.context_ptr)
                               : regs;
  uint32_t resume_iar = target_ip;
  if (resume_iar != static_cast<uint32_t>(ctx->lr) ||
      static_cast<uint32_t>(resume.gpr[1]) != static_cast<uint32_t>(ctx->r[1])) {
    // A transfer into a different frame (SEH __except, longjmp). Needs the
    // host-frame transfer described in GUEST_EXCEPTIONS.md; not built.
    XELOGE(
        "GuestEH: RtlUnwind to {:08X} with r1 {:08X} from {:08X}/{:08X}: "
        "cross-frame transfer is not implemented",
        resume_iar, static_cast<uint32_t>(resume.gpr[1]),
        static_cast<uint32_t>(ctx->lr), static_cast<uint32_t>(ctx->r[1]));
    return;
  }
  // Same frame, same pc: "restore the context" is returning from this export
  // with the restored non-volatiles and r3 = ReturnValue.
  for (int i = 14; i < 32; ++i) {
    ctx->r[i] = resume.gpr[i];
    ctx->f[i] = BitsDouble(resume.fpr[i]);
  }
  ctx->r[3] = return_value;

  if (bridged && dispatch) {
    // The catching frame is dispatch->frames[throw_index]. Arm the host
    // return so the catch continuation folds the throw's host frames away
    // when that frame returns.
    std::vector<cpu::backend::Backend::GuestUnwindFrame> frames;
    for (size_t i = 0; i <= throw_index && i < dispatch->frames.size(); ++i) {
      frames.push_back(dispatch->frames[i].unwind);
    }
    bool armed = throw_index < dispatch->frames.size() &&
                 dispatch->frames[throw_index].establisher == target_frame &&
                 kernel_state()->processor()->backend()->ArmGuestUnwindReturn(
                     dispatch->host_scan_from, dispatch->frames[0].control_pc,
                     frames.data(), frames.size(), throw_index);
    XELOGI(
        "GuestEH: unwound to frame {:08X} (depth {} of the throw at {:08X}); "
        "host return {}",
        target_frame, throw_index, dispatch->throw_lr,
        armed ? "armed" : "NOT armed (continuation stays nested)");
    // This dispatch is over: the handler resumes the guest at its catch.
    t_dispatches.pop_back();
  }
}

void CaptureGuestContext(uint32_t context_ptr) {
  if (!context_ptr) {
    return;
  }
  auto* ctx = XThread::GetCurrentThread()->thread_state()->context();
  WriteContext(context_ptr, RegistersFrom(ctx), static_cast<uint32_t>(ctx->lr),
               ctx);
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
