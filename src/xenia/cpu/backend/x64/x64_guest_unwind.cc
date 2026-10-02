/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/x64_guest_unwind.h"

#include <vector>

#include "xenia/base/logging.h"
#include "xenia/cpu/backend/x64/x64_code_cache.h"
#include "xenia/cpu/backend/x64/x64_function.h"
#include "xenia/cpu/backend/x64/x64_stack_layout.h"
#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

std::atomic<uint32_t> g_pending_host_return_count{0};
std::atomic<uint32_t> g_pending_host_returns_taken{0};

namespace {

struct PendingHostReturn {
  uint32_t guest_return_address;
  uint32_t guest_caller_sp;
  uint64_t slot;                 // host stack address holding the return
  uint64_t host_return_address;  // its value when armed (staleness check)
};

struct ThreadRecords {
  std::vector<PendingHostReturn> records;  // oldest first
  ~ThreadRecords() {
    g_pending_host_return_count.fetch_sub(uint32_t(records.size()));
  }
  void Erase(size_t first, size_t last) {
    g_pending_host_return_count.fetch_sub(uint32_t(last - first));
    records.erase(records.begin() + first, records.begin() + last);
  }
};
thread_local ThreadRecords t_records;

uint64_t Read64(uint64_t address) {
  return *reinterpret_cast<const uint64_t*>(address);
}

X64Function* JitFunctionAt(X64CodeCache* code_cache, uint64_t host_pc) {
  uint64_t base = code_cache->execute_base_address();
  if (host_pc < base || host_pc >= base + code_cache->total_size()) {
    return nullptr;
  }
  auto fn = code_cache->LookupFunction(host_pc);
  return fn ? static_cast<X64Function*>(fn) : nullptr;
}

// The guest return address a JIT frame's caller passed it.
uint32_t FrameGuestReturn(uint64_t frame_rsp) {
  return static_cast<uint32_t>(Read64(frame_rsp + StackLayout::GUEST_RET_ADDR));
}

}  // namespace

uint64_t TakePendingHostReturn(ppc::PPCContext_s* context, uint32_t target,
                               uint64_t jit_rsp) {
  auto& records = t_records.records;
  uint32_t guest_sp = static_cast<uint32_t>(context->r[1]);
  for (size_t i = records.size(); i-- > 0;) {
    auto& r = records[i];
    if (r.slot <= jit_rsp || Read64(r.slot) != r.host_return_address) {
      // Its frame is gone (popped normally, or discarded by a longjmp such
      // as the DTA throw hook's). Forget it.
      t_records.Erase(i, i + 1);
      continue;
    }
    if (r.guest_return_address == target && r.guest_caller_sp == guest_sp) {
      uint64_t slot = r.slot;
      // Everything newer lives in the frames being discarded.
      t_records.Erase(i, records.size());
      g_pending_host_returns_taken.fetch_add(1);
      return slot;
    }
  }
  return 0;
}

bool ArmPendingHostReturn(X64CodeCache* code_cache, uint64_t host_scan_from,
                          uint32_t first_guest_pc,
                          const Backend::GuestUnwindFrame* frames,
                          size_t frame_count, size_t target_index) {
  if (!frame_count || target_index >= frame_count) {
    return false;
  }
  // Find the JIT frame of frames[0]: the host return address its call into
  // the kernel export pushed lies a little above the export's own frame.
  uint64_t rsp = 0;
  X64Function* fn = nullptr;
  constexpr uint64_t kScanLimit = 512 * 1024;
  for (uint64_t a = host_scan_from & ~uint64_t(7);
       a < host_scan_from + kScanLimit; a += 8) {
    auto candidate = JitFunctionAt(code_cache, Read64(a));
    if (!candidate || !candidate->stack_size() ||
        first_guest_pc < candidate->address() ||
        first_guest_pc >= candidate->end_address()) {
      continue;
    }
    if (FrameGuestReturn(a + 8) == frames[0].guest_return_address) {
      rsp = a + 8;
      fn = candidate;
      break;
    }
  }
  if (!fn) {
    XELOGW("GuestEH: no JIT frame found for guest pc {:08X}", first_guest_pc);
    return false;
  }

  for (size_t i = 0;; ++i) {
    uint64_t slot = 0;
    if (FrameGuestReturn(rsp) == frames[i].guest_return_address) {
      slot = rsp + fn->stack_size();
    } else {
      // A frame that is itself a catch continuation (an earlier exception
      // was caught here) carries the wrong guest return address; its real
      // return path is the pending record armed for it.
      for (auto& r : t_records.records) {
        if (r.guest_return_address == frames[i].guest_return_address &&
            r.guest_caller_sp == frames[i].guest_caller_sp &&
            Read64(r.slot) == r.host_return_address) {
          slot = r.slot;
          break;
        }
      }
      if (!slot) {
        XELOGW(
            "GuestEH: host frame {} (rsp {:X}, fn {:08X}) returns to guest "
            "{:08X}, guest unwind says {:08X}; not arming",
            i, rsp, fn->address(), FrameGuestReturn(rsp),
            frames[i].guest_return_address);
        return false;
      }
    }
    if (i == target_index) {
      t_records.records.push_back({frames[i].guest_return_address,
                                   frames[i].guest_caller_sp, slot,
                                   Read64(slot)});
      g_pending_host_return_count.fetch_add(1);
      return true;
    }
    uint64_t host_return = Read64(slot);
    fn = JitFunctionAt(code_cache, host_return);
    if (!fn || !fn->stack_size()) {
      XELOGW(
          "GuestEH: host frame {} returns to {:X}, not JIT code; not arming",
          i, host_return);
      return false;
    }
    rsp = slot + 8;
  }
}

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
