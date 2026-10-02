/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 guest main-thread hook (NOT upstream). See dc3_main_thread.h.
 ******************************************************************************
 */

#include "xenia/titles/dc3/dc3_main_thread.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/titles/dc3/dc3_hacks.h"

namespace xe {
namespace dc3 {

namespace {

// ORIGINAL debug.xex (symbols.txt; first four words from the dtk listing).
constexpr uint32_t kHolmesClientPollKeyboard = 0x825F0F78;
constexpr uint32_t kMainThread = 0x82330F30;  // bool MainThread()
constexpr uint32_t kGHolmesStream = 0x82F68C1C;
constexpr uint32_t kPollWords[4] = {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0,
                                    0x9421FFA0};
constexpr uint32_t kMainThreadWords[4] = {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0,
                                          0x9421FFA0};

struct Task {
  const char* name;
  MainThreadTask fn;
};

std::mutex g_mutex;
bool g_installed = false;
cpu::Processor* g_processor = nullptr;
Memory* g_memory = nullptr;
// Written only before the first poll (launch hooks); read on the main thread.
std::vector<Task> g_tasks;
std::atomic<uint64_t> g_polls{0};
std::atomic<uint32_t> g_main_tid{0};

bool WordsMatch(Memory* memory, uint32_t addr, const uint32_t (&words)[4]) {
  auto* p = memory->TranslateVirtual<uint8_t*>(addr);
  if (!p) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    uint32_t w = xe::load_and_swap<uint32_t>(p + 4 * i);
    if (w != words[i]) {
      XELOGE("DC3 main-thread hook: fingerprint mismatch at {:08X}+{} (have "
             "{:08X}, want {:08X}); not the original debug.xex",
             addr, 4 * i, w, words[i]);
      return false;
    }
  }
  return true;
}

void PollExtern(cpu::ppc::PPCContext* ctx, kernel::KernelState*) {
  HackCountOverrideHit(kHolmesClientPollKeyboard);
  auto* ts = ctx->thread_state;
  uint64_t n = ++g_polls;
  uint32_t tid = kernel::XThread::GetCurrentThreadId();
  const cpu::ppc::PPCContext saved = *ctx;
  if (n == 1) {
    uint32_t is_main = static_cast<uint32_t>(
        g_processor->Execute(ts, kMainThread, nullptr, 0));
    *ctx = saved;
    g_main_tid = tid;
    uint32_t holmes = xe::load_and_swap<uint32_t>(
        g_memory->TranslateVirtual<uint8_t*>(kGHolmesStream));
    auto* xt = kernel::XThread::GetCurrentThread();
    XELOGI("DC3 main-thread hook: first poll on guest thread {:08X} ('{}'); "
           "guest MainThread()={} gHolmesStream={:08X}; {} task(s)",
           tid, xt ? xt->name() : "?", is_main & 0xFF, holmes, g_tasks.size());
    if (holmes) {
      XELOGE("DC3 main-thread hook: gHolmesStream is SET: the skipped stock "
             "HolmesClientPollKeyboard body would have polled a Holmes "
             "connection (TAINTED)");
    }
  }
  if (n % 1800 == 0) {
    // The frame rate the `+N` flow offsets are measured in.
    static auto s_last = std::chrono::steady_clock::now();
    static uint64_t s_last_n = 0;
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - s_last).count();
    XELOGI("DC3 main-thread hook: frame {} ({:.1f} frames/s over the last {})",
           n, secs > 0 ? (n - s_last_n) / secs : 0.0, n - s_last_n);
    s_last = now;
    s_last_n = n;
  }
  if (tid != g_main_tid) {
    static std::atomic<int> warned{0};
    if (warned++ < 5) {
      XELOGW("DC3 main-thread hook: poll from UNEXPECTED guest thread {:08X} "
             "(first poll was {:08X}); tasks not run",
             tid, g_main_tid.load());
    }
    return;
  }
  // A task's guest call can reach SystemPoll -> KeyboardPoll again (a load
  // that pumps the frame loop); never run the tasks re-entrantly.
  static thread_local int t_depth = 0;
  if (t_depth > 0) {
    static std::atomic<int> nested{0};
    if (nested++ < 5) {
      XELOGW("DC3 main-thread hook: re-entered from a task's guest call "
             "(poll #{}); tasks skipped",
             n);
    }
    return;
  }
  ++t_depth;
  for (const auto& task : g_tasks) {
    task.fn(ts, n);
    *ctx = saved;
  }
  --t_depth;
}

}  // namespace

bool AddMainThreadTask(cpu::Processor* processor, Memory* memory,
                       const char* name, MainThreadTask task) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_installed) {
    if (!WordsMatch(memory, kHolmesClientPollKeyboard, kPollWords) ||
        !WordsMatch(memory, kMainThread, kMainThreadWords)) {
      XELOGE("DC3 main-thread hook: NOT installed; task '{}' dropped", name);
      return false;
    }
    g_processor = processor;
    g_memory = memory;
    HackRegisterOverride(processor, kHolmesClientPollKeyboard, &PollExtern,
                         "main_thread.holmes_poll_keyboard",
                         "dc3_main_thread_poll");
    g_installed = true;
    XELOGI("DC3 main-thread hook: installed on HolmesClientPollKeyboard "
           "{:08X}",
           kHolmesClientPollKeyboard);
  }
  g_tasks.push_back({name, task});
  XELOGI("DC3 main-thread hook: task '{}' added", name);
  return true;
}

uint32_t MainThreadId() { return g_main_tid.load(); }

bool MainThreadHookInstalled() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_installed;
}

uint64_t MainThreadFrame() { return g_polls.load(); }

}  // namespace dc3
}  // namespace xe
