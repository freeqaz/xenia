/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/exception_handler.h"

#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <cstdint>

#include <atomic>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/host_thread_context.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"

// Livelock circuit-breaker: when a recovered host fault re-executes the SAME
// guest/host instruction at the SAME faulting address this many times in a row
// with no forward progress, treat it as a fatal wedge (log once + abort) rather
// than spinning forever. A legitimate soft-fault (stack-guard read, GPU write
// watch, DC3 stub-keepalive) always advances rip or changes the address, so it
// resets the counter and never trips. 0 disables. (RB3 Deluxe title-screen heap
// OOM falls into MemHeap::Alloc's post-assert dead code and stores to guest
// 0xFFFFFFFC forever; this surfaces it as a diagnosable crash. DC3-inert.)
DEFINE_uint64(fault_spin_limit, 4096,
              "Abort after this many consecutive identical recovered host "
              "faults (same rip+address, no progress). 0 = disabled.",
              "CPU");

namespace xe {

// Diagnostic counters for fault loop detection.
static std::atomic<uint64_t> sigsegv_count_{0};
static std::atomic<uint64_t> last_fault_address_{0};
static std::atomic<uint64_t> last_fault_rip_{0};
// Livelock diagnostics: when the circuit-breaker trips, we capture the faulting
// thread's guest PPCContext pointer (Xenia keeps it in host rsi throughout JIT
// execution) and raise a flag so a safe thread (the headless status loop) can
// decode the guest registers + heap name and terminate the process cleanly.
// Calling std::exit() from inside the signal handler hangs (unsafe), so on trip
// the faulting thread just parks itself here.
static std::atomic<uint64_t> last_fault_context_{0};
static std::atomic<bool> livelock_tripped_{false};
// All 16 host GPRs captured at the livelock trip. The guest r25 (MemHeap*) and
// r26 (request size) are live in host registers on the failure path but not
// reliably synced into the in-memory PPCContext (Xenia caches guest regs in host
// regs mid-block), so we snapshot the raw host register file and scan it.
static uint64_t last_fault_host_gprs_[16] = {0};

bool signal_handlers_installed_ = false;
struct sigaction original_sigill_handler_;
struct sigaction original_sigsegv_handler_;
struct sigaction original_sigbus_handler_;

// This can be as large as needed, but isn't often needed.
// As we will be sometimes firing many exceptions we want to avoid having to
// scan the table too much or invoke many custom handlers.
constexpr size_t kMaxHandlerCount = 8;

// All custom handlers, left-aligned and null terminated.
// Executed in order.
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

static void ExceptionHandlerCallback(int signal_number, siginfo_t* signal_info,
                                     void* signal_context) {
  mcontext_t& mcontext =
      reinterpret_cast<ucontext_t*>(signal_context)->uc_mcontext;

  HostThreadContext thread_context;

#if XE_ARCH_AMD64
  thread_context.rip = uint64_t(mcontext.gregs[REG_RIP]);
  thread_context.eflags = uint32_t(mcontext.gregs[REG_EFL]);
  // The REG_ order may be different than the register indices in the
  // instruction encoding.
  thread_context.rax = uint64_t(mcontext.gregs[REG_RAX]);
  thread_context.rcx = uint64_t(mcontext.gregs[REG_RCX]);
  thread_context.rdx = uint64_t(mcontext.gregs[REG_RDX]);
  thread_context.rbx = uint64_t(mcontext.gregs[REG_RBX]);
  thread_context.rsp = uint64_t(mcontext.gregs[REG_RSP]);
  thread_context.rbp = uint64_t(mcontext.gregs[REG_RBP]);
  thread_context.rsi = uint64_t(mcontext.gregs[REG_RSI]);
  thread_context.rdi = uint64_t(mcontext.gregs[REG_RDI]);
  thread_context.r8 = uint64_t(mcontext.gregs[REG_R8]);
  thread_context.r9 = uint64_t(mcontext.gregs[REG_R9]);
  thread_context.r10 = uint64_t(mcontext.gregs[REG_R10]);
  thread_context.r11 = uint64_t(mcontext.gregs[REG_R11]);
  thread_context.r12 = uint64_t(mcontext.gregs[REG_R12]);
  thread_context.r13 = uint64_t(mcontext.gregs[REG_R13]);
  thread_context.r14 = uint64_t(mcontext.gregs[REG_R14]);
  thread_context.r15 = uint64_t(mcontext.gregs[REG_R15]);
  std::memcpy(thread_context.xmm_registers, mcontext.fpregs->_xmm,
              sizeof(thread_context.xmm_registers));
#elif XE_ARCH_ARM64
  std::memcpy(thread_context.x, mcontext.regs, sizeof(thread_context.x));
  thread_context.sp = mcontext.sp;
  thread_context.pc = mcontext.pc;
  thread_context.pstate = mcontext.pstate;
  struct fpsimd_context* mcontext_fpsimd = nullptr;
  struct esr_context* mcontext_esr = nullptr;
  for (struct _aarch64_ctx* mcontext_extension =
           reinterpret_cast<struct _aarch64_ctx*>(mcontext.__reserved);
       mcontext_extension->magic;
       mcontext_extension = reinterpret_cast<struct _aarch64_ctx*>(
           reinterpret_cast<uint8_t*>(mcontext_extension) +
           mcontext_extension->size)) {
    switch (mcontext_extension->magic) {
      case FPSIMD_MAGIC:
        mcontext_fpsimd =
            reinterpret_cast<struct fpsimd_context*>(mcontext_extension);
        break;
      case ESR_MAGIC:
        mcontext_esr =
            reinterpret_cast<struct esr_context*>(mcontext_extension);
        break;
      default:
        break;
    }
  }
  assert_not_null(mcontext_fpsimd);
  if (mcontext_fpsimd) {
    thread_context.fpsr = mcontext_fpsimd->fpsr;
    thread_context.fpcr = mcontext_fpsimd->fpcr;
    std::memcpy(thread_context.v, mcontext_fpsimd->vregs,
                sizeof(thread_context.v));
  }
#endif  // XE_ARCH

  Exception ex;
  switch (signal_number) {
    case SIGILL:
      ex.InitializeIllegalInstruction(&thread_context);
      break;
    case SIGBUS:
    case SIGSEGV: {
      Exception::AccessViolationOperation access_violation_operation;
#if XE_ARCH_AMD64
      // x86_pf_error_code::X86_PF_WRITE
      constexpr uint64_t kX86PageFaultErrorCodeWrite = UINT64_C(1) << 1;
      access_violation_operation =
          (uint64_t(mcontext.gregs[REG_ERR]) & kX86PageFaultErrorCodeWrite)
              ? Exception::AccessViolationOperation::kWrite
              : Exception::AccessViolationOperation::kRead;
#elif XE_ARCH_ARM64
      // For a Data Abort (EC - ESR_EL1 bits 31:26 - 0b100100 from a lower
      // Exception Level, 0b100101 without a change in the Exception Level),
      // bit 6 is 0 for reading from a memory location, 1 for writing to a
      // memory location.
      if (mcontext_esr && ((mcontext_esr->esr >> 26) & 0b111110) == 0b100100) {
        access_violation_operation =
            (mcontext_esr->esr & (UINT64_C(1) << 6))
                ? Exception::AccessViolationOperation::kWrite
                : Exception::AccessViolationOperation::kRead;
      } else {
        // Determine the memory access direction based on which instruction has
        // requested it.
        // esr_context may be unavailable on certain hosts (for instance, on
        // Android, it was added only in NDK r16 - which is the first NDK
        // version to support the Android API level 27, while NDK r15 doesn't
        // have esr_context in its API 26 sigcontext.h).
        // On AArch64 (unlike on AArch32), the program counter is the address of
        // the currently executing instruction.
        bool instruction_is_store;
        if (IsArm64LoadPrefetchStore(
                *reinterpret_cast<const uint32_t*>(mcontext.pc),
                instruction_is_store)) {
          access_violation_operation =
              instruction_is_store ? Exception::AccessViolationOperation::kWrite
                                   : Exception::AccessViolationOperation::kRead;
        } else {
          assert_always(
              "No ESR in the exception thread context, or it's not a Data "
              "Abort, and the faulting instruction is not a known load, "
              "prefetch or store instruction");
          access_violation_operation =
              Exception::AccessViolationOperation::kUnknown;
        }
      }
#else
      access_violation_operation =
          Exception::AccessViolationOperation::kUnknown;
#endif  // XE_ARCH
      sigsegv_count_.fetch_add(1, std::memory_order_relaxed);
      last_fault_address_.store(reinterpret_cast<uint64_t>(signal_info->si_addr),
                                std::memory_order_relaxed);
#if XE_ARCH_AMD64
      last_fault_rip_.store(uint64_t(mcontext.gregs[REG_RIP]),
                            std::memory_order_relaxed);
      // Livelock circuit-breaker: detect a recovered fault that re-executes the
      // same host instruction at the same faulting address with no progress
      // (e.g. RB3 Deluxe's MemHeap::Alloc post-OOM store to guest 0xFFFFFFFC,
      // which the soft-fault path "recovers" without advancing the guest PC).
      if (cvars::fault_spin_limit != 0) {
        thread_local uint64_t tls_prev_rip = 0;
        thread_local uint64_t tls_prev_addr = 0;
        thread_local uint64_t tls_repeat = 0;
        uint64_t cur_rip = uint64_t(mcontext.gregs[REG_RIP]);
        uint64_t cur_addr = reinterpret_cast<uint64_t>(signal_info->si_addr);
        if (cur_rip == tls_prev_rip && cur_addr == tls_prev_addr) {
          if (++tls_repeat >= cvars::fault_spin_limit) {
            // Capture the faulting thread's guest context. Xenia's x64 backend
            // reserves rsi = PPCContext* and rdi = membase for the whole of JIT
            // execution, so at any in-JIT fault rsi is a live, valid context
            // pointer. A safe thread (headless status loop) decodes r25/r26 +
            // the exhausted-heap name from it and terminates the process.
            last_fault_context_.store(uint64_t(mcontext.gregs[REG_RSI]),
                                      std::memory_order_relaxed);
            // Snapshot all host GPRs (order: RAX RCX RDX RBX RSP RBP RSI RDI
            // R8..R15) for the diagnosis scan.
            last_fault_host_gprs_[0] = uint64_t(mcontext.gregs[REG_RAX]);
            last_fault_host_gprs_[1] = uint64_t(mcontext.gregs[REG_RCX]);
            last_fault_host_gprs_[2] = uint64_t(mcontext.gregs[REG_RDX]);
            last_fault_host_gprs_[3] = uint64_t(mcontext.gregs[REG_RBX]);
            last_fault_host_gprs_[4] = uint64_t(mcontext.gregs[REG_RSP]);
            last_fault_host_gprs_[5] = uint64_t(mcontext.gregs[REG_RBP]);
            last_fault_host_gprs_[6] = uint64_t(mcontext.gregs[REG_RSI]);
            last_fault_host_gprs_[7] = uint64_t(mcontext.gregs[REG_RDI]);
            last_fault_host_gprs_[8] = uint64_t(mcontext.gregs[REG_R8]);
            last_fault_host_gprs_[9] = uint64_t(mcontext.gregs[REG_R9]);
            last_fault_host_gprs_[10] = uint64_t(mcontext.gregs[REG_R10]);
            last_fault_host_gprs_[11] = uint64_t(mcontext.gregs[REG_R11]);
            last_fault_host_gprs_[12] = uint64_t(mcontext.gregs[REG_R12]);
            last_fault_host_gprs_[13] = uint64_t(mcontext.gregs[REG_R13]);
            last_fault_host_gprs_[14] = uint64_t(mcontext.gregs[REG_R14]);
            last_fault_host_gprs_[15] = uint64_t(mcontext.gregs[REG_R15]);
            XELOGE(
                "FAULT LIVELOCK: {} consecutive recovered faults at host rip "
                "{:016X}, fault addr {:016X} (guest EA {:08X}) with no "
                "progress -- resume-without-advance wedge. This is a fatal "
                "guest fault (likely heap OOM); ctx(rsi)={:016X}. Parking "
                "thread; see jit-fault-wiki/09-rb3dx-title-to-menu.md.",
                tls_repeat + 1, cur_rip, cur_addr,
                uint32_t(cur_addr & 0xFFFFFFFFull),
                uint64_t(mcontext.gregs[REG_RSI]));
            livelock_tripped_.store(true, std::memory_order_release);
            // Do NOT std::exit() from a signal handler (unsafe, hangs). Park the
            // faulting thread so the spin stops; the main/headless thread reads
            // the flag and terminates cleanly. nanosleep is async-signal-safe.
            struct timespec park_ts = {0, 100 * 1000 * 1000};  // 100ms
            while (true) {
              nanosleep(&park_ts, nullptr);
            }
          }
        } else {
          tls_prev_rip = cur_rip;
          tls_prev_addr = cur_addr;
          tls_repeat = 0;
        }
      }
#endif
      ex.InitializeAccessViolation(
          &thread_context, reinterpret_cast<uint64_t>(signal_info->si_addr),
          access_violation_operation);
    } break;
    default:
      assert_unhandled_case(signal_number);
  }

  bool handled = false;
  for (size_t i = 0; i < xe::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      handled = true;
      // Exception handled.
#if XE_ARCH_AMD64
      mcontext.gregs[REG_RIP] = greg_t(thread_context.rip);
      mcontext.gregs[REG_EFL] = greg_t(thread_context.eflags);
      uint32_t modified_register_index;
      // The order must match the order in X64Register.
      static const size_t kIntRegisterMap[] = {
          REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
          REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
          REG_R12, REG_R13, REG_R14, REG_R15,
      };
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (xe::bit_scan_forward(modified_int_registers_remaining,
                                  &modified_register_index)) {
        modified_int_registers_remaining &=
            ~(UINT16_C(1) << modified_register_index);
        mcontext.gregs[kIntRegisterMap[modified_register_index]] =
            thread_context.int_registers[modified_register_index];
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (xe::bit_scan_forward(modified_xmm_registers_remaining,
                                  &modified_register_index)) {
        modified_xmm_registers_remaining &=
            ~(UINT16_C(1) << modified_register_index);
        std::memcpy(&mcontext.fpregs->_xmm[modified_register_index],
                    &thread_context.xmm_registers[modified_register_index],
                    sizeof(vec128_t));
      }
#elif XE_ARCH_ARM64
      uint32_t modified_register_index;
      uint32_t modified_x_registers_remaining = ex.modified_x_registers();
      while (xe::bit_scan_forward(modified_x_registers_remaining,
                                  &modified_register_index)) {
        modified_x_registers_remaining &=
            ~(UINT32_C(1) << modified_register_index);
        mcontext.regs[modified_register_index] =
            thread_context.x[modified_register_index];
      }
      mcontext.sp = thread_context.sp;
      mcontext.pc = thread_context.pc;
      mcontext.pstate = thread_context.pstate;
      if (mcontext_fpsimd) {
        mcontext_fpsimd->fpsr = thread_context.fpsr;
        mcontext_fpsimd->fpcr = thread_context.fpcr;
        uint32_t modified_v_registers_remaining = ex.modified_v_registers();
        while (xe::bit_scan_forward(modified_v_registers_remaining,
                                    &modified_register_index)) {
          modified_v_registers_remaining &=
              ~(UINT32_C(1) << modified_register_index);
          std::memcpy(&mcontext_fpsimd->vregs[modified_register_index],
                      &thread_context.v[modified_register_index],
                      sizeof(vec128_t));
          mcontext.regs[modified_register_index] =
              thread_context.x[modified_register_index];
        }
      }
#endif  // XE_ARCH
      return;
    }
  }

  if (!handled) {
    // No handler handled this exception. Restore the original signal handler
    // and return — the faulting instruction will re-execute, and the original
    // handler (or SIG_DFL) will produce a proper crash/core dump.
    if (signal_number == SIGSEGV) {
      sigaction(SIGSEGV, &original_sigsegv_handler_, nullptr);
    } else if (signal_number == SIGBUS) {
      sigaction(SIGBUS, &original_sigbus_handler_, nullptr);
    } else if (signal_number == SIGILL) {
      sigaction(SIGILL, &original_sigill_handler_, nullptr);
    }
  }
}

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!signal_handlers_installed_) {
    struct sigaction signal_handler;

    std::memset(&signal_handler, 0, sizeof(signal_handler));
    signal_handler.sa_sigaction = ExceptionHandlerCallback;
    signal_handler.sa_flags = SA_SIGINFO;

    if (sigaction(SIGILL, &signal_handler, &original_sigill_handler_) != 0) {
      assert_always("Failed to install new SIGILL handler");
    }
    if (sigaction(SIGSEGV, &signal_handler, &original_sigsegv_handler_) != 0) {
      assert_always("Failed to install new SIGSEGV handler");
    }
    if (sigaction(SIGBUS, &signal_handler, &original_sigbus_handler_) != 0) {
      assert_always("Failed to install new SIGBUS handler");
    }
    signal_handlers_installed_ = true;
  }

  for (size_t i = 0; i < xe::countof(handlers_); ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < xe::countof(handlers_); ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < xe::countof(handlers_) - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }

  bool has_any = false;
  for (size_t i = 0; i < xe::countof(handlers_); ++i) {
    if (handlers_[i].first) {
      has_any = true;
      break;
    }
  }
  if (!has_any) {
    if (signal_handlers_installed_) {
      if (sigaction(SIGILL, &original_sigill_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGILL handler");
      }
      if (sigaction(SIGSEGV, &original_sigsegv_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGSEGV handler");
      }
      if (sigaction(SIGBUS, &original_sigbus_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGBUS handler");
      }
      signal_handlers_installed_ = false;
    }
  }
}

uint64_t ExceptionHandler::GetSigsegvCount() {
  return sigsegv_count_.load(std::memory_order_relaxed);
}

uint64_t ExceptionHandler::GetLastFaultAddress() {
  return last_fault_address_.load(std::memory_order_relaxed);
}

uint64_t ExceptionHandler::GetLastFaultRip() {
  return last_fault_rip_.load(std::memory_order_relaxed);
}

uint64_t ExceptionHandler::GetLastFaultContext() {
  return last_fault_context_.load(std::memory_order_relaxed);
}

uint64_t ExceptionHandler::GetLastFaultHostGpr(int index) {
  if (index < 0 || index >= 16) return 0;
  return last_fault_host_gprs_[index];
}

bool ExceptionHandler::IsLivelockTripped() {
  return livelock_tripped_.load(std::memory_order_acquire);
}

}  // namespace xe
