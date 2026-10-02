/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914): MemAlloc / MemFree instrumentation (NOT
 * upstream). --rb3dx_alloc_trace_path (binary per-call trace) and
 * --rb3dx_alloc_probe (log suspicious requests).
 ******************************************************************************
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"
#include "xenia/titles/probe_threads.h"
#include "xenia/titles/rb3/rb3_flags.h"
#include "xenia/titles/rb3/rb3_internal.h"
#include "xenia/titles/title_hooks.h"

namespace xe {
namespace titles {
namespace rb3 {

namespace {

// Heap-"main" fragmentation attribution (--rb3dx_alloc_trace_path).
//
// A per-allocation binary trace. Every record is 32 bytes, little-endian host
// order, appended to one file:
//
//   u8  tag    1 = MemAlloc entry, 2 = MemAlloc return, 3 = MemFree entry,
//              4 = MemAlloc caller-of-caller frames
//   u8  pad
//   u16 tid    guest thread id (low 16 bits)
//   u32 seq    MemAlloc ordinal (tags 1, 2, 4 share it; MemFree ordinal for 3)
//   u64 ns     steady_clock ns since the sink opened
//   u32 a      tag1: size (r3)      tag2: returned pointer (r3)  tag3: ptr (r3)
//   u32 b      tag1: align (r4)     tag2: 0                      tag3: header
//   u32 lr     caller LR (r12 at the callee's entry)
//   u32 sp     caller SP (r1 at the callee's entry, before its stwu)
//
// Why binary: the measured rate is ~876 MemAlloc/s mean and 1900/s peak, so a
// formatted log line per call would cost more than the emulation. A record is
// a memcpy under a mutex into a 1 MiB-buffered FILE.
struct TraceRec {
  uint8_t tag;
  uint8_t pad;
  uint16_t tid;
  uint32_t seq;
  uint64_t ns;
  uint32_t a;
  uint32_t b;
  uint32_t lr;
  uint32_t sp;
};
static_assert(sizeof(TraceRec) == 32, "trace record must stay 32 bytes");

class AllocTraceSink {
 public:
  explicit AllocTraceSink(const std::string& path)
      : f_(fopen(path.c_str(), "wb")), t0_(std::chrono::steady_clock::now()) {
    if (f_) {
      setvbuf(f_, nullptr, _IOFBF, 1 << 20);
    }
  }
  ~AllocTraceSink() {
    if (f_) {
      fflush(f_);
      fclose(f_);
    }
  }
  bool ok() const { return f_ != nullptr; }
  uint64_t written() const { return count_.load(std::memory_order_relaxed); }

  void Emit(uint8_t tag, uint32_t tid, uint32_t seq, uint32_t a, uint32_t b,
            uint32_t lr, uint32_t sp) {
    if (!f_) {
      return;
    }
    TraceRec r{};
    r.tag = tag;
    r.tid = static_cast<uint16_t>(tid);
    r.seq = seq;
    r.ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0_)
            .count());
    r.a = a;
    r.b = b;
    r.lr = lr;
    r.sp = sp;
    std::lock_guard<std::mutex> g(m_);
    fwrite(&r, sizeof(r), 1, f_);
    count_.fetch_add(1, std::memory_order_relaxed);
  }
  void Flush() {
    std::lock_guard<std::mutex> g(m_);
    if (f_) {
      fflush(f_);
    }
  }

 private:
  FILE* f_;
  std::chrono::steady_clock::time_point t0_;
  std::mutex m_;
  std::atomic<uint64_t> count_{0};
};

// Published before the guest module is launched and kept for the process
// lifetime: the guest-thread handlers read it without synchronisation, and
// guest threads may still be running when the title terminates.
std::unique_ptr<AllocTraceSink> trace_sink;

// Per-guest-thread record of the MemAlloc call currently in flight, so that
// the __restgprlr_23 epilogue override can attribute the returned pointer back
// to the entry record. MemAlloc cannot recurse into itself, so one slot per
// thread is exact.
struct PendingAlloc {
  bool active = false;
  uint32_t sp = 0;   // MemAlloc's entry SP (== r1 at the epilogue restore)
  uint32_t lr = 0;   // caller LR (== the word reloaded from [sp-8])
  uint32_t seq = 0;  // MemAlloc ordinal of the entry record
};
thread_local PendingAlloc t_pending_alloc;

std::atomic<uint32_t> memfree_calls{0};
std::atomic<uint64_t> ret_matches{0};

uint32_t GuestTid(cpu::ppc::PPCContext* ppc_context) {
  return ppc_context->thread_state ? ppc_context->thread_state->thread_id()
                                   : 0;
}

// MemFree@0x827BC430 entry probe (--rb3dx_free_trace).
//
// Same recipe as the MemAlloc side, one helper down the save chain: MemFree's
// prologue is `mflr r12 ; bl 0x82829250 ; addi r31,r1,-0x90 ; stwu r1,-0x90(r1)`,
// i.e. it calls __savegprlr_26 and returns to 0x827BC438 -- a unique filter key.
// The handler emulates __savegprlr_26 exactly (std r26..r31 at r1-0x38..r1-0x10,
// stw r12 at r1-8) so every other caller of that helper is unaffected.
// At the filter point r3 = the pointer being freed, r12 = the caller LR.
void SaveGprLr26FreeExtern(cpu::ppc::PPCContext* ppc_context,
                           kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) {
    return;
  }
  uint8_t* base = kernel_state->memory()->virtual_membase();
  uint32_t sp = static_cast<uint32_t>(ppc_context->r[1]);
  // --- exact __savegprlr_26 emulation (must be semantically transparent) ---
  for (int i = 0; i < 6; ++i) {
    xe::store_and_swap<uint64_t>(base + sp - 0x38 + i * 8,
                                 ppc_context->r[26 + i]);
  }
  xe::store_and_swap<uint32_t>(base + sp - 8,
                               static_cast<uint32_t>(ppc_context->r[12]));
  const uint32_t kMemFreeRet = 0x827BC438;  // bl at 0x827BC434 in MemFree
  if (static_cast<uint32_t>(ppc_context->lr) != kMemFreeRet) {
    return;
  }
  auto* sink = trace_sink.get();
  if (!sink) {
    return;
  }
  uint32_t ptr = static_cast<uint32_t>(ppc_context->r[3]);
  // The allocated-block header is the word immediately below the payload:
  // (totalUsedWords << 8) | (padWords << 4) | flags. Only read it for pointers
  // that plausibly live in a guest heap arena, so a MemFree(NULL) or a wild
  // pointer can never fault the host.
  uint32_t header = 0;
  if (ptr >= 0x30000000u && ptr < 0x60000000u && (ptr & 3u) == 0u) {
    header = xe::load_and_swap<uint32_t>(base + ptr - 4);
  }
  uint32_t seq = memfree_calls.fetch_add(1, std::memory_order_relaxed);
  sink->Emit(3, GuestTid(ppc_context), seq, ptr, header,
             static_cast<uint32_t>(ppc_context->r[12]), sp);
}

// MemAlloc RETURN probe (--rb3dx_ret_trace).
//
// MemAlloc's epilogue is `mr r3,r30 ; addi r1,r31,0xb0 ; b 0x82c5ffc0`, and
// that thunk lands in __restgprlr_23 @0x82829294 (ld r23..r31 from
// r1-0x50..r1-0x10, lwz r12,-8(r1), mtlr r12, blr). Overriding it gives us r3,
// the pointer handed back -- which says whether a caller's blocks land at the
// FirstFit bottom or the LastFit top of the arena. The override is exact, and
// Xenia compiles the guest tail-branch as CallExtern + jmp epilog, so control
// still unwinds to MemAlloc's caller.
//
// Attribution filter: this helper is shared by every function that saved
// r23..r31, so we match against the per-thread pending slot -- r1 must equal
// the SP recorded at MemAlloc's entry AND the LR being restored must equal
// that call's caller LR.
void RestGprLr23RetExtern(cpu::ppc::PPCContext* ppc_context,
                          kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) {
    return;
  }
  uint8_t* base = kernel_state->memory()->virtual_membase();
  uint32_t sp = static_cast<uint32_t>(ppc_context->r[1]);
  // --- exact __restgprlr_23 emulation ---
  for (int i = 0; i < 9; ++i) {
    ppc_context->r[23 + i] =
        xe::load_and_swap<uint64_t>(base + sp - 0x50 + i * 8);
  }
  uint32_t restored_lr = xe::load_and_swap<uint32_t>(base + sp - 8);
  ppc_context->r[12] = restored_lr;
  ppc_context->lr = restored_lr;
  // --- probe part ---
  auto& p = t_pending_alloc;
  if (!p.active || p.sp != sp || p.lr != restored_lr) {
    return;
  }
  p.active = false;
  auto* sink = trace_sink.get();
  if (!sink) {
    return;
  }
  ret_matches.fetch_add(1, std::memory_order_relaxed);
  sink->Emit(2, GuestTid(ppc_context), p.seq,
             static_cast<uint32_t>(ppc_context->r[3]), 0, restored_lr, sp);
}

void InstallExactOverride(const TitleLaunchContext& ctx, uint32_t address,
                          uint32_t expected_word, cpu::GuestFunction::ExternHandler handler,
                          const char* name) {
  uint32_t w =
      xe::load_and_swap<uint32_t>(ctx.memory->virtual_membase() + address);
  if (w != expected_word) {
    XELOGW("RB3DX: {} NOT installed (unexpected word 0x{:08X} at 0x{:08X})",
           name, w, address);
    return;
  }
  ctx.processor->RegisterGuestFunctionOverride(address, handler, name);
  XELOGI("RB3DX: {} installed at 0x{:08X}", name, address);
}

}  // namespace

bool AllocTraceActive() { return trace_sink != nullptr; }

void AllocTraceOnMemAlloc(cpu::ppc::PPCContext* ppc_context, uint32_t seq,
                          uint32_t size, uint32_t sp) {
  auto* sink = trace_sink.get();
  if (!sink) {
    return;
  }
  uint8_t* base =
      ppc_context->kernel_state->memory()->virtual_membase();
  uint32_t tid = GuestTid(ppc_context);
  uint32_t caller_lr = static_cast<uint32_t>(ppc_context->r[12]);
  sink->Emit(1, tid, seq, size, static_cast<uint32_t>(ppc_context->r[4]),
             caller_lr, sp);
  // Caller-of-caller frames (tag 4, --rb3dx_stack_trace). The immediate
  // caller LR is not enough when the caller is a thin shim (the dominant
  // idle-churn site is XMemAlloc's `bl MemAlloc`). Every non-leaf here starts
  // `mflr r12 ; bl __savegprlr_N ; stwu r1,-F(r1)`, and __savegprlr_N stores
  // r12 at [entry_r1 - 8]; so for a frame at F_sp, [F_sp] is its caller's SP
  // and its own return address sits at (caller_sp - 8). Everything is
  // range-checked and the walk stops on the first implausible link.
  if (cvars::rb3dx_stack_trace) {
    uint32_t frames[4] = {0, 0, 0, 0};
    uint32_t f_sp = sp;
    for (int k = 0; k < 4; ++k) {
      if (f_sp < 0x40000000u || f_sp >= 0x80000000u || (f_sp & 3u)) {
        break;
      }
      uint32_t parent = xe::load_and_swap<uint32_t>(base + f_sp);
      if (parent <= f_sp || parent < 0x40000000u || parent >= 0x80000000u ||
          (parent & 3u)) {
        break;
      }
      uint32_t ret = xe::load_and_swap<uint32_t>(base + parent - 8);
      frames[k] = (ret >= 0x82000000u && ret < 0x83000000u) ? ret : 0;
      f_sp = parent;
    }
    sink->Emit(4, tid, seq, frames[0], frames[1], frames[2], frames[3]);
  }
  auto& p = t_pending_alloc;
  p.active = true;
  p.sp = sp;
  p.lr = caller_lr;
  p.seq = seq;
}

void AllocProbeOnMemAlloc(cpu::ppc::PPCContext* ppc_context, Memory* memory,
                          uint64_t seq, uint32_t size, uint32_t sp) {
  static std::atomic<uint32_t> s_reports{0};
  uint32_t align = static_cast<uint32_t>(ppc_context->r[4]);
  uint32_t caller_lr = static_cast<uint32_t>(ppc_context->r[12]);
  if (seq <= 5) {
    XELOGI(
        "RB3DX ALLOC PROBE: MemAlloc warmup #{} size=0x{:08X} align={} "
        "callerLR(r12)=0x{:08X} sp=0x{:08X}",
        seq, size, static_cast<int32_t>(align), caller_lr, sp);
  }
  bool suspicious = (size & 0xFF000000u) != 0;
  bool big = size >= 0x00400000u;  // >= 4 MiB: context even when top byte clean
  if (!suspicious && !big) {
    return;
  }
  if (s_reports.fetch_add(1, std::memory_order_relaxed) >= 64) {
    return;  // cap log volume; the first hits are the interesting ones
  }
  uint8_t* membase = memory->virtual_membase();
  auto readable = [&](uint32_t addr) -> bool {
    if (addr < 0x1000 || (addr & 3)) {
      return false;
    }
    auto* heap = memory->LookupHeap(addr);
    uint32_t prot = 0;
    return heap && heap->QueryProtect(addr, &prot) &&
           (prot & kMemoryProtectRead) != 0;
  };
  auto load32 = [&](uint32_t addr) -> uint32_t {
    return xe::load_and_swap<uint32_t>(membase + addr);
  };
  XELOGI(
      "RB3DX ALLOC PROBE: {} call #{} size=0x{:08X} ({}) align={} "
      "callerLR=0x{:08X} sp=0x{:08X}",
      suspicious ? "SUSPICIOUS" : "BIG", seq, size, size,
      static_cast<int32_t>(align), caller_lr, sp);
  // Guest stack backchain walk (MSVC Xenon: entry_sp = [sp], saved return
  // address = [entry_sp - 8]).
  uint32_t cur = sp;
  for (int i = 0; i < 10; ++i) {
    if (!readable(cur)) {
      break;
    }
    uint32_t prev = load32(cur);
    if (prev <= cur || prev - cur > 0x40000 || !readable(prev - 8)) {
      break;
    }
    XELOGI("RB3DX ALLOC PROBE:   frame[{}] entry_sp=0x{:08X} ret=0x{:08X}", i,
           prev, load32(prev - 8));
    cur = prev;
  }
  // Raw code-pointer scan of the caller stack window (covers leaf frames and
  // broken backchains).
  for (uint32_t a = sp & ~3u; a < sp + 0x180; a += 4) {
    if (!readable(a)) {
      break;
    }
    uint32_t v = load32(a);
    if (v >= 0x82000000 && v < 0x82F00000) {
      XELOGI("RB3DX ALLOC PROBE:   [sp+0x{:03X}]=0x{:08X}", a - sp, v);
    }
  }
  for (int i = 0; i < 32; i += 8) {
    std::string regs;
    for (int k = 0; k < 8; ++k) {
      regs += fmt::format(" {:08X}",
                          static_cast<uint32_t>(ppc_context->r[i + k]));
    }
    XELOGI("RB3DX ALLOC PROBE:   r{:<2}-r{:<2}{}", i, i + 7, regs);
  }
}

void InstallAllocTrace(const TitleLaunchContext& ctx) {
  if (cvars::rb3dx_alloc_trace_path.empty()) {
    return;
  }
  trace_sink = std::make_unique<AllocTraceSink>(cvars::rb3dx_alloc_trace_path);
  if (!trace_sink->ok()) {
    XELOGE("RB3DX: alloc trace could NOT open '{}' -- tracing disabled",
           cvars::rb3dx_alloc_trace_path);
    trace_sink.reset();
    return;
  }
  XELOGI("RB3DX: alloc trace -> '{}' (32-byte records)",
         cvars::rb3dx_alloc_trace_path);
  if (cvars::rb3dx_free_trace) {
    // std r26,-0x38(r1)
    InstallExactOverride(ctx, 0x82829250, 0xFB41FFC8, SaveGprLr26FreeExtern,
                         "RB3DX:__savegprlr_26(MemFree trace)");
  }
  if (cvars::rb3dx_ret_trace) {
    // ld r23,-0x50(r1)
    InstallExactOverride(ctx, 0x82829294, 0xEAE1FFB0, RestGprLr23RetExtern,
                         "RB3DX:__restgprlr_23(MemAlloc return trace)");
  }
  // Periodic flush + progress so a run killed on a wall-clock deadline still
  // leaves a complete-to-the-second trace on disk.
  SpawnProbeThread([]() {
    while (ProbeSleep(10000)) {
      auto* sink = trace_sink.get();
      if (!sink) {
        return;
      }
      sink->Flush();
      XELOGI("RB3DX alloc trace: {} records ({} alloc returns matched)",
             sink->written(), ret_matches.load(std::memory_order_relaxed));
    }
    if (auto* sink = trace_sink.get()) {
      sink->Flush();
    }
  });
}

}  // namespace rb3
}  // namespace titles
}  // namespace xe
