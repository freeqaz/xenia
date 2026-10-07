/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Observer seam for the guest's debug-print trap (NOT upstream).
 * See debug_print_observer.h.
 ******************************************************************************
 */

#include "xenia/cpu/debug_print_observer.h"

#include <atomic>

namespace xe {
namespace cpu {

namespace {
std::atomic<DebugPrintObserver> g_debug_print_observer{nullptr};
}  // namespace

void SetDebugPrintObserver(DebugPrintObserver observer) {
  g_debug_print_observer.store(observer, std::memory_order_release);
}

void NotifyDebugPrint(uint32_t thread_id, std::string_view text) {
  auto observer = g_debug_print_observer.load(std::memory_order_acquire);
  if (observer) {
    observer(thread_id, text);
  }
}

}  // namespace cpu
}  // namespace xe
