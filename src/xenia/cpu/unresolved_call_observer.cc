/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Observer seam for the JIT's unresolved-call reports (NOT upstream).
 * See unresolved_call_observer.h.
 ******************************************************************************
 */

#include "xenia/cpu/unresolved_call_observer.h"

#include <atomic>

namespace xe {
namespace cpu {

namespace {
std::atomic<UnresolvedCallObserver*> g_unresolved_call_observer{nullptr};
}  // namespace

void SetUnresolvedCallObserver(UnresolvedCallObserver* observer) {
  g_unresolved_call_observer.store(observer, std::memory_order_release);
}

bool UnresolvedCallObserverIsActive() {
  auto* observer = g_unresolved_call_observer.load(std::memory_order_acquire);
  return observer && observer->IsActive();
}

void NotifyUnresolvedCallStubHit(std::string_view reason, uint32_t guest_addr,
                                 uint32_t callsite_pc) {
  auto* observer = g_unresolved_call_observer.load(std::memory_order_acquire);
  if (observer) {
    observer->OnUnresolvedCallStubHit(reason, guest_addr, callsite_pc);
  }
}

}  // namespace cpu
}  // namespace xe
