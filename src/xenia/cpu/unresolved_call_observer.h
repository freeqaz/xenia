/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Observer seam for the JIT's unresolved-call reports (NOT upstream).
 *
 * When the x64 backend hands a guest call a no-op stub (null target, failed
 * resolve, no machine code) or sees a call into executable memory outside
 * .text, it reports the event here. A title module that records such events
 * (titles/dc3/dc3_runtime_telemetry.cc) installs itself as the observer; the
 * CPU never depends on a title. With no observer installed every call below is
 * a no-op and IsActive() is false.
 ******************************************************************************
 */

#ifndef XENIA_CPU_UNRESOLVED_CALL_OBSERVER_H_
#define XENIA_CPU_UNRESOLVED_CALL_OBSERVER_H_

#include <cstdint>
#include <string_view>

namespace xe {
namespace cpu {

class UnresolvedCallObserver {
 public:
  virtual ~UnresolvedCallObserver() = default;
  // While false, the JIT skips work it would only do to report to the
  // observer (the non-.text target classification).
  virtual bool IsActive() const = 0;
  virtual void OnUnresolvedCallStubHit(std::string_view reason,
                                       uint32_t guest_addr,
                                       uint32_t callsite_pc) = 0;
};

// Installs (or, with nullptr, removes) the process-wide observer. The observer
// must outlive every JIT'd call that can report to it.
void SetUnresolvedCallObserver(UnresolvedCallObserver* observer);
bool UnresolvedCallObserverIsActive();
void NotifyUnresolvedCallStubHit(std::string_view reason, uint32_t guest_addr,
                                 uint32_t callsite_pc);

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_UNRESOLVED_CALL_OBSERVER_H_
