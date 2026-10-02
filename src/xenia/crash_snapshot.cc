/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Guest crash diagnostics written from Emulator::ExceptionCallback (NOT upstream).
 *
 * Moved verbatim out of emulator.cc (Phase 1, FORK_CLEANUP_PLAN.md).
 ******************************************************************************
 */

#include "xenia/crash_snapshot.h"

#include <cinttypes>
#include <fstream>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

DEFINE_string(
    dc3_crash_snapshot_path, "",
    "DC3: optional JSON crash snapshot output path written from guest crash "
    "dumps (headless-friendly structured artifact for postmortem tools).",
    "DC3");

namespace xe {

namespace {

const char* AccessOpName(Exception::AccessViolationOperation op) {
  switch (op) {
    case Exception::AccessViolationOperation::kRead:
      return "READ";
    case Exception::AccessViolationOperation::kWrite:
      return "WRITE";
    default:
      return "UNKNOWN";
  }
}

}  // namespace

void MaybeWriteCrashSnapshotJson(const Emulator* emulator, Exception* ex,
                                 kernel::XThread* current_thread,
                                 const cpu::Function* guest_function,
                                 const cpu::ppc::PPCContext* context) {
  if (cvars::dc3_crash_snapshot_path.empty() || !emulator || !ex ||
      !current_thread || !context) {
    return;
  }

  uint32_t guest_pc = 0;
  if (guest_function && guest_function->is_guest()) {
    guest_pc = static_cast<const cpu::GuestFunction*>(guest_function)
                   ->MapMachineCodeToGuestAddress(ex->pc());
  }

  uint64_t host_fault = 0;
  uint32_t guest_fault = 0;
  bool has_fault = ex->code() == Exception::Code::kAccessViolation;
  if (has_fault) {
    host_fault = ex->fault_address();
    if (emulator->memory() && emulator->memory()->virtual_membase()) {
      uint64_t membase =
          reinterpret_cast<uintptr_t>(emulator->memory()->virtual_membase());
      guest_fault = static_cast<uint32_t>(host_fault - membase);
    }
  }

  std::ofstream out(cvars::dc3_crash_snapshot_path,
                    std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    XELOGW("DC3 crash snapshot: failed to open {}", cvars::dc3_crash_snapshot_path);
    return;
  }

  out << "{\n";
  out << fmt::format("  \"host_pc\": {},\n", static_cast<uint64_t>(ex->pc()));
  out << fmt::format("  \"guest_pc\": {},\n", guest_pc);
  out << fmt::format("  \"guest_lr\": {},\n", static_cast<uint32_t>(context->lr));
  out << fmt::format("  \"guest_ctr\": {},\n", static_cast<uint32_t>(context->ctr));
  out << fmt::format("  \"guest_cr\": {},\n", static_cast<uint32_t>(context->cr()));
  out << fmt::format(
      "  \"guest_xer\": {{\"ca\": {}, \"ov\": {}, \"so\": {}}},\n",
      static_cast<uint32_t>(context->xer_ca), static_cast<uint32_t>(context->xer_ov),
      static_cast<uint32_t>(context->xer_so));
  out << fmt::format("  \"thread\": {{\"host_id\": {}, \"guest_id\": {}, \"handle\": {}}},\n",
                     current_thread->thread()->system_id(), current_thread->thread_id(),
                     current_thread->handle());
  out << "  \"gpr\": [";
  for (int i = 0; i < 32; ++i) {
    if (i) out << ", ";
    out << static_cast<uint64_t>(context->r[i]);
  }
  out << "],\n";
  if (has_fault) {
    out << fmt::format(
        "  \"fault\": {{\"host\": {}, \"guest\": {}, \"access\": \"{}\"}},\n",
        host_fault, guest_fault, AccessOpName(ex->access_violation_operation()));
  } else {
    out << "  \"fault\": null,\n";
  }
  out << "  \"guest_code_words\": [";
  bool first = true;
  if (guest_pc && emulator->memory() && guest_pc >= 0x80000000 && guest_pc < 0xA0000000) {
    uint32_t dump_start = (guest_pc > 0x20) ? (guest_pc - 0x20) : guest_pc;
    uint32_t dump_end = guest_pc + 0x20;
    for (uint32_t addr = dump_start; addr < dump_end; addr += 4) {
      auto* mem_ptr = emulator->memory()->TranslateVirtual<uint8_t*>(addr);
      if (!mem_ptr) break;
      uint32_t instr = xe::load_and_swap<uint32_t>(mem_ptr);
      if (!first) out << ", ";
      first = false;
      out << fmt::format("{{\"addr\": {}, \"word\": {}}}", addr, instr);
    }
  }
  out << "]\n";
  out << "}\n";
  out.flush();
  if (!out.good()) {
    XELOGW("DC3 crash snapshot: write failed {}", cvars::dc3_crash_snapshot_path);
    return;
  }
  XELOGI("DC3 crash snapshot JSON written: {}", cvars::dc3_crash_snapshot_path);
}

void DumpGuestCrashDetails(Memory* memory, Exception* ex,
                           cpu::GuestFunction* guest_function,
                           const cpu::ppc::PPCContext* context) {
  // Dump fault address details for access violations.
  if (ex->code() == Exception::Code::kAccessViolation) {
    uint64_t host_fault = ex->fault_address();
    uint64_t membase =
        reinterpret_cast<uintptr_t>(memory->virtual_membase());
    uint32_t guest_fault = static_cast<uint32_t>(host_fault - membase);
    XELOGE("Fault address: host=0x{:016X} guest=0x{:08X} ({})", host_fault,
           guest_fault,
           ex->access_violation_operation() ==
                   Exception::AccessViolationOperation::kWrite
               ? "WRITE"
               : "READ");
  }

  // Dump guest function info and code around crash PC.
  if (guest_function) {
    uint32_t guest_pc = guest_function->MapMachineCodeToGuestAddress(ex->pc());
    XELOGE("Guest function: {} (0x{:08X})", guest_function->name(),
           guest_function->address());
    if (guest_pc >= 0x82000000 && guest_pc < 0x90000000) {
      uint32_t dump_start = (guest_pc > 0x40) ? (guest_pc - 0x40) : guest_pc;
      uint32_t dump_end = guest_pc + 0x40;
      XELOGE("Guest code near PC 0x{:08X}:", guest_pc);
      for (uint32_t addr = dump_start; addr < dump_end; addr += 4) {
        auto* mem_ptr = memory->TranslateVirtual<uint8_t*>(addr);
        if (!mem_ptr) break;
        uint32_t instr = xe::load_and_swap<uint32_t>(mem_ptr);
        XELOGE("  0x{:08X}: {:08X}{}", addr, instr,
               addr == guest_pc ? "  <-- CRASH PC" : "");
      }
    }
  }

  // Walk the PPC stack to identify call chain (helps diagnose recursion).
  // fork-cleanup-review C9: this runs INSIDE the exception handler for a
  // guest fault. TranslateVirtual is membase+addr and can NEVER return null
  // (the old `if (!ptr) break` guards were dead), so every read must first
  // prove the page is committed or a decommitted stack page turns a
  // diagnosable guest crash into a nested host SIGSEGV with no output.
  {
    auto stack_word_readable = [&](uint32_t addr) -> bool {
      if (addr < 0x70000000 || addr >= 0x78000000) return false;
      auto* heap = memory->LookupHeap(addr);
      if (!heap) return false;
      HeapAllocationInfo info = {};
      if (!heap->QueryRegionInfo(addr, &info)) return false;
      return (info.state & kMemoryAllocationCommit) != 0;
    };
    uint32_t sp = static_cast<uint32_t>(context->r[1]);
    XELOGE("==== STACK WALK (SP=0x{:08X}) ====", sp);
    int frame = 0;
    uint32_t last_lr = 0;
    int repeat_count = 0;
    constexpr int kMaxWalkFrames = 512;
    for (; frame < kMaxWalkFrames && sp >= 0x70000000 && sp < 0x78000000;
         frame++) {
      if (!stack_word_readable(sp) || !stack_word_readable(sp + 8)) {
        XELOGE("  [{}] sp=0x{:08X} not committed -- stopping walk", frame, sp);
        break;
      }
      auto* host_ptr = memory->TranslateVirtual<uint8_t*>(sp);
      uint32_t back_chain = xe::load_and_swap<uint32_t>(host_ptr);
      // Try multiple LR save locations:
      uint32_t lr_sp4 = xe::load_and_swap<uint32_t>(host_ptr + 4);
      uint32_t lr_sp8 = xe::load_and_swap<uint32_t>(host_ptr + 8);
      // Also try __savegprlr convention: LR at back_chain - 8
      uint32_t lr_bc8 = 0;
      if (back_chain >= 0x70000008 && back_chain < 0x78000000 &&
          stack_word_readable(back_chain - 8)) {
        auto* bc_ptr = memory->TranslateVirtual<uint8_t*>(back_chain - 8);
        lr_bc8 = xe::load_and_swap<uint32_t>(bc_ptr);
      }
      // Pick the most likely LR (first non-BEBEBEBE, non-zero, in code range)
      uint32_t best_lr = 0;
      if (lr_bc8 >= 0x82000000 && lr_bc8 < 0x8A000000) best_lr = lr_bc8;
      else if (lr_sp4 >= 0x82000000 && lr_sp4 < 0x8A000000) best_lr = lr_sp4;
      else if (lr_sp8 >= 0x82000000 && lr_sp8 < 0x8A000000) best_lr = lr_sp8;
      else best_lr = lr_sp4;  // fallback
      uint32_t frame_size = (back_chain > sp) ? (back_chain - sp) : 0;
      // Log with sampling: first 30 frames, then every 100th, last 10
      bool should_log = (frame < 30) || (frame % 100 == 0) ||
                         (best_lr != last_lr && best_lr != 0xBEBEBEBE);
      if (should_log) {
        if (repeat_count > 0) {
          XELOGE("  ... ({} identical frames skipped, lr=0x{:08X})",
                 repeat_count, last_lr);
          repeat_count = 0;
        }
        XELOGE("  [{}] sp=0x{:08X} sz={} lr_sp4=0x{:08X} lr_bc8=0x{:08X}",
               frame, sp, frame_size, lr_sp4, lr_bc8);
      } else {
        repeat_count++;
      }
      last_lr = best_lr;
      if (back_chain == 0 || back_chain == sp || back_chain < 0x70000000 ||
          back_chain >= 0x78000000)
        break;
      sp = back_chain;
    }
    if (repeat_count > 0) {
      XELOGE("  ... ({} identical frames skipped, lr=0x{:08X})",
             repeat_count, last_lr);
    }
    XELOGE("==== END STACK WALK ({} frames) ====", frame);
  }
}

}  // namespace xe
