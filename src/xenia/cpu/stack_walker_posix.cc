/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/stack_walker.h"

#include <algorithm>
#include <cstring>

#include "xenia/base/host_thread_context.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"

#if XE_PLATFORM_LINUX && XE_ARCH_AMD64
#define XE_POSIX_STACK_WALKER 1
#include <execinfo.h>
#endif

namespace xe {
namespace cpu {

#if XE_POSIX_STACK_WALKER

// Best-effort stack walker for the Linux/x86-64 JIT backend.
//
// Xenia's debugger (breakpoints / pause / single-step) only requires two
// things from the stack walker:
//   1. CaptureStackTrace(): produce a list of host return-address PCs for a
//      (suspended) guest thread, starting from a known host context. The
//      x86-64 backend emits standard rbp frame-pointer prologues for guest
//      functions, so we can chase the saved-rbp back-chain to recover frames.
//   2. ResolveStack(): map each host PC to a guest function + guest PC via the
//      JIT code cache (identical logic to the Win32 implementation).
//
// The frame-pointer walk is heuristic (it stops as soon as it leaves plausible
// stack memory), but for breakpoint hits the most important frame -- frame[0],
// the exact faulting PC -- is always exact because it is taken directly from
// the supplied host context. That is all Processor::OnThreadBreakpointHit needs
// to match a guest breakpoint and report registers.
class PosixStackWalker : public StackWalker {
 public:
  explicit PosixStackWalker(backend::CodeCache* code_cache)
      : code_cache_(code_cache) {
    code_cache_min_ = code_cache_->execute_base_address();
    code_cache_max_ = code_cache_min_ + code_cache_->total_size();
  }

  // Current-thread capture (used by Dump()). Uses libc backtrace().
  size_t CaptureStackTrace(uint64_t* frame_host_pcs, size_t frame_offset,
                           size_t frame_count,
                           uint64_t* out_stack_hash) override {
    if (out_stack_hash) {
      *out_stack_hash = 0;
    }
    void* buffer[256];
    int total = backtrace(
        buffer,
        static_cast<int>(std::min<size_t>(256, frame_offset + frame_count)));
    size_t out = 0;
    for (int i = static_cast<int>(frame_offset); i < total && out < frame_count;
         ++i) {
      frame_host_pcs[out++] = reinterpret_cast<uint64_t>(buffer[i]);
    }
    return out;
  }

  // Capture from a supplied host context (used by the debugger when a thread is
  // suspended or an exception fires). Walks the rbp back-chain.
  size_t CaptureStackTrace(void* /*thread_handle*/, uint64_t* frame_host_pcs,
                           size_t frame_offset, size_t frame_count,
                           const HostThreadContext* in_host_context,
                           HostThreadContext* out_host_context,
                           uint64_t* out_stack_hash) override {
    if (out_stack_hash) {
      *out_stack_hash = 0;
    }
    if (!in_host_context) {
      // We can only walk a thread we have a context for. The Linux backend
      // always supplies one (from the exception/suspend path).
      return 0;
    }
    if (out_host_context) {
      *out_host_context = *in_host_context;
    }

    uint64_t pc = in_host_context->rip;
    uint64_t rbp = in_host_context->rbp;
    uint64_t rsp = in_host_context->rsp;

    size_t produced = 0;
    size_t collected = 0;

    auto emit = [&](uint64_t frame_pc) {
      if (collected++ < frame_offset) {
        return;
      }
      if (produced < frame_count) {
        frame_host_pcs[produced++] = frame_pc;
      }
    };

    // Frame 0 is the exact faulting / suspended PC.
    emit(pc);

    // Chase the saved rbp chain. Standard prologue: push rbp; mov rbp, rsp.
    // At each frame [rbp] = caller rbp, [rbp+8] = return address.
    uint64_t cur_rbp = rbp;
    uint64_t prev_rbp = rsp;  // monotonic guard (stack grows down)
    for (int depth = 0; depth < 256 && produced < frame_count; ++depth) {
      if (cur_rbp < prev_rbp || (cur_rbp & 0x7) != 0) {
        break;
      }
      uint64_t saved_rbp = 0;
      uint64_t ret_addr = 0;
      if (!SafeReadU64(cur_rbp, &saved_rbp) ||
          !SafeReadU64(cur_rbp + 8, &ret_addr)) {
        break;
      }
      if (ret_addr == 0) {
        break;
      }
      emit(ret_addr);
      if (saved_rbp <= cur_rbp) {
        break;
      }
      prev_rbp = cur_rbp;
      cur_rbp = saved_rbp;
    }

    return produced;
  }

  bool ResolveStack(uint64_t* frame_host_pcs, StackFrame* frames,
                    size_t frame_count) override {
    for (size_t i = 0; i < frame_count; ++i) {
      auto& frame = frames[i];
      std::memset(&frame, 0, sizeof(frame));
      frame.host_pc = frame_host_pcs[i];

      if (frame.host_pc >= code_cache_min_ && frame.host_pc < code_cache_max_) {
        frame.type = StackFrame::Type::kGuest;
        auto function = code_cache_->LookupFunction(frame.host_pc);
        if (function) {
          frame.guest_symbol.function = function;
          if (function->is_guest()) {
            auto guest_function = static_cast<GuestFunction*>(function);
            // Frame 0 is the precise fault/suspend PC and must map exactly so
            // breakpoint matching works. Caller frames hold the return address
            // (the instruction *after* the call), so adjust by -1 to land back
            // inside the call instruction, matching the Win32 walker.
            uintptr_t lookup_pc =
                (i == 0) ? static_cast<uintptr_t>(frame.host_pc)
                         : static_cast<uintptr_t>(frame.host_pc - 1);
            frame.guest_pc =
                guest_function->MapMachineCodeToGuestAddress(lookup_pc);
          }
        } else {
          frame.guest_symbol.function = nullptr;
        }
      } else {
        frame.type = StackFrame::Type::kHost;
        frame.host_symbol.address = frame.host_pc;
        frame.host_symbol.name[0] = '\0';
      }
    }
    return true;
  }

 private:
  // Reads 8 bytes of host memory, guarding against obviously-bad pointers.
  // We can't cheaply probe page protections here, so we reject null/low and
  // misaligned addresses; the monotonic-rbp guard in the walk loop keeps us on
  // well-formed JIT frames in practice.
  static bool SafeReadU64(uint64_t addr, uint64_t* out) {
    if (addr < 0x10000 || (addr & 0x7) != 0) {
      return false;
    }
    *out = *reinterpret_cast<const uint64_t*>(addr);
    return true;
  }

  backend::CodeCache* code_cache_ = nullptr;
  uintptr_t code_cache_min_ = 0;
  uintptr_t code_cache_max_ = 0;
};

std::unique_ptr<StackWalker> StackWalker::Create(
    backend::CodeCache* code_cache) {
  if (!code_cache) {
    return nullptr;
  }
  XELOGI("Using posix (frame-pointer) stack walker for guest debugging");
  return std::make_unique<PosixStackWalker>(code_cache);
}

#else

std::unique_ptr<StackWalker> StackWalker::Create(
    backend::CodeCache* code_cache) {
  XELOGD("Stack walker unimplemented on this posix platform");
  return nullptr;
}

#endif  // XE_POSIX_STACK_WALKER

}  // namespace cpu
}  // namespace xe
