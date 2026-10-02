/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_BACKEND_H_
#define XENIA_CPU_BACKEND_BACKEND_H_

#include <memory>

#include "xenia/cpu/backend/machine_info.h"
#include "xenia/cpu/thread_debug_info.h"

namespace xe {
namespace cpu {
class Breakpoint;
class Function;
class GuestFunction;
class Module;
class Processor;
}  // namespace cpu
}  // namespace xe

namespace xe {
namespace cpu {
namespace backend {

class Assembler;
class CodeCache;

class Backend {
 public:
  explicit Backend();
  virtual ~Backend();

  Processor* processor() const { return processor_; }
  const MachineInfo* machine_info() const { return &machine_info_; }
  CodeCache* code_cache() const { return code_cache_; }

  virtual bool Initialize(Processor* processor);

  virtual void* AllocThreadData();
  virtual void FreeThreadData(void* thread_data);

  virtual void CommitExecutableRange(uint32_t guest_low,
                                     uint32_t guest_high) = 0;

  virtual std::unique_ptr<Assembler> CreateAssembler() = 0;

  virtual std::unique_ptr<GuestFunction> CreateGuestFunction(
      Module* module, uint32_t address) = 0;

  // Calculates the next host instruction based on the current thread state and
  // current PC. This will look for branches and other control flow
  // instructions.
  virtual uint64_t CalculateNextHostInstruction(ThreadDebugInfo* thread_info,
                                                uint64_t current_pc) = 0;

  // Guest exception support. One guest frame of a throw chain, newest first:
  // the return address out of the frame and the caller's stack pointer.
  struct GuestUnwindFrame {
    uint32_t guest_return_address;
    uint32_t guest_caller_sp;
  };
  // Called when a guest unwind has chosen frames[target_index] as the frame
  // that will catch. frames[0] is the frame that called the raising kernel
  // export; host_scan_from is a host stack address inside that export (its
  // JIT caller's frame lies above it). Arms a one-shot "pending host return":
  // when the guest later returns out of the target frame to its caller from
  // somewhere higher on the host stack (the catch continuation runs as a
  // fresh JIT entry), the backend resumes the target frame's ORIGINAL host
  // caller instead of nesting deeper. False if the host frames could not be
  // matched to the guest frames (nothing is armed; the guest still runs).
  virtual bool ArmGuestUnwindReturn(uint64_t host_scan_from,
                                    uint32_t first_guest_pc,
                                    const GuestUnwindFrame* frames,
                                    size_t frame_count, size_t target_index) {
    return false;
  }

  virtual void InstallBreakpoint(Breakpoint* breakpoint) {}
  virtual void InstallBreakpoint(Breakpoint* breakpoint, Function* fn) {}
  virtual void UninstallBreakpoint(Breakpoint* breakpoint) {}

 protected:
  Processor* processor_ = nullptr;
  MachineInfo machine_info_;
  CodeCache* code_cache_ = nullptr;
};

}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_BACKEND_H_
