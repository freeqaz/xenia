/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Host probe threads owned on behalf of title modules (NOT upstream).
 *
 * Moved out of emulator.cc (it was the RB3DX probe-thread block).
 ******************************************************************************
 */

#ifndef XENIA_TITLES_PROBE_THREADS_H_
#define XENIA_TITLES_PROBE_THREADS_H_

#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace xe {
namespace titles {

// Probe-thread ownership (fork-cleanup-review.md C10). These samplers used to
// be detached std::threads with for(;;) bodies capturing Memory*; ~Emulator()
// destroyed memory_ under them mid-LookupHeap — a guaranteed shutdown
// use-after-free on every probe-enabled run. They are now owned here: spawned
// via SpawnProbeThread(), polling ProbeSleep() instead of a bare
// sleep_for, and joined from Emulator::TerminateTitle() / ~Emulator() through the
// title modules' on_terminate_title / on_shutdown hooks.

namespace probe_threads_detail {
std::mutex& Mutex();
std::vector<std::thread>& Threads();
}  // namespace probe_threads_detail

bool ProbeThreadsShouldStop();

// Sleep in short slices so a stopping emulator never waits out a full probe
// period. Returns false (caller should exit) when a stop was requested.
bool ProbeSleep(uint64_t ms);

template <typename Fn>
void SpawnProbeThread(Fn&& fn) {
  std::lock_guard<std::mutex> lock(probe_threads_detail::Mutex());
  probe_threads_detail::Threads().emplace_back(std::forward<Fn>(fn));
}

void JoinProbeThreads();

}  // namespace titles
}  // namespace xe

#endif  // XENIA_TITLES_PROBE_THREADS_H_
