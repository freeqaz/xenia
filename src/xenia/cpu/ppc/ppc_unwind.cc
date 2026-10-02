/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_unwind.h"

#include <vector>

namespace xe {
namespace cpu {
namespace ppc {

RuntimeFunction RuntimeFunction::Decode(uint32_t entry_address, uint32_t begin,
                                        uint32_t info) {
  RuntimeFunction f;
  f.entry_address = entry_address;
  f.begin_address = begin;
  f.prolog_length = info & 0xFF;
  f.function_length = (info >> 8) & 0x3FFFFF;
  f.thirty_two_bit = (info >> 30) & 1;
  f.exception_flag = (info >> 31) & 1;
  return f;
}

bool LookupFunctionEntry(const GuestMemoryReader& memory,
                         uint32_t pdata_address, uint32_t pdata_size,
                         uint32_t pc, RuntimeFunction* out_function) {
  uint32_t count = pdata_size / 8;
  uint32_t lo = 0, hi = count;
  // Find the last entry whose BeginAddress <= pc.
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    uint32_t begin;
    if (!memory.read32(pdata_address + mid * 8, &begin)) {
      return false;
    }
    if (begin <= pc) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) {
    return false;
  }
  uint32_t entry = pdata_address + (lo - 1) * 8;
  uint32_t begin, info;
  if (!memory.read32(entry, &begin) || !memory.read32(entry + 4, &info)) {
    return false;
  }
  auto f = RuntimeFunction::Decode(entry, begin, info);
  if (pc < f.begin_address || pc >= f.end_address()) {
    return false;
  }
  *out_function = f;
  return true;
}

namespace {

// A guest register's value as a symbolic expression over the frame's entry
// stack pointer r1_0 (the caller's r1), or unknown.
struct Sym {
  bool r1_relative = false;
  int64_t offset = 0;
};

enum class SaveKind { kGpr, kFpr };
struct Save {
  SaveKind kind;
  uint32_t reg;
  int64_t offset;  // from r1_0
  uint32_t size;   // 4 or 8
};

// Where the frame's incoming LR value currently lives.
struct LrLocation {
  enum Kind { kLrRegister, kGpr, kMemory, kLost } kind = kLrRegister;
  uint32_t reg = 0;     // kGpr
  int64_t offset = 0;   // kMemory, from r1_0
};

struct PrologState {
  Sym gpr[32];
  int64_t r1_offset = 0;  // current r1 - r1_0
  bool r1_dynamic = false;
  std::vector<Save> saves;
  LrLocation lr;
  bool frame_allocated = false;
};

int32_t Simm16(uint32_t w) { return static_cast<int16_t>(w & 0xFFFF); }

// Register `reg` is overwritten with something that is not r1-derived.
void Clobber(PrologState& s, uint32_t reg) {
  s.gpr[reg] = Sym();
  if (s.lr.kind == LrLocation::kGpr && s.lr.reg == reg) {
    s.lr.kind = LrLocation::kLost;
  }
}

constexpr uint32_t kBlr = 0x4E800020;

// The symbolic value of register `ra` used as a store base.
bool BaseOffset(const PrologState& s, uint32_t ra, int64_t* out) {
  if (ra == 1) {
    *out = s.r1_offset;
    return true;
  }
  if (s.gpr[ra].r1_relative) {
    *out = s.gpr[ra].offset;
    return true;
  }
  return false;
}

void RecordGprStore(PrologState& s, uint32_t rs, int64_t address_offset,
                    uint32_t size) {
  s.saves.push_back({SaveKind::kGpr, rs, address_offset, size});
  if (s.lr.kind == LrLocation::kGpr && s.lr.reg == rs) {
    s.lr.kind = LrLocation::kMemory;
    s.lr.offset = address_offset;
  }
}

// Applies one non-branch prologue instruction. Returns false for an
// instruction the unwinder does not understand (it is then ignored).
bool ApplyInstruction(PrologState& s, uint32_t w) {
  uint32_t op = w >> 26;
  uint32_t rs = (w >> 21) & 31;
  uint32_t ra = (w >> 16) & 31;
  uint32_t rb = (w >> 11) & 31;
  int64_t base;
  if ((w & 0xFC1FFFFF) == 0x7C0802A6) {  // mflr rD
    s.lr.kind = LrLocation::kGpr;
    s.lr.reg = rs;
    s.gpr[rs] = Sym();
    return true;
  }
  switch (op) {
    case 36:  // stw rS, d(rA)
      if (!BaseOffset(s, ra, &base)) return false;
      RecordGprStore(s, rs, base + Simm16(w), 4);
      return true;
    case 62:  // std / stdu rS, ds(rA)
      if ((w & 3) > 1 || !BaseOffset(s, ra, &base)) return false;
      RecordGprStore(s, rs, base + static_cast<int16_t>(w & 0xFFFC), 8);
      if ((w & 3) == 1 && ra == 1) {  // stdu r1 (never seen; be safe)
        s.r1_offset += static_cast<int16_t>(w & 0xFFFC);
        s.frame_allocated = true;
      }
      return true;
    case 54:  // stfd fS, d(rA)
      if (!BaseOffset(s, ra, &base)) return false;
      s.saves.push_back({SaveKind::kFpr, rs, base + Simm16(w), 8});
      return true;
    case 37:  // stwu rS, d(rA)
      if (ra == 1 && rs == 1) {
        s.r1_offset += Simm16(w);
        s.frame_allocated = true;
        return true;
      }
      return false;
    case 14:  // addi rD, rA, simm  (li when rA == 0)
      if (ra != 0 && BaseOffset(s, ra, &base)) {
        Clobber(s, rs);
        s.gpr[rs].r1_relative = true;
        s.gpr[rs].offset = base + Simm16(w);
      } else {
        Clobber(s, rs);
      }
      return true;
    case 15:  // addis / lis
    case 24:  // ori
      Clobber(s, op == 24 ? ra : rs);
      return true;
    case 4:  // VMX128 store in a save helper: not tracked.
      return true;
    case 31: {
      uint32_t xo = (w >> 1) & 0x3FF;
      if (xo == 183 && rs == 1 && ra == 1) {  // stwux r1, r1, rB
        s.r1_dynamic = true;
        s.frame_allocated = true;
        return true;
      }
      if (xo == 444) {  // or rA, rS, rB  (mr when rS == rB)
        if (rs == rb && BaseOffset(s, rs, &base)) {
          Clobber(s, ra);
          s.gpr[ra].r1_relative = true;
          s.gpr[ra].offset = base;
        } else {
          Clobber(s, ra);
        }
        return true;
      }
      if (xo == 104) {  // neg rD, rA (large-frame size computation)
        Clobber(s, rs);
        return true;
      }
      if (xo == 231 || xo == 487) {  // stvx / stvxl: not tracked
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

// Interprets the body of a save helper (__savegprlr_N, __savefpr_N,
// __savevmx_N): straight-line stores ending in blr. False if the target is
// anything else.
bool ApplySaveHelper(const GuestMemoryReader& memory, PrologState& s,
                     uint32_t target) {
  PrologState trial = s;
  for (int i = 0; i < 80; ++i) {
    uint32_t w;
    if (!memory.read32(target + i * 4, &w)) {
      return false;
    }
    if (w == kBlr) {
      s = trial;
      return true;
    }
    uint32_t op = w >> 26;
    bool is_store = op == 36 || op == 62 || op == 54 || op == 4 ||
                    (op == 31 && (((w >> 1) & 0x3FF) == 231 ||
                                  ((w >> 1) & 0x3FF) == 487));
    bool is_li = op == 14 && ((w >> 16) & 31) == 0;
    if (!(is_store || is_li) || !ApplyInstruction(trial, w)) {
      return false;
    }
  }
  return false;
}

}  // namespace

bool VirtualUnwind(const GuestMemoryReader& memory,
                   const RuntimeFunction& function, uint32_t pc,
                   UnwindRegisters* regs, uint32_t* out_establisher_frame,
                   uint32_t* out_return_address) {
  PrologState s;
  uint32_t prolog_end = function.prolog_end_address();
  uint32_t stop = pc < prolog_end ? pc : prolog_end;
  for (uint32_t a = function.begin_address; a < stop; a += 4) {
    uint32_t w;
    if (!memory.read32(a, &w)) {
      return false;
    }
    if ((w >> 26) == 18 && (w & 3) == 1) {  // bl target
      int32_t li = static_cast<int32_t>(w & 0x03FFFFFC);
      if (li & 0x02000000) li |= static_cast<int32_t>(0xFC000000u);
      uint32_t target = a + li;
      // bl overwrites LR; the incoming value must already be elsewhere.
      if (s.lr.kind == LrLocation::kLrRegister) {
        s.lr.kind = LrLocation::kLost;
      }
      ApplySaveHelper(memory, s, target);
      continue;
    }
    ApplyInstruction(s, w);
  }

  uint32_t r1 = static_cast<uint32_t>(regs->gpr[1]);
  uint32_t r1_0;
  if (s.frame_allocated) {
    // The back chain at the allocated frame is the entry r1, for stwu and
    // stwux alike.
    if (!memory.read32(r1, &r1_0)) {
      return false;
    }
  } else {
    r1_0 = static_cast<uint32_t>(r1 - s.r1_offset);
  }

  // The return address, read before any register is restored (it may live in
  // a register the prologue saved earlier, e.g. std r31 then mflr r31).
  uint32_t return_address;
  switch (s.lr.kind) {
    case LrLocation::kLrRegister:
      return_address = regs->lr;
      break;
    case LrLocation::kGpr:
      return_address = static_cast<uint32_t>(regs->gpr[s.lr.reg]);
      break;
    case LrLocation::kMemory:
      if (!memory.read32(static_cast<uint32_t>(r1_0 + s.lr.offset),
                         &return_address)) {
        return false;
      }
      break;
    default:
      return false;
  }

  for (const auto& save : s.saves) {
    uint32_t address = static_cast<uint32_t>(r1_0 + save.offset);
    if (save.kind == SaveKind::kGpr && save.reg == 1) {
      continue;  // the back chain itself
    }
    uint64_t value;
    if (save.size == 8) {
      if (!memory.read64(address, &value)) return false;
    } else {
      uint32_t v32;
      if (!memory.read32(address, &v32)) return false;
      // stw saves the low word; the high word of a GPR is not recoverable,
      // keep the current one.
      value = (regs->gpr[save.reg] & 0xFFFFFFFF00000000ull) | v32;
    }
    if (save.kind == SaveKind::kGpr) {
      regs->gpr[save.reg] = value;
    } else {
      regs->fpr[save.reg] = value;
    }
  }

  *out_establisher_frame = r1;
  regs->gpr[1] = r1_0;
  regs->lr = return_address;
  *out_return_address = return_address;
  return true;
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe
