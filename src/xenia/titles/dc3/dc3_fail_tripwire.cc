/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 Debug::Fail tripwire (NOT upstream). See dc3_fail_tripwire.h.
 ******************************************************************************
 */

#include "xenia/titles/dc3/dc3_fail_tripwire.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include "xenia/base/byte_order.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/debug_print_observer.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/titles/dc3/dc3_flags.h"
#include "xenia/titles/dc3/dc3_hack_pack.h"
#include "xenia/titles/dc3/dc3_hacks.h"
#include "xenia/titles/probe_threads.h"

DEFINE_bool(dc3_fail_tripwire, true,
            "DC3 (original debug.xex): watch TheDebug from a read-only host "
            "thread and log 'DC3 TRIPWIRE ... TAINTED' when Debug::mFailing "
            "latches or mFailThreadMsg is set (every later MILO_FAIL is then "
            "silent), with the failure message and its captured guest stack. "
            "Also logs the DC3 override audit every 30 s.",
            "DC3");

namespace xe {
namespace dc3 {

namespace {

// dc3-decomp src/system/os/Debug.h; TheDebug from symbols.txt (original).
constexpr uint32_t kTheDebug = 0x82F655D8;
constexpr uint32_t kFailingOff = 0x5;           // bool mFailing
constexpr uint32_t kTryOff = 0xC;               // int mTry
constexpr uint32_t kFailThreadStackOff = 0x3C;  // unsigned int[50]
constexpr uint32_t kFailThreadMsgOff = 0x104;   // const char*
constexpr int kStackWords = 50;

bool Readable(Memory* memory, uint32_t addr, uint32_t size) {
  if (addr < 0x00010000u || addr >= 0xF0000000u) {
    return false;
  }
  auto* heap = memory->LookupHeap(addr);
  if (!heap) {
    return false;
  }
  return heap->QueryRangeAccess(addr, addr + size - 1) !=
         xe::memory::PageAccess::kNoAccess;
}

std::string ReadCString(Memory* memory, uint32_t addr, size_t max) {
  std::string s;
  for (size_t i = 0; i < max; ++i) {
    uint32_t a = addr + static_cast<uint32_t>(i);
    if (!Readable(memory, a, 1)) {
      break;
    }
    char c = *memory->TranslateVirtual<char*>(a);
    if (!c) {
      break;
    }
    // One line, and no quote that would end the harness's '...' capture.
    s.push_back(c == '\n' || c == '\r' ? ' ' : (c == '\'' ? '"' : c));
  }
  return s;
}

// The failure text, from the game's own log. Debug::Modal(kModalFail) prints
// "FAIL-MSG: <msg>\n" (MILO_LOG -> Debug::Print -> OutputDebugStringA ->
// RtlDebugPrintHelper's debug-print trap) before it shows the modal, and a
// worker-thread Debug::Fail prints "THREAD-FAIL: <msg>\n". The trap's host
// handler only logs at debug level, so the text never reached a default-level
// log; this observer (cpu/debug_print_observer.h) reads it there. Read-only:
// the guest's print is unchanged.
std::mutex g_fail_text_mutex;
std::string g_fail_text;  // the last FAIL-MSG text, one line

std::string OneLine(std::string_view text, size_t max) {
  std::string s;
  for (char c : text.substr(0, max)) {
    // One line, and no quote that would end the harness's '...' capture.
    s.push_back(c == '\n' || c == '\r' ? ' ' : (c == '\'' ? '"' : c));
  }
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

void OnDebugPrint(uint32_t thread_id, std::string_view text) {
  constexpr std::string_view kFailMsg = "FAIL-MSG: ";
  constexpr std::string_view kThreadFail = "THREAD-FAIL: ";
  bool main_fail = text.substr(0, kFailMsg.size()) == kFailMsg;
  bool thread_fail = text.substr(0, kThreadFail.size()) == kThreadFail;
  if (!main_fail && !thread_fail) {
    return;
  }
  std::string msg = OneLine(
      text.substr(main_fail ? kFailMsg.size() : kThreadFail.size()), 1000);
  XELOGE("DC3 TRIPWIRE: guest {} (thread {:08X}): '{}'",
         main_fail ? "FAIL-MSG" : "THREAD-FAIL", thread_id, msg);
  if (main_fail) {
    std::lock_guard<std::mutex> lock(g_fail_text_mutex);
    g_fail_text = msg;
  }
}

// A main-thread Fail keeps its message in stack locals (Debug::Fail's
// StackString msgStr, Debug::Modal's modalMsg). Read-only: scan the guest
// main thread's stack above its SP for long printable runs and log the first
// few, so a main-thread fail says what failed, not just when.
std::string LogMainThreadStackStrings(Memory* memory,
                                      kernel::KernelState* ks) {
  std::string first;
  if (!ks) {
    return first;
  }
  for (auto& thread :
       ks->object_table()->GetObjectsByType<kernel::XThread>()) {
    if (!thread->main_thread() || !thread->thread_state()) {
      continue;
    }
    uint32_t sp =
        static_cast<uint32_t>(thread->thread_state()->context()->r[1]);
    int found = 0;
    std::string run;
    constexpr uint32_t kScanBytes = 0x8000;
    for (uint32_t a = sp; a < sp + kScanBytes && found < 4; ++a) {
      char c = Readable(memory, a, 1) ? *memory->TranslateVirtual<char*>(a)
                                      : '\0';
      if (c >= 0x20 && c < 0x7F) {
        run.push_back(c == '\'' ? '"' : c);
        continue;
      }
      if (run.size() >= 24) {
        if (first.empty()) {
          first = run.substr(0, 240);
        }
        XELOGE("DC3 TRIPWIRE: main thread {:08X} stack string @{:08X}: '{}'",
               thread->thread_id(), a - static_cast<uint32_t>(run.size()),
               run.substr(0, 240));
        ++found;
      }
      run.clear();
    }
    if (!found) {
      XELOGW("DC3 TRIPWIRE: no message found on main thread {:08X} stack "
             "(SP {:08X})",
             thread->thread_id(), sp);
    }
  }
  return first;
}

// True when TheUI->mCurrentScreen's name is `name` (original layout).
bool CurrentScreenNameIs(Memory* memory, const char* name) {
  constexpr uint32_t kTheUI = 0x82F1A8E0;
  if (!Readable(memory, kTheUI, 4)) return false;
  uint32_t ui = xe::load_and_swap<uint32_t>(
      memory->TranslateVirtual<uint8_t*>(kTheUI));
  if (!Readable(memory, ui + 0x48, 4)) return false;
  uint32_t scr =
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(ui + 0x48));
  size_t len = std::strlen(name);
  for (uint32_t off : {0x1Cu, 0x20u}) {
    if (!Readable(memory, scr + off, 4)) continue;
    uint32_t p = xe::load_and_swap<uint32_t>(
        memory->TranslateVirtual<uint8_t*>(scr + off));
    if (!Readable(memory, p, static_cast<uint32_t>(len + 1))) continue;
    if (std::memcmp(memory->TranslateVirtual<char*>(p), name, len + 1) == 0) {
      return true;
    }
  }
  return false;
}

// (RETIRED 2026-10-07, lane flow-wake) seq.controller_mode: TheGestureMgr->
// mInControllerMode := 1 every 100 ms (gap O6). The 360 boots outside
// controller mode (GestureMgr's ctor), and ShellInput swallows the first pad
// press there; the shared flow now says so itself with a `wake` (L3) before
// each screen's first press, as a player does (docs/fork/dc3/PATCH_MANIFEST.md).

void TripwireThread(Memory* memory, cpu::Processor* processor,
                    kernel::KernelState* kernel_state) {
  if (!Readable(memory, kTheDebug, kFailThreadMsgOff + 4)) {
    XELOGW("DC3 TRIPWIRE: TheDebug {:08X} not readable; tripwire off",
           kTheDebug);
    return;
  }
  auto* dbg = memory->TranslateVirtual<uint8_t*>(kTheDebug);
  uint8_t last_failing = 0;
  uint32_t last_msg = 0;
  auto start = std::chrono::steady_clock::now();
  auto last_audit = start;
  auto latched_since = start;
  bool latched_reported = false;
  bool main_scan_pending = false;
  auto last_faults = start;
  uint64_t last_segv = 0;
  uint64_t last_xma = 0;
  XELOGI("DC3 TRIPWIRE: watching TheDebug {:08X} (mFailing +0x5, "
         "mFailThreadMsg +0x104)",
         kTheDebug);
  uint32_t ticks = 0;
  while (titles::ProbeSleep(100)) {
    ++ticks;
    // IK telemetry (--dc3_ik_telemetry): ~1/s on game_screen. Read-only;
    // ReadDc3IKTelemetry guards every guest read. Was driven by the NUI
    // sequencer callback.
    if (cvars::dc3_ik_telemetry && ticks % 10 == 0 &&
        CurrentScreenNameIs(memory, "game_screen")) {
      ReadDc3IKTelemetry(memory, ticks / 10);
    }
    auto now = std::chrono::steady_clock::now();
    uint8_t failing = dbg[kFailingOff];
    uint32_t msg = xe::load_and_swap<uint32_t>(dbg + kFailThreadMsgOff);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - start)
                  .count();
    if (msg != last_msg) {
      std::string text =
          msg && Readable(memory, msg, 1) ? ReadCString(memory, msg, 400) : "";
      std::string stack;
      for (int i = 0; i < kStackWords; ++i) {
        uint32_t ra = xe::load_and_swap<uint32_t>(dbg + kFailThreadStackOff +
                                                  4 * i);
        if (!ra) {
          break;
        }
        stack += fmt::format(" {:08X}", ra);
      }
      if (msg) {
        XELOGE("DC3 TRIPWIRE: TAINTED at {}ms: worker-thread Debug::Fail "
               "mFailThreadMsg={:08X} '{}' mFailing={} mTry={} guest "
               "stack:{}",
               ms, msg, text, failing,
               xe::load_and_swap<uint32_t>(dbg + kTryOff), stack);
      } else {
        XELOGI("DC3 TRIPWIRE: mFailThreadMsg cleared at {}ms", ms);
      }
      last_msg = msg;
    }
    if (failing != last_failing) {
      if (failing) {
        latched_since = now;
        latched_reported = false;
        XELOGW("DC3 TRIPWIRE: Debug::mFailing 0->1 at {}ms (a MILO_FAIL is "
               "being handled)",
               ms);
        main_scan_pending = true;
      } else {
        XELOGI("DC3 TRIPWIRE: Debug::mFailing 1->0 at {}ms", ms);
        // That Fail is over; a later one must not inherit its text.
        std::lock_guard<std::mutex> lock(g_fail_text_mutex);
        g_fail_text.clear();
      }
      last_failing = failing;
    }
    // A main-thread Fail clears mFailing when its modal returns. Still set
    // after 2 s = latched: every MILO_FAIL from here on is a silent no-op.
    if (failing && !latched_reported &&
        now - latched_since >= std::chrono::seconds(2)) {
      latched_reported = true;
      if (main_scan_pending && !msg) {
        // No mFailThreadMsg: a MAIN-thread Fail, stuck in Debug::Modal
        // (kModalFail ends in Exit()). The message is the game's own
        // FAIL-MSG print when it got that far, else the first stack string;
        // logged in the harness's mFailThreadMsg='...' shape so
        // fork-regress records it with the worker fails.
        std::string text;
        {
          std::lock_guard<std::mutex> lock(g_fail_text_mutex);
          text = g_fail_text;
        }
        if (text.empty()) {
          text = LogMainThreadStackStrings(memory, kernel_state);
        }
        XELOGE("DC3 TRIPWIRE: TAINTED: main-thread Debug::Fail "
               "mFailThreadMsg={:08X} '{}'",
               0, text);
      }
      main_scan_pending = false;
      XELOGE("DC3 TRIPWIRE: TAINTED at {}ms: Debug::mFailing LATCHED (set "
             "for 2 s); every later MILO_FAIL is silent. mFailThreadMsg={:08X}",
             ms, msg);
    }
    // Fault counters for the harness: every guest store to the XMA register
    // aperture [0x7FEA0000,0x7FEB0000) is a trapped-and-emulated device
    // register write (recovered by construction), so SIGSEGV alone counts
    // real audio as faults. NON_XMA is the number that means "a guest access
    // faulted". Logged when it changes, at most every 3 s.
    if (now - last_faults >= std::chrono::seconds(3)) {
      last_faults = now;
      // XMA first, SIGSEGV second: the handler bumps SIGSEGV then XMA,
      // so SIGSEGV >= XMA here (exception_handler.h).
      uint64_t xma = ExceptionHandler::GetXmaSoftFaultCount();
      uint64_t segv = ExceptionHandler::GetSigsegvCount();
      if (segv != last_segv || xma != last_xma) {
        last_segv = segv;
        last_xma = xma;
        XELOGI("DC3 FAULTS ({}ms): SIGSEGV={} XMA={} NON_XMA={} "
               "last_non_xma=0x{:X} rip=0x{:X}",
               ms, segv, xma, segv - xma,
               ExceptionHandler::GetLastRealFaultAddress(),
               ExceptionHandler::GetLastRealFaultRip());
      }
    }
    if (now - last_audit >= std::chrono::seconds(30)) {
      last_audit = now;
      HackLogOverrideAudit(processor, true);
    }
  }
}

}  // namespace

void StartFailTripwire(Memory* memory, cpu::Processor* processor,
                       kernel::KernelState* kernel_state) {
  if (!cvars::dc3_fail_tripwire) {
    return;
  }
  cpu::SetDebugPrintObserver(&OnDebugPrint);
  titles::SpawnProbeThread([memory, processor, kernel_state] {
    TripwireThread(memory, processor, kernel_state);
  });
}

}  // namespace dc3
}  // namespace xe
