/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * DC3 DTA evaluation channel (dc3-oracle). See dc3_dta_channel.h for the
 * contract and docs/dc3-oracle/SPIKE_LOG.md for the measurements behind it.
 */

#include "xenia/titles/dc3/dc3_dta_channel.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_cpp_throw_hook.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

DEFINE_string(dc3_dta_channel, "",
              "DC3 (original debug.xex only): unix socket path for the DTA "
              "evaluation channel. Requests are evaluated on the guest MAIN "
              "thread from an override of HolmesClientPollKeyboard. Empty = off "
              "(no override installed, no behaviour change).",
              "DC3");
DEFINE_int32(dc3_dta_channel_timeout_ms, 120000,
             "DC3 DTA channel: how long a request waits for the guest main "
             "thread to drain it before the socket answers 504.",
             "DC3");

namespace xe {

namespace {

// ---------------------------------------------------------------------------
// Guest addresses, ORIGINAL debug.xex (xex sha256 2d5e4a32...). Source:
// dc3-decomp config/373307D9/symbols.txt, cross-checked against
// orig/373307D9/ham_xbox_r.map (none of these is an ICF fold).
// ---------------------------------------------------------------------------
constexpr uint32_t kHolmesClientPollKeyboard = 0x825F0F78;  // hook point
constexpr uint32_t kDataReadString = 0x825C12E8;   // DataArray* (const char*)
constexpr uint32_t kDataArrayExecute = 0x825A1528; // DataNode (bool) [r3=ret]
constexpr uint32_t kDataArrayRelease = 0x823317E8; // void ()
constexpr uint32_t kThreadMemStack = 0x827CB658;   // MemHeapStack& (bool)
constexpr uint32_t kMainThread = 0x82330F30;       // bool ()

constexpr uint32_t kTheDebug = 0x82F655D8;  // Debug
constexpr uint32_t kDebugFailingOff = 0x5;  // bool mFailing
constexpr uint32_t kDebugTryOff = 0xC;      // int mTry
constexpr uint32_t kGHolmesStream = 0x82F68C1C;

// Interpreter globals the native port's DtaEval::ScriptStateGuard snapshots
// (native/src/platform/DtaEvalSupport.cpp:380). A guest `throw` that we cut
// short with longjmp skips every destructor between Debug::Fail and our
// frame, so these are restored by hand, exactly as native does after a
// siglongjmp.
struct GlobalWord {
  const char* name;
  uint32_t addr;
};
constexpr GlobalWord kInterpGlobals[] = {
    {"gCallStackPtr", 0x82F10504},   {"gPreExecuteFunc", 0x82F648C0},
    {"gPreExecuteLevel", 0x82F648B8}, {"gDataThis", 0x82F64D6C},
    {"gDataDir", 0x82F64D68},        {"gVarStackPtr", 0x82F1090C},
    {"gFile", 0x82F648CC},
};
constexpr uint32_t kMemHeapStackBytes = 0x48;  // int[16] + mSize + mTempRefs

// First four instruction words of every function we call, from the target
// listing (dc3-decomp build/373307D9/asm). Refuse to install on any mismatch:
// a wrong address here calls into the middle of an unrelated function.
struct Fingerprint {
  uint32_t addr;
  uint32_t words[4];
};
constexpr Fingerprint kFingerprints[] = {
    {kHolmesClientPollKeyboard, {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FFA0}},
    {kDataReadString, {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0}},
    {kDataArrayExecute, {0x7D8802A6, 0x483FC3F1, 0x3BE1FF10, 0x9421FF10}},
    {kDataArrayRelease, {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FF90}},
    {kThreadMemStack, {0x7D8802A6, 0x481D22B9, 0x3BE1FF50, 0x9421FF50}},
    {kMainThread, {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FFA0}},
};

// DataType (dc3-decomp src/system/obj/Data.h)
enum : uint32_t {
  kDataInt = 0,
  kDataFloat = 1,
  kDataVar = 2,
  kDataFunc = 3,
  kDataObject = 4,
  kDataSymbol = 5,
  kDataUnhandled = 6,
  kDataArray = 16,
  kDataCommand = 17,
  kDataString = 18,
  kDataProperty = 19,
  kDataGlob = 20,
};

// Contract limits (tools/console/dc3_eval.py, RB3Enhanced DTAEval.h).
constexpr size_t kMaxScriptBytes = 16384;
constexpr size_t kMaxResultBytes = 32768;
constexpr int kMaxDepth = 8;
constexpr int kMaxElems = 256;
const char kTruncationNotice[] =
    "\n!! output truncated, raise RB3E_DTA_OUTPUT_MAX or split the script\n";

// Scratch block in guest memory (SystemHeapAlloc).
constexpr uint32_t kScratchBytes = 0x5000;
constexpr uint32_t kScratchScriptOff = 0x0;     // kMaxScriptBytes + NUL
constexpr uint32_t kScratchResultOff = 0x4800;  // DataNode (8 bytes)

struct Request {
  std::string body;
  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;
  uint32_t status = 0;
  std::string reply;
};

struct Channel {
  cpu::Processor* processor = nullptr;
  Memory* memory = nullptr;
  std::string socket_path;
  uint32_t scratch = 0;
  std::mutex qmtx;
  std::deque<std::shared_ptr<Request>> queue;
  std::atomic<uint64_t> polls{0};
  std::atomic<uint64_t> evals{0};
  std::atomic<uint32_t> main_thread_id{0};
};
Channel* g_channel = nullptr;

// ---------------------------------------------------------------------------
// Guest C++ throw -> longjmp recovery.
// ---------------------------------------------------------------------------
thread_local std::jmp_buf* t_trap = nullptr;
thread_local uint32_t t_thrown_ptr = 0;

void CppThrowHook(uint32_t thrown_object_ptr) {
  std::jmp_buf* trap = t_trap;
  if (!trap) {
    return;  // nothing armed on this thread: stock behaviour
  }
  t_trap = nullptr;
  t_thrown_ptr = thrown_object_ptr;
  std::longjmp(*trap, 1);
}

// ---------------------------------------------------------------------------
// Guest memory helpers.
// ---------------------------------------------------------------------------
uint32_t Load32(uint32_t addr) {
  return xe::load_and_swap<uint32_t>(
      g_channel->memory->TranslateVirtual<uint8_t*>(addr));
}
int16_t Load16s(uint32_t addr) {
  return static_cast<int16_t>(xe::load_and_swap<uint16_t>(
      g_channel->memory->TranslateVirtual<uint8_t*>(addr)));
}
void Store32(uint32_t addr, uint32_t v) {
  xe::store_and_swap<uint32_t>(
      g_channel->memory->TranslateVirtual<uint8_t*>(addr), v);
}
uint8_t Load8(uint32_t addr) {
  return *g_channel->memory->TranslateVirtual<uint8_t*>(addr);
}
void Store8(uint32_t addr, uint8_t v) {
  *g_channel->memory->TranslateVirtual<uint8_t*>(addr) = v;
}
bool Plausible(uint32_t addr) {
  // Reject null/small ints and anything outside the guest virtual heaps
  // (the channel's own scratch comes from SystemHeapAlloc, ~0x0003xxxx), and
  // anything with no heap behind it.
  if (addr < 0x00010000u || addr >= 0xA0000000u) return false;
  return g_channel->memory->LookupHeap(addr) != nullptr;
}
std::string ReadCString(uint32_t addr, size_t max = 4096) {
  std::string s;
  if (!Plausible(addr)) return "<bad-string>";
  for (size_t i = 0; i < max; ++i) {
    char c = static_cast<char>(Load8(addr + static_cast<uint32_t>(i)));
    if (!c) break;
    s.push_back(c);
  }
  return s;
}

// Snapshot of every piece of guest state a cut-short throw can leave dirty.
struct GuestSnapshot {
  uint32_t globals[sizeof(kInterpGlobals) / sizeof(kInterpGlobals[0])];
  uint8_t debug_failing;
  uint32_t debug_try;
  uint32_t heap_stack_addr;
  uint8_t heap_stack[kMemHeapStackBytes];
};

GuestSnapshot TakeSnapshot(uint32_t heap_stack_addr) {
  GuestSnapshot s{};
  for (size_t i = 0; i < std::size(kInterpGlobals); ++i) {
    s.globals[i] = Load32(kInterpGlobals[i].addr);
  }
  s.debug_failing = Load8(kTheDebug + kDebugFailingOff);
  s.debug_try = Load32(kTheDebug + kDebugTryOff);
  s.heap_stack_addr = heap_stack_addr;
  if (Plausible(heap_stack_addr)) {
    std::memcpy(s.heap_stack,
                g_channel->memory->TranslateVirtual<uint8_t*>(heap_stack_addr),
                kMemHeapStackBytes);
  }
  return s;
}

std::string RestoreSnapshot(const GuestSnapshot& s) {
  std::string repaired;
  for (size_t i = 0; i < std::size(kInterpGlobals); ++i) {
    uint32_t now = Load32(kInterpGlobals[i].addr);
    if (now != s.globals[i]) {
      if (!repaired.empty()) repaired += ", ";
      repaired += kInterpGlobals[i].name;
      Store32(kInterpGlobals[i].addr, s.globals[i]);
    }
  }
  if (Load8(kTheDebug + kDebugFailingOff) != s.debug_failing) {
    if (!repaired.empty()) repaired += ", ";
    repaired += "Debug::mFailing";
    Store8(kTheDebug + kDebugFailingOff, s.debug_failing);
  }
  Store32(kTheDebug + kDebugTryOff, s.debug_try);
  if (Plausible(s.heap_stack_addr)) {
    auto* p =
        g_channel->memory->TranslateVirtual<uint8_t*>(s.heap_stack_addr);
    if (std::memcmp(p, s.heap_stack, kMemHeapStackBytes) != 0) {
      if (!repaired.empty()) repaired += ", ";
      repaired += "MemHeapStack";
      std::memcpy(p, s.heap_stack, kMemHeapStackBytes);
    }
  }
  return repaired;
}

// Call a guest function with Debug::mTry raised, so that a MILO_FAIL inside
// it becomes a guest C++ throw (Debug::Fail: `if (mTry) { mTry--; throw msg; }`)
// that the throw hook turns into a longjmp back here. Returns false and fills
// *error on a trapped failure.
bool GuardedCall(cpu::ThreadState* ts, uint32_t fn, uint64_t* args,
                 size_t nargs, uint64_t* ret, std::string* error,
                 uint32_t heap_stack_addr) {
  auto* ctx = ts->context();
  // Plain values only: this frame is the longjmp target.
  const cpu::ppc::PPCContext saved_ctx = *ctx;
  const GuestSnapshot snap = TakeSnapshot(heap_stack_addr);
  std::jmp_buf trap;
  if (setjmp(trap) != 0) {
    // A guest throw was cut short. The guest stack frames between here and
    // the throw are abandoned (their destructors never ran).
    *ctx = saved_ctx;
    uint32_t thrown = t_thrown_ptr;
    std::string msg = Plausible(thrown) ? ReadCString(Load32(thrown), 1024)
                                        : "<unreadable throw>";
    std::string repaired = RestoreSnapshot(snap);
    // Keep the message single-line: the reply is line-framed.
    for (auto& c : msg) {
      if (c == '\n' || c == '\r') c = ' ';
    }
    while (!msg.empty() && msg.back() == ' ') msg.pop_back();
    *error = msg;
    XELOGW("DC3 DTA channel: guest failure trapped: {}{}{}", msg,
           repaired.empty() ? "" : " [repaired: ",
           repaired.empty() ? "" : repaired + "]");
    return false;
  }
  // Debug::Fail only throws when !mFailing && mTry. Under Xenia mFailing is
  // already STUCK at 1 by boot time (a worker-thread Fail whose spin the
  // fork's Debug::Fail patch turned into a return; measured: mFailThreadMsg
  // 'BinkMovieImpl::Ready called in the wrong thread ...'), which silently
  // turns every later MILO_FAIL into a fall-through. Clear it for the
  // duration of the call so a failing probe is reported, not fallen through,
  // and put the stuck value back afterwards so the game is left as found.
  Store8(kTheDebug + kDebugFailingOff, 0);
  Store32(kTheDebug + kDebugTryOff, snap.debug_try + 1);
  t_trap = &trap;
  *ret = g_channel->processor->Execute(ts, fn, args, nargs);
  t_trap = nullptr;
  Store32(kTheDebug + kDebugTryOff, snap.debug_try);
  Store8(kTheDebug + kDebugFailingOff, snap.debug_failing);
  return true;
}

// ---------------------------------------------------------------------------
// Result printing: a port of RB3Enhanced DTAEval_PrintNode (source/DTAEval.c)
// so the "=> " lines match the contract dc3_eval.py already parses.
// ---------------------------------------------------------------------------
void PrintNode(std::string& out, uint32_t node, int depth);

void PrintArray(std::string& out, uint32_t array, char open, char close,
                int depth) {
  if (!Plausible(array)) {
    out += "<bad-array>";
    return;
  }
  uint32_t nodes = Load32(array + 0x0);
  int count = Load16s(array + 0x8);
  out.push_back(open);
  int shown = count < 0 ? 0 : (count > kMaxElems ? kMaxElems : count);
  for (int i = 0; i < shown; ++i) {
    if (i) out.push_back(' ');
    PrintNode(out, nodes + 8 * i, depth + 1);
  }
  if (count > shown) out += " ...";
  out.push_back(close);
}

void PrintNode(std::string& out, uint32_t node, int depth) {
  if (!Plausible(node)) {
    out += "<bad-node>";
    return;
  }
  if (depth > kMaxDepth) {
    out += "...";
    return;
  }
  uint32_t value = Load32(node + 0);
  uint32_t type = Load32(node + 4);
  char scratch[64];
  switch (type) {
    case kDataInt:
      snprintf(scratch, sizeof(scratch), "%i", static_cast<int32_t>(value));
      out += scratch;
      break;
    case kDataFloat: {
      float f;
      std::memcpy(&f, &value, 4);
      snprintf(scratch, sizeof(scratch), "%f", f);
      out += scratch;
      break;
    }
    case kDataSymbol:
      out += Plausible(value) ? ReadCString(value) : "<bad-symbol>";
      break;
    case kDataString:
      // value is a DataArray whose mNodes word is the char buffer.
      out.push_back('"');
      if (Plausible(value)) out += ReadCString(Load32(value));
      out.push_back('"');
      break;
    case kDataObject:
      snprintf(scratch, sizeof(scratch), "<object 0x%08X>", value);
      out += scratch;
      break;
    case kDataArray:
      PrintArray(out, value, '(', ')', depth);
      break;
    case kDataCommand:
      PrintArray(out, value, '{', '}', depth);
      break;
    case kDataProperty:
      PrintArray(out, value, '[', ']', depth);
      break;
    case kDataUnhandled:
      break;
    case kDataVar:
      snprintf(scratch, sizeof(scratch), "<var 0x%08X>", value);
      out += scratch;
      break;
    case kDataFunc:
      snprintf(scratch, sizeof(scratch), "<func 0x%08X>", value);
      out += scratch;
      break;
    default:
      snprintf(scratch, sizeof(scratch), "<type %i 0x%08X>",
               static_cast<int>(type), value);
      out += scratch;
      break;
  }
}

// ---------------------------------------------------------------------------
// One request, on the guest main thread.
// ---------------------------------------------------------------------------
void Evaluate(cpu::ThreadState* ts, Request& req) {
  auto* mem = g_channel->memory;
  const uint32_t script = g_channel->scratch + kScratchScriptOff;
  const uint32_t result = g_channel->scratch + kScratchResultOff;
  if (req.body.size() >= kMaxScriptBytes) {
    req.status = 413;
    req.reply = "DTA script too large";
    return;
  }
  std::memcpy(mem->TranslateVirtual<uint8_t*>(script), req.body.data(),
              req.body.size());
  Store8(script + static_cast<uint32_t>(req.body.size()), 0);

  uint32_t heap_stack = static_cast<uint32_t>([&] {
    uint64_t a[1] = {1};
    return g_channel->processor->Execute(ts, kThreadMemStack, a, 1);
  }());

  std::string out;
  std::string err;
  uint64_t ret = 0;
  uint64_t parse_args[1] = {script};
  if (!GuardedCall(ts, kDataReadString, parse_args, 1, &ret, &err,
                   heap_stack)) {
    XELOGW("DC3 DTA channel: parse error: {}", err);
    req.status = 200;
    req.reply = "!! parse error\n";
    return;
  }
  const uint32_t arr = static_cast<uint32_t>(ret);
  int count = Plausible(arr) ? Load16s(arr + 0x8) : 0;
  uint32_t nodes = Plausible(arr) ? Load32(arr + 0x0) : 0;
  if (!Plausible(arr) || count < 1 || !Plausible(nodes)) {
    req.status = 200;
    req.reply = "!! parse error\n";
    if (Plausible(arr)) {
      uint64_t a[1] = {arr};
      g_channel->processor->Execute(ts, kDataArrayRelease, a, 1);
    }
    return;
  }

  bool all_commands = true;
  for (int i = 0; i < count; ++i) {
    if (Load32(nodes + 8 * i + 4) != kDataCommand) {
      all_commands = false;
      break;
    }
  }

  auto run_one = [&](uint32_t array_to_exec) -> bool {
    Store32(result + 0, 0);
    Store32(result + 4, kDataUnhandled);
    uint64_t a[3] = {result, array_to_exec, 1};
    uint64_t r = 0;
    if (!out.empty() && out.back() != '\n') out.push_back('\n');
    if (!GuardedCall(ts, kDataArrayExecute, a, 3, &r, &err, heap_stack)) {
      out += "=> !! refused: script error: " + err + "\n";
    } else {
      out += "=> ";
      PrintNode(out, result, 0);
      out += "\n";
      // ~DataNode for array-typed results (kDataArray/Command/String/...).
      uint32_t type = Load32(result + 4);
      uint32_t value = Load32(result + 0);
      if ((type & kDataArray) && Plausible(value)) {
        uint64_t ra[1] = {value};
        g_channel->processor->Execute(ts, kDataArrayRelease, ra, 1);
      }
    }
    g_channel->evals++;
    if (out.size() + sizeof(kTruncationNotice) >= kMaxResultBytes) {
      out.resize(kMaxResultBytes - sizeof(kTruncationNotice) - 1);
      out += kTruncationNotice;
      return false;
    }
    return true;
  };

  if (all_commands) {
    for (int i = 0; i < count; ++i) {
      uint32_t cmd = Load32(nodes + 8 * i + 0);
      if (!run_one(cmd)) break;
    }
  } else {
    run_one(arr);
  }

  uint64_t a[1] = {arr};
  g_channel->processor->Execute(ts, kDataArrayRelease, a, 1);
  req.status = 200;
  req.reply = std::move(out);
}

// Override of HolmesClientPollKeyboard (void()). The stock body polls the
// Holmes host-PC connection for remote keystrokes; with no Holmes stream
// (gHolmesStream == 0, verified and logged below) it does nothing observable.
void PollExtern(cpu::ppc::PPCContext* ctx, kernel::KernelState*) {
  auto* ts = ctx->thread_state;
  uint64_t n = ++g_channel->polls;
  uint32_t tid = kernel::XThread::GetCurrentThreadId();
  if (n == 1) {
    const cpu::ppc::PPCContext saved = *ctx;
    uint32_t is_main = static_cast<uint32_t>(
        g_channel->processor->Execute(ts, kMainThread, nullptr, 0));
    *ctx = saved;
    g_channel->main_thread_id = tid;
    auto* xt = kernel::XThread::GetCurrentThread();
    XELOGI(
        "DC3 DTA channel: first poll on guest thread {:08X} ('{}'); guest "
        "MainThread()={} gHolmesStream={:08X}",
        tid, xt ? xt->name() : "?", is_main & 0xFF, Load32(kGHolmesStream));
  }
  if (n == 1 || (n & (n - 1)) == 0) {
    // Debug state that decides whether a MILO_FAIL can reach our trap at all:
    // Debug::Fail is `if (!mNoDebug && !mFailing) { ... if (mTry) throw; }`.
    uint32_t ftm = Load32(kTheDebug + 0x104);
    XELOGI("DC3 DTA channel: poll #{} TheDebug mNoDebug={} mFailing={} "
           "mTry={} mFailThreadMsg={:08X} '{}'",
           n, Load8(kTheDebug + 0x4), Load8(kTheDebug + kDebugFailingOff),
           Load32(kTheDebug + kDebugTryOff), ftm,
           Plausible(ftm) ? ReadCString(ftm, 200) : "");
  }
  if (tid != g_channel->main_thread_id) {
    static std::atomic<int> warned{0};
    if (warned++ < 5) {
      XELOGW("DC3 DTA channel: poll from UNEXPECTED guest thread {:08X} "
             "(first poll was {:08X}); not draining here",
             tid, g_channel->main_thread_id.load());
    }
    return;
  }
  if ((n & (n - 1)) == 0 && n >= 1024) {
    XELOGI("DC3 DTA channel: {} polls, {} evals", n, g_channel->evals.load());
  }

  std::deque<std::shared_ptr<Request>> batch;
  {
    std::lock_guard<std::mutex> lk(g_channel->qmtx);
    batch.swap(g_channel->queue);
  }
  if (batch.empty()) return;

  const cpu::ppc::PPCContext saved = *ctx;
  for (auto& req : batch) {
    auto t0 = std::chrono::steady_clock::now();
    const uint8_t failing_before = Load8(kTheDebug + kDebugFailingOff);
    const uint32_t ftm_before = Load32(kTheDebug + 0x104);
    Evaluate(ts, *req);
    const uint8_t failing_after = Load8(kTheDebug + kDebugFailingOff);
    const uint32_t ftm_after = Load32(kTheDebug + 0x104);
    if (failing_before != failing_after || ftm_before != ftm_after) {
      XELOGW("DC3 DTA channel: eval CHANGED Debug state: mFailing {}->{} "
             "mFailThreadMsg {:08X}->{:08X}",
             failing_before, failing_after, ftm_before, ftm_after);
    }
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    XELOGI("DC3 DTA channel: evaluated {} bytes -> status {} ({} bytes) in "
           "{} us on guest thread {:08X}",
           req->body.size(), req->status, req->reply.size(), us, tid);
    {
      std::lock_guard<std::mutex> lk(req->mtx);
      req->done = true;
    }
    req->cv.notify_one();
  }
  // Leave the caller's registers exactly as a void call that did nothing.
  *ctx = saved;
}

// ---------------------------------------------------------------------------
// Socket server (host thread).
// ---------------------------------------------------------------------------
bool ReadAll(int fd, void* buf, size_t n) {
  auto* p = static_cast<uint8_t*>(buf);
  while (n) {
    ssize_t r = ::read(fd, p, n);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}
bool WriteAll(int fd, const void* buf, size_t n) {
  auto* p = static_cast<const uint8_t*>(buf);
  while (n) {
    ssize_t r = ::write(fd, p, n);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

void ServeConnection(int fd) {
  for (;;) {
    uint32_t len_le = 0;
    if (!ReadAll(fd, &len_le, 4)) break;
    uint32_t len = xe::byte_swap(xe::byte_swap(len_le));  // host is LE
    auto req = std::make_shared<Request>();
    if (len > 4 * kMaxScriptBytes) {
      req->status = 413;
      req->reply = "DTA script too large";
      // Drain what we can so the stream stays framed.
      std::string sink(len, '\0');
      if (!ReadAll(fd, sink.data(), len)) break;
    } else {
      req->body.resize(len);
      if (len && !ReadAll(fd, req->body.data(), len)) break;
      if (g_channel->polls.load() == 0) {
        req->status = 503;
        req->reply = "guest main thread has not polled yet (still booting)";
      } else {
        {
          std::lock_guard<std::mutex> lk(g_channel->qmtx);
          g_channel->queue.push_back(req);
        }
        std::unique_lock<std::mutex> lk(req->mtx);
        if (!req->cv.wait_for(
                lk,
                std::chrono::milliseconds(cvars::dc3_dta_channel_timeout_ms),
                [&] { return req->done; })) {
          // Leave it queued: the main thread will still evaluate it later
          // (it holds a shared_ptr), we just stop waiting.
          req->status = 504;
          req->reply =
              "guest main thread did not drain the request in time "
              "(blocked in a load, a modal, or not reaching SystemPoll)";
        }
      }
    }
    uint32_t hdr[2] = {req->status, static_cast<uint32_t>(req->reply.size())};
    if (!WriteAll(fd, hdr, sizeof(hdr))) break;
    if (!WriteAll(fd, req->reply.data(), req->reply.size())) break;
  }
  ::close(fd);
}

void ServerThread(int listen_fd) {
  for (;;) {
    int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR) continue;
      XELOGE("DC3 DTA channel: accept failed: {}", strerror(errno));
      return;
    }
    // One connection at a time is the contract (eval is serialised on the
    // main thread anyway).
    ServeConnection(fd);
  }
}

}  // namespace

bool Dc3DtaChannelInstall(cpu::Processor* processor, Memory* memory,
                          const std::string& socket_path) {
  if (g_channel) return true;
  for (const auto& fp : kFingerprints) {
    auto* p = memory->TranslateVirtual<uint8_t*>(fp.addr);
    for (int i = 0; i < 4; ++i) {
      uint32_t w = xe::load_and_swap<uint32_t>(p + 4 * i);
      if (w != fp.words[i]) {
        XELOGE(
            "DC3 DTA channel: NOT installed: fingerprint mismatch at {:08X}+{} "
            "(have {:08X}, want {:08X}); this is not the original debug.xex",
            fp.addr, 4 * i, w, fp.words[i]);
        return false;
      }
    }
  }

  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (fd < 0 || socket_path.size() >= sizeof(addr.sun_path)) {
    XELOGE("DC3 DTA channel: NOT installed: bad socket path '{}'",
           socket_path);
    if (fd >= 0) ::close(fd);
    return false;
  }
  std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  ::unlink(socket_path.c_str());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 4) != 0) {
    XELOGE("DC3 DTA channel: NOT installed: bind/listen '{}' failed: {}",
           socket_path, strerror(errno));
    ::close(fd);
    return false;
  }

  g_channel = new Channel();
  g_channel->processor = processor;
  g_channel->memory = memory;
  g_channel->socket_path = socket_path;
  g_channel->scratch = memory->SystemHeapAlloc(kScratchBytes);
  if (!g_channel->scratch) {
    XELOGE("DC3 DTA channel: NOT installed: SystemHeapAlloc failed");
    ::close(fd);
    delete g_channel;
    g_channel = nullptr;
    return false;
  }
  kernel::xboxkrnl::g_cpp_throw_hook = &CppThrowHook;
  processor->RegisterGuestFunctionOverride(kHolmesClientPollKeyboard,
                                           &PollExtern,
                                           "dc3_dta_channel_poll");
  std::thread(ServerThread, fd).detach();
  XELOGI(
      "DC3 DTA channel: installed on '{}' (hook HolmesClientPollKeyboard "
      "{:08X}, scratch {:08X})",
      socket_path, kHolmesClientPollKeyboard, g_channel->scratch);
  return true;
}

}  // namespace xe
