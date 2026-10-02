/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_UNWIND_H_
#define XENIA_CPU_PPC_PPC_UNWIND_H_

#include <cstdint>
#include <functional>

namespace xe {
namespace cpu {
namespace ppc {

// Guest stack unwinding for Xbox 360 (MSVC PowerPC) code: the pieces the
// kernel's RtlLookupFunctionEntry / RtlVirtualUnwind / RtlDispatchException
// need. Pure functions over a guest-memory reader, so they can be unit-tested
// without a processor. See docs/fork/core/GUEST_EXCEPTIONS.md.

// Reads a big-endian guest word / doubleword. Returns false if the address
// is not readable (the unwinder then stops rather than faulting).
struct GuestMemoryReader {
  std::function<bool(uint32_t address, uint32_t* out)> read32;
  std::function<bool(uint32_t address, uint64_t* out)> read64;
};

// One 8-byte compact .pdata entry (RUNTIME_FUNCTION):
//   word 0: BeginAddress
//   word 1: PrologLength:8 | FunctionLength:22 | ThirtyTwoBit:1 |
//           ExceptionFlag:1   (lengths in instructions, LSB first)
// With ExceptionFlag set, the language handler and its data are the two words
// just before BeginAddress.
struct RuntimeFunction {
  uint32_t entry_address = 0;  // guest address of the .pdata entry itself
  uint32_t begin_address = 0;
  uint32_t prolog_length = 0;    // instructions
  uint32_t function_length = 0;  // instructions
  bool thirty_two_bit = false;
  bool exception_flag = false;

  uint32_t end_address() const { return begin_address + function_length * 4; }
  uint32_t prolog_end_address() const {
    return begin_address + prolog_length * 4;
  }
  // Valid only when exception_flag is set.
  uint32_t handler_address_slot() const { return begin_address - 8; }
  uint32_t handler_data_slot() const { return begin_address - 4; }

  static RuntimeFunction Decode(uint32_t entry_address, uint32_t begin,
                                uint32_t info);
};

// Binary search of a sorted .pdata table at [pdata_address, +pdata_size) for
// the function containing `pc`.
bool LookupFunctionEntry(const GuestMemoryReader& memory,
                         uint32_t pdata_address, uint32_t pdata_size,
                         uint32_t pc, RuntimeFunction* out_function);

// The register state the unwinder tracks: what MSVC's prologues save and
// what a C++ catch continuation restores (VMX registers are not tracked).
struct UnwindRegisters {
  uint64_t gpr[32] = {};
  uint64_t fpr[32] = {};  // raw IEEE double bits
  uint32_t lr = 0;
};

// Virtual unwind of one frame. `pc` is the address of the instruction the
// frame is executing (for a frame below the top, the call instruction, i.e.
// return address - 4). On entry `regs` hold the state at `pc`; on success
// they hold the caller's state at its call site: r1 is the caller's stack
// pointer, the non-volatiles the prologue saved are restored, and
// regs->lr = *out_return_address = the return address into the caller.
// *out_establisher_frame is the frame's stack pointer after its prologue
// (the EstablisherFrame the language handler receives).
//
// Understood prologue instructions: mflr rX; stw/std/stfd to r1-relative
// (or r1-derived) slots; stwu r1,-X(r1); stwux r1,r1,rX; addi/subi rX,r1,k
// and mr rX,r1; bl to the CRT save helpers __savegprlr_N / __savefpr_N /
// __savevmx_N, recognised by their code, not by symbol. Anything else in a
// prologue is ignored; an unknown save is reported as failure only if it
// leaves the return address unrecoverable. Epilogues are not modelled: a
// frame whose pc is inside its epilogue (never the case at a call site)
// unwinds as if the full prologue had run.
bool VirtualUnwind(const GuestMemoryReader& memory,
                   const RuntimeFunction& function, uint32_t pc,
                   UnwindRegisters* regs, uint32_t* out_establisher_frame,
                   uint32_t* out_return_address);

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_UNWIND_H_
