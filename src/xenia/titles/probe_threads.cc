/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Host probe threads owned on behalf of title modules (NOT upstream).
 *
 * See probe_threads.h.
 ******************************************************************************
 */

#include "xenia/titles/probe_threads.h"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace xe {
namespace titles {

namespace {
std::mutex s_probe_thread_mutex;
std::vector<std::thread> s_probe_threads;
std::atomic<bool> s_probe_threads_stop{false};
}  // namespace

namespace probe_threads_detail {
std::mutex& Mutex() { return s_probe_thread_mutex; }
std::vector<std::thread>& Threads() { return s_probe_threads; }
}  // namespace probe_threads_detail

bool ProbeThreadsShouldStop() {
  return s_probe_threads_stop.load(std::memory_order_relaxed);
}

bool ProbeSleep(uint64_t ms) {
  for (uint64_t waited = 0; waited < ms; waited += 250) {
    if (ProbeThreadsShouldStop()) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(
        std::min<uint64_t>(250, ms - waited)));
  }
  return !ProbeThreadsShouldStop();
}

void JoinProbeThreads() {
  s_probe_threads_stop.store(true, std::memory_order_relaxed);
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> lock(s_probe_thread_mutex);
    threads.swap(s_probe_threads);
  }
  for (auto& t : threads) {
    if (t.joinable()) {
      t.join();
    }
  }
}

}  // namespace titles
}  // namespace xe
