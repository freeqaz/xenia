/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * milo-trace X-track capture emitter (NOT upstream xenia). See milo_trace.h.
 *
 * Serializes per-function-call records in the FROZEN milo-trace .mtr format
 * (mtr_format.h, vendored verbatim). The Python codec
 * (milo-trace/milo_trace/trace_format/codec.py) parses these exact bytes; the
 * F1 round-trip test is the cross-check.
 *
 * Endianness contract (schema.md §3.1):
 *   - Framing integers (magic/len/type/flags/counts/va, and the register SCALARS
 *     gpr/fpr-u64/cr/xer/ctr/lr/msr) are LITTLE-endian on disk. The Xenia host
 *     is x86-64 (LE), so a plain memcpy of a host integer writes LE bytes — no
 *     swap needed.
 *   - Captured guest MEMORY blobs (STACK/MEM_NODE/WRITES data) are BIG-endian
 *     guest-native, byte-exact, NEVER swapped. Xenia keeps guest memory BE in the
 *     membase, so memcpy(dst, virtual_membase + guest_va, len) yields exactly the
 *     guest's bytes (matches x64_tracers' raw membase model).
 *   - FPRs are the raw 64-bit doubleword bit patterns (NEVER a host double). We
 *     bit-cast ctx->f[i] via memcpy into a uint64_t.
 ******************************************************************************
 */

#include "xenia/cpu/milo_trace.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/error/en.h"

#include "xenia/base/logging.h"
#include "xenia/cpu/mtr_format.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {

namespace {

using xe::cpu::ppc::PPCContext;

// ---------------------------------------------------------------------------
// Tier-B bounded pointer-chase budgets (FROZEN — schema.md §2.3 step 3).
// ---------------------------------------------------------------------------
constexpr int kMaxChaseDepth = 3;
constexpr int kMaxChaseNodes = 64;
constexpr uint32_t kMaxChaseBytes = 8192;
constexpr uint32_t kNodeWindow = 256;

// X6-capture-fix (DEEPER CAPTURE): widened budgets used when a record is in
// EXACT-MEM mode (--milo_trace_exact_mem or a per-addr "exact_mem":true). The
// frozen budgets above were too shallow to close a member-deref read graph
// (SetSpeed/SetPan-class). schema.md §2.3 allows a per-capture-wider chase; the
// node's own base/len record the actual window so the reader never assumes.
//
// NOTE on DEPTH: in exact mode the precise read graph is closed by the Tier-C
// ACCESS_LOG (CaptureObservedReads folds in the EXACT addresses the function
// read), NOT by a deeper blind BFS. A deeper BFS just follows more FALSE pointers
// (random stack words that look mapped) and exhausts the node budget on noise —
// so depth stays at the frozen 3; only the node/byte ceilings widen, to leave
// room for the observed-read windows the access log adds.
constexpr int kMaxChaseDepthExact = 3;
constexpr int kMaxChaseNodesExact = 128;
constexpr uint32_t kMaxChaseBytesExact = 49152;  // 48 KB (room for observed reads)

// Heuristic mapped-range pointer test (GDB-style fallback bounds, schema §2.3
// step 2). Xenia's real test is memory()->LookupHeap(addr); we also use that.
constexpr uint32_t kPtrLo = 0x00010000u;
constexpr uint32_t kPtrHi = 0xFF000000u;

// A captured memory node (Tier A stack or Tier B chased object).
struct CaptureNode {
  uint16_t node_id = 0;
  uint16_t flags = 0;  // mtr_mem_node_hdr_t flags (is_vtable / reached_via_stack)
  uint32_t base = 0;
  uint32_t sp = 0;  // for a stack node only
  std::vector<uint8_t> data;
  bool is_stack = false;
};

// A pending record built between OnEntry and OnExit (one per active call frame).
struct PendingRecord {
  uint32_t func_va = 0;
  uint32_t caller_va = 0;
  uint32_t thread_id = 0;
  uint16_t depth_hint = 0;
  mtr_regs_t regs_in{};
  std::vector<CaptureNode> nodes;        // mem_in (stack + chased)
  // Entry snapshot of every captured interval, for the Method-A write diff.
  // Parallel to `nodes`: pre_snapshot[i] == nodes[i].data at entry.
  std::vector<std::vector<uint8_t>> pre_snapshot;
  // X6: ordered outbound-call target VAs observed while this frame was on top of
  // the shadow stack. Each nested OnEntry (a traced callee) appends its func_va
  // to its parent's list, giving a real DYNAMIC outbound-call list (target_va
  // only — args/ret/writes are an X7-class refinement, see SerializeRecord's
  // CALLS note). With a trace-everything session (empty manifest) this is the
  // complete guest call list; with a manifest gate it covers only traced
  // callees.
  std::vector<uint32_t> outbound_calls;

  // X6-capture-fix (DEEPER CAPTURE): this record is in EXACT-MEM mode (wider
  // Tier-B chase + Tier-C ACCESS_LOG). Set from MiloTraceConfig.exact_mem or a
  // per-addr cfg.exact_mem.
  bool exact_mem = false;

  // X6-capture-fix (ENTRY-PIN): the frame exited via a TAIL-call (the function
  // jumps away mid-frame), so regs_out is the args-to-tail-callee state, not a
  // clean return. The record is tagged with a `TAIL:` NOTE so the replay
  // consumer routes it correctly.
  bool tail_exit = false;

  // X6-capture-fix (DEEPER CAPTURE): the Tier-B chase hit a node/byte budget, so
  // the read graph may be incomplete (=> MTR_MF_TRUNCATED).
  bool truncated_chase = false;

  // X6-capture-fix (DEEPER CAPTURE / Tier C): the actual guest memory accesses
  // the traced call performed, as (va, size, is_write) entries — populated by
  // the per-load/store JIT instrumentation (MiloTraceMemAccess) only when
  // exact_mem is on. Drives ACCESS_LOG (schema §2.3 Tier C / §3.2 type 10): it
  // tells replay EXACTLY which addresses were read, so an under-capture is loud
  // (DECISION D1) instead of silently matching, and a deferred pass captures the
  // read-region windows the heuristic chase missed.
  struct Access {
    uint32_t va;
    uint8_t size;
    uint8_t is_write;
  };
  std::vector<Access> access_log;
};

// Cap the per-record access log so a long-running traced function (a hot loop)
// cannot grow it without bound. 8192 entries ~ 64 KB/record, plenty to close a
// setter/getter read graph; truncation sets MTR_MF_TRUNCATED.
constexpr size_t kMaxAccessLog = 8192;

// Per-thread shadow stack of pending records (nested calls) + an output buffer.
struct ThreadBuffer {
  std::vector<PendingRecord> pending;  // shadow stack
  std::vector<uint8_t> out;            // serialized records awaiting flush
};

struct MiloTraceState {
  std::mutex mutex;
  std::atomic<bool> active{false};
  std::ofstream file;
  bool file_failed = false;

  MiloTraceConfig config;
  uint64_t session_id = 0;
  std::atomic<uint64_t> call_seq{0};

  // Traced-address gate + per-addr config. Empty set => trace everything.
  std::unordered_set<uint32_t> traced;
  std::unordered_map<uint32_t, MiloTraceAddrConfig> addr_cfg;
  MiloTraceAddrConfig default_cfg;

  // Registry of every live per-thread buffer, so MiloTraceEnd can drain ALL
  // threads (not just the caller's). The guest runs on a different OS thread
  // than the headless/UI thread that calls End — without this, that thread's
  // sub-256 KB buffered records (i.e. an entire short boot run) are lost. Guarded
  // by its own mutex (registration is off the per-call hot path; only at
  // first-touch per thread). ThreadBuffers are intentionally leaked at thread
  // exit (X6-era refinement to deregister + flush on thread teardown).
  std::mutex registry_mutex;
  std::unordered_set<ThreadBuffer*> live_buffers;
};

MiloTraceState& State() {
  static MiloTraceState s;
  return s;
}

// Per-thread storage (one ThreadBuffer per OS thread; no contention on the hot
// path except at flush boundaries).
thread_local ThreadBuffer* tls_buffer = nullptr;

ThreadBuffer* GetThreadBuffer() {
  if (!tls_buffer) {
    tls_buffer = new ThreadBuffer();
    // Register so MiloTraceEnd can drain this thread's buffer even if End runs
    // on a different thread. First-touch only — off the per-call hot path.
    MiloTraceState& st = State();
    std::lock_guard<std::mutex> reg(st.registry_mutex);
    st.live_buffers.insert(tls_buffer);
  }
  return tls_buffer;
}

// ---- LE framing append helpers (host x86-64 is LE; no swap). ----
void PutU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void PutU16(std::vector<uint8_t>& b, uint16_t v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + 2);
}
void PutU32(std::vector<uint8_t>& b, uint32_t v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + 4);
}
void PutU64(std::vector<uint8_t>& b, uint64_t v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + 8);
}
void PutBytes(std::vector<uint8_t>& b, const void* p, size_t n) {
  const uint8_t* c = reinterpret_cast<const uint8_t*>(p);
  b.insert(b.end(), c, c + n);
}

// ---- guest memory accessors (BE blobs are copied raw from the membase). ----

uint8_t* GuestPtr(PPCContext* ctx, uint32_t guest_va) {
  return ctx->virtual_membase + guest_va;
}

// Is `va` a plausible, mapped guest pointer? Prefer the real heap map; fall back
// to the heuristic range when no processor/memory is reachable.
//
// X6-capture-fix (DEEPER CAPTURE — precision): require 4-byte alignment. Every
// real Xenon guest object / vtable / code pointer is at least word-aligned; the
// chase's FALSE positives were overwhelmingly UNALIGNED ASCII stack words
// ("DOS ", "METI", 0x41879026) that happened to land in a huge mapped region and
// got chased as objects — flooding the node budget with garbage that masked the
// real read graph. Rejecting unaligned values keeps every legitimate pointer and
// drops the noise. (A misaligned datum is never a Milo object base.)
bool IsGuestPointer(PPCContext* ctx, uint32_t va) {
  if (va < kPtrLo || va >= kPtrHi) return false;
  if (va & 0x3u) return false;  // real guest pointers are word-aligned
  if (ctx->processor) {
    auto* memory = ctx->processor->memory();
    if (memory) {
      return memory->LookupHeap(va) != nullptr;
    }
  }
  return true;  // heuristic fallback
}

// Static instruction count of the function at `func_va`, looked up from the
// processor's symbol table: (end_address - start_address)/4 + 1. This is the
// function's SIZE in instructions, used as a coarse proxy for the schema's
// insn_count ("instructions executed entry->exit"). A true dynamic count needs
// per-instruction instrumentation (X7-class); the static count is a meaningful,
// non-zero stand-in for non-leaf records and is exact for straight-line leaves.
// Returns 0 if the function is not resolvable (then the record's insn_count
// stays 0, as before).
uint32_t StaticInsnCount(PPCContext* ctx, uint32_t func_va) {
  if (!ctx->processor) return 0;
  Function* fn = ctx->processor->QueryFunction(func_va);
  if (!fn) return 0;
  uint32_t start = fn->address();
  uint32_t end = fn->end_address();
  if (end <= start) return 0;
  return (end - start) / 4 + 1;
}

// How many of the `len` requested bytes starting at `va` are actually safe to
// read — i.e. lie within the contiguous run of COMMITTED + readable guest pages
// beginning at `va`. The X1 code read the whole requested window in one memcpy
// after a single start-of-window IsGuestPointer() check; but a window straddling
// the end of a heap region (classic for a Tier-A stack window `[sp-256, sp+512)`
// whose low end falls below the thread's stack reservation, or a Tier-B node
// near the top of an allocation) faults — the guest reservation is contiguous in
// host VA but not all of it is committed/readable. Clamp to the mapped run so
// memcpy never crosses into a guard/uncommitted page (the SIGSEGV fix that lets
// real capture emit records). Returns 0 if `va` itself is not readable.
uint32_t MappedReadableLen(PPCContext* ctx, uint32_t va, uint32_t len) {
  if (len == 0) return 0;
  if (va < kPtrLo || va > kPtrHi - len) {
    // Fall back to a conservative per-region clamp below for in-range, but a
    // wrap or absurd va is never readable.
    if (va < kPtrLo) return 0;
  }
  if (!ctx->processor) {
    // No heap map reachable (test/standalone). Trust the heuristic range but do
    // not read past kPtrHi.
    if (va >= kPtrHi) return 0;
    uint32_t cap = kPtrHi - va;
    return len < cap ? len : cap;
  }
  auto* memory = ctx->processor->memory();
  if (!memory) {
    if (va >= kPtrHi) return 0;
    uint32_t cap = kPtrHi - va;
    return len < cap ? len : cap;
  }

  uint32_t got = 0;
  uint32_t cur = va;
  // Walk contiguous identical-attribute regions, accumulating committed+readable
  // bytes until we cover `len`, hit a non-readable region, or run out of heap.
  while (got < len) {
    BaseHeap* heap = memory->LookupHeap(cur);
    if (!heap) break;
    HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(cur, &info)) break;
    const bool committed = (info.state & kMemoryAllocationCommit) != 0;
    const bool readable = (info.protect & kMemoryProtectRead) != 0;
    if (!committed || !readable) break;
    // CAREFUL: QueryRegionInfo sets info.base_address = the (possibly UNALIGNED)
    // address we passed, but info.region_size is measured in whole pages from the
    // page that CONTAINS that address. So the mapped run covers
    // [page_floor(cur), page_floor(cur) + region_size); the readable bytes from
    // `cur` itself are region_size minus the intra-page offset. Using
    // base_address + region_size directly over-reports by that offset and walks
    // straight off the end of a heap region into a guard page — the SIGSEGV.
    const uint32_t page = heap->page_size();
    const uint32_t page_floor = cur - (cur % page);
    const uint32_t region_end = page_floor + info.region_size;
    if (cur >= region_end) break;  // defensive (shouldn't happen)
    uint32_t run = region_end - cur;
    got += run;
    cur = region_end;
  }
  return got < len ? got : len;
}

// Copy up to `len` BE guest bytes at `va` into `out`, clamped to the mapped run.
// Returns false only if NOTHING at `va` is readable (so the caller skips the
// node); a partial read is a success with a shorter `out` (the record records
// the actual captured length, so a clamped window is still well-formed).
bool ReadGuest(PPCContext* ctx, uint32_t va, uint32_t len,
               std::vector<uint8_t>& out) {
  uint32_t safe = MappedReadableLen(ctx, va, len);
  if (safe == 0) return false;
  out.resize(safe);
  std::memcpy(out.data(), GuestPtr(ctx, va), safe);  // raw BE guest-native bytes
  return true;
}

// Read a 32-bit big-endian guest word at `va` (for pointer-chase seeds).
// Returns 0 if the 4 bytes are not all readable (so it never faults).
uint32_t ReadGuestBE32(PPCContext* ctx, uint32_t va) {
  if (MappedReadableLen(ctx, va, 4) < 4) return 0;
  const uint8_t* p = GuestPtr(ctx, va);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// ---- register snapshot ----

mtr_regs_t SnapshotRegs(PPCContext* ctx) {
  mtr_regs_t r{};
  for (int i = 0; i < 32; ++i) {
    r.gpr[i] = static_cast<uint32_t>(ctx->r[i]);
  }
  for (int i = 0; i < 32; ++i) {
    // FPR raw u64 bit pattern (never a host double).
    uint64_t bits;
    std::memcpy(&bits, &ctx->f[i], sizeof(bits));
    r.fpr[i] = bits;
  }
  r.cr = static_cast<uint32_t>(ctx->cr());
  // XER: reconstruct from the split bytes. PPC bit numbering (MSB=0):
  // SO=bit32(0x80000000), OV=bit33(0x40000000), CA=bit34(0x20000000).
  r.xer = (uint32_t(ctx->xer_so ? 1u : 0u) << 31) |
          (uint32_t(ctx->xer_ov ? 1u : 0u) << 30) |
          (uint32_t(ctx->xer_ca ? 1u : 0u) << 29);
  r.ctr = static_cast<uint32_t>(ctx->ctr);
  r.lr = static_cast<uint32_t>(ctx->lr);
  // Xenia has no first-class guest MSR; the FP-enabled bit is what replay needs.
  // Capture the fpscr value as a proxy (replay overrides msr |= MSR_FP anyway).
  r.msr = ctx->fpscr.value;
  return r;
}

// ---- Tier A + Tier B capture ----

// Capture the stack window [r1-back, r1+fwd) as a STACK-flagged node.
void CaptureStackWindow(PPCContext* ctx, const MiloTraceAddrConfig& cfg,
                        PendingRecord& rec) {
  uint32_t sp = static_cast<uint32_t>(ctx->r[1]);
  if (sp < cfg.mem_window_back) return;  // nonsensical SP; skip
  uint32_t lo = sp - cfg.mem_window_back;
  uint32_t len = cfg.mem_window_back + cfg.mem_window_fwd;
  CaptureNode node;
  node.is_stack = true;
  node.flags = 1u << 2;  // NODE_REACHED_VIA_STACK
  node.base = lo;
  node.sp = sp;
  if (!ReadGuest(ctx, lo, len, node.data)) {
    // Clamp: try just the forward half if the back half is unmapped.
    if (!ReadGuest(ctx, sp, cfg.mem_window_fwd, node.data)) return;
    node.base = sp;
  }
  node.node_id = static_cast<uint16_t>(rec.nodes.size());
  rec.pre_snapshot.push_back(node.data);
  rec.nodes.push_back(std::move(node));
}

// Add `va` (window [va, va+want)) as a chased node if it is a live pointer and
// not already covered. Returns true iff a node was added. Sets the vtable hint.
// Shared by the entry chase and the X6-capture-fix exit-time read-region capture.
bool MaybeAddChaseNode(PPCContext* ctx, PendingRecord& rec, uint32_t va,
                       uint32_t want, uint32_t& total_bytes,
                       uint32_t max_bytes) {
  if (total_bytes >= max_bytes) return false;
  for (const auto& n : rec.nodes) {
    if (va >= n.base && va < n.base + static_cast<uint32_t>(n.data.size())) {
      return false;  // coarse interval-merge: already covered
    }
  }
  if (total_bytes + want > max_bytes) want = max_bytes - total_bytes;
  if (want < 4) return false;
  CaptureNode node;
  node.base = va;
  if (!ReadGuest(ctx, va, want, node.data)) return false;
  node.node_id = static_cast<uint16_t>(rec.nodes.size());
  uint32_t first_word = ReadGuestBE32(ctx, va);
  if (IsGuestPointer(ctx, first_word)) {
    node.flags |= 1u << 0;  // NODE_IS_VTABLE hint (coarse)
  }
  total_bytes += static_cast<uint32_t>(node.data.size());
  rec.pre_snapshot.push_back(node.data);
  rec.nodes.push_back(std::move(node));
  return true;
}

// Bounded BFS pointer chase from arg regs r3..r10 (schema §2.3 Tier B). Captures
// up to kMaxChaseNodes / kMaxChaseBytes; merges overlapping intervals coarsely
// (skips a seed already inside a captured node). Vtable special-case: if a node's
// first word points into a mapped region we still capture the node; full vtable
// slot capture + EDGES are an F2-side refinement (left to the pointer-chase lib).
//
// X6-capture-fix (DEEPER CAPTURE): when rec.exact_mem is set, the depth/node/byte
// budgets widen and the seed set also includes ctr/lr (the indirect-branch
// target + return continuation), so a `bctr`-dispatching dispatcher's target
// object/vtable page is captured. Sets `truncated` (=> MTR_MF_TRUNCATED) if a
// budget is hit mid-chase, so an under-capture is visible, not silent.
void CaptureArgChase(PPCContext* ctx, const MiloTraceAddrConfig& cfg,
                     PendingRecord& rec, bool& truncated) {
  const bool exact = rec.exact_mem;
  const int max_depth = exact ? kMaxChaseDepthExact : kMaxChaseDepth;
  const int max_nodes = exact ? kMaxChaseNodesExact : kMaxChaseNodes;
  const uint32_t max_bytes = exact ? kMaxChaseBytesExact : kMaxChaseBytes;
  (void)cfg;

  uint32_t total_bytes = 0;
  for (const auto& n : rec.nodes) {
    total_bytes += static_cast<uint32_t>(n.data.size());
  }

  struct Hop {
    uint32_t va;
    int depth;
  };
  std::vector<Hop> queue;
  for (int reg = 3; reg <= 10; ++reg) {
    uint32_t v = static_cast<uint32_t>(ctx->r[reg]);
    if (IsGuestPointer(ctx, v)) queue.push_back({v, 0});
  }
  if (exact) {
    // Seed the indirect-branch target (ctr) and the return continuation (lr) so a
    // dispatcher's resolved-target page + its caller frame's object are captured.
    uint32_t ctr = static_cast<uint32_t>(ctx->ctr);
    uint32_t lr = static_cast<uint32_t>(ctx->lr);
    if (IsGuestPointer(ctx, ctr)) queue.push_back({ctr, 0});
    if (IsGuestPointer(ctx, lr)) queue.push_back({lr, 0});
  }

  size_t qi = 0;
  while (qi < queue.size()) {
    Hop hop = queue[qi++];
    if (static_cast<int>(rec.nodes.size()) >= max_nodes) {
      // In EXACT mode the blind chase is supplementary — the Tier-C ACCESS_LOG is
      // the authoritative read graph, so hitting the (deliberately deep) node
      // ceiling is expected and benign; do NOT mark the record incomplete (that
      // would make the trust gate reject a record whose precise reads ARE
      // captured). Only the access-log cap (MiloTraceMemAccess) flags exact
      // truncation. For the non-exact heuristic-only chase, a budget hit IS a
      // real under-capture risk and is flagged.
      if (!exact) truncated = true;
      break;
    }
    if (total_bytes >= max_bytes) {
      if (!exact) truncated = true;
      break;
    }

    if (!MaybeAddChaseNode(ctx, rec, hop.va, kNodeWindow, total_bytes,
                           max_bytes)) {
      continue;
    }

    // Enqueue child pointers for the next hop.
    if (hop.depth + 1 <= max_depth) {
      const CaptureNode& just = rec.nodes.back();
      for (uint32_t off = 0; off + 4 <= just.data.size(); off += 4) {
        uint32_t w = (uint32_t(just.data[off]) << 24) |
                     (uint32_t(just.data[off + 1]) << 16) |
                     (uint32_t(just.data[off + 2]) << 8) |
                     uint32_t(just.data[off + 3]);
        if (IsGuestPointer(ctx, w)) queue.push_back({w, hop.depth + 1});
      }
    }
  }
}

// X6-capture-fix (DEEPER CAPTURE / Tier C): after the traced call returns, fold
// the observed read addresses (rec.access_log, gathered by the per-load JIT
// instrumentation) into the captured node set — capturing the [page, page+256)
// window of any READ address the heuristic chase missed. This is what closes the
// member-deref read graph (SetSpeed/SetPan-class): the setter reads through `this`
// to an object the arg chase never reached, and that read is now captured at its
// real VA so a1 replay can reproduce it (DECISION D1: an uncaptured read faults).
// Only runs for exact_mem records. Bounded by the exact byte budget.
void CaptureObservedReads(PPCContext* ctx, PendingRecord& rec) {
  if (!rec.exact_mem || rec.access_log.empty()) return;
  uint32_t total_bytes = 0;
  for (const auto& n : rec.nodes) {
    total_bytes += static_cast<uint32_t>(n.data.size());
  }
  for (const auto& a : rec.access_log) {
    if (a.is_write) continue;  // writes land in mem_out; capture READ inputs
    if (total_bytes >= kMaxChaseBytesExact) break;
    if (!IsGuestPointer(ctx, a.va)) continue;
    // Capture an aligned 256 B window covering the access so a small struct/array
    // touched at an offset is grabbed whole.
    uint32_t win_base = a.va & ~0x3Fu;  // 64 B align down (covers the access)
    MaybeAddChaseNode(ctx, rec, win_base, kNodeWindow, total_bytes,
                      kMaxChaseBytesExact);
  }
}

// ---- serialization (matches mtr_format.h / codec.py byte-for-byte) ----

void AppendSectionPrefix(std::vector<uint8_t>& b, uint16_t type, uint16_t flags,
                         uint32_t length) {
  PutU16(b, type);
  PutU16(b, flags);
  PutU32(b, length);
}

void AppendRegsSection(std::vector<uint8_t>& b, uint16_t type,
                       const mtr_regs_t& r) {
  // mtr_regs_t == 404 bytes, all framing-LE; host LE memcpy is exact.
  AppendSectionPrefix(b, type, 0, sizeof(mtr_regs_t));
  PutBytes(b, &r, sizeof(mtr_regs_t));
}

void AppendStackSection(std::vector<uint8_t>& b, const CaptureNode& n) {
  // STACK payload: {sp,lo,hi, data[hi-lo]} (mtr_stack_hdr_t + BE blob).
  uint32_t lo = n.base;
  uint32_t hi = n.base + static_cast<uint32_t>(n.data.size());
  uint32_t payload_len =
      static_cast<uint32_t>(sizeof(mtr_stack_hdr_t) + n.data.size());
  AppendSectionPrefix(b, MTR_SEC_STACK, 0, payload_len);
  PutU32(b, n.sp);
  PutU32(b, lo);
  PutU32(b, hi);
  PutBytes(b, n.data.data(), n.data.size());
}

void AppendMemNodeSection(std::vector<uint8_t>& b, const CaptureNode& n) {
  // MEM_NODE payload: {node_id,flags,base,len, data[len]}. The 16-bit node flags
  // are mirrored into BOTH the section prefix flags and the node header flags
  // (codec.py: _pack_mem_node writes node.flags into the section prefix).
  uint32_t payload_len =
      static_cast<uint32_t>(sizeof(mtr_mem_node_hdr_t) + n.data.size());
  AppendSectionPrefix(b, MTR_SEC_MEM_NODE, n.flags, payload_len);
  PutU16(b, n.node_id);
  PutU16(b, n.flags);
  PutU32(b, n.base);
  PutU32(b, static_cast<uint32_t>(n.data.size()));
  PutBytes(b, n.data.data(), n.data.size());
}

// Method-A write delta: diff each captured interval pre vs post. Emits one
// WRITES section with the changed spans (kind=0 snapshot). schema §2.5.
void AppendWritesSection(std::vector<uint8_t>& b, PPCContext* ctx,
                         const PendingRecord& rec, bool& any_writes) {
  // First collect the changed spans so we can emit the count prefix.
  struct Span {
    uint32_t addr;
    std::vector<uint8_t> bytes;
  };
  std::vector<Span> spans;
  for (size_t i = 0; i < rec.nodes.size(); ++i) {
    const CaptureNode& node = rec.nodes[i];
    const std::vector<uint8_t>& pre = rec.pre_snapshot[i];
    uint32_t n = static_cast<uint32_t>(pre.size());
    // Re-read the post image at the same VA.
    std::vector<uint8_t> post;
    if (!ReadGuest(ctx, node.base, n, post)) continue;
    // ReadGuest clamps to the mapped run, so the post image may be shorter than
    // the entry snapshot (e.g. the stack contracted). Only diff the overlap.
    if (post.size() < n) n = static_cast<uint32_t>(post.size());
    // Walk for contiguous changed runs.
    uint32_t j = 0;
    while (j < n) {
      if (post[j] != pre[j]) {
        uint32_t start = j;
        while (j < n && post[j] != pre[j]) ++j;
        Span s;
        s.addr = node.base + start;
        s.bytes.assign(post.begin() + start, post.begin() + j);
        spans.push_back(std::move(s));
      } else {
        ++j;
      }
    }
  }
  if (spans.empty()) {
    any_writes = false;
    return;
  }
  any_writes = true;

  std::vector<uint8_t> payload;
  PutU32(payload, static_cast<uint32_t>(spans.size()));
  for (const auto& s : spans) {
    // mtr_write_entry_t: addr u32, len u16, kind u8, _pad u8, then data[len].
    PutU32(payload, s.addr);
    PutU16(payload, static_cast<uint16_t>(s.bytes.size()));
    PutU8(payload, 0);  // kind = snapshot
    PutU8(payload, 0);  // _pad
    PutBytes(payload, s.bytes.data(), s.bytes.size());
  }
  // writes_complete == scoped (Method A only sees captured intervals).
  AppendSectionPrefix(b, MTR_SEC_WRITES, MTR_WRITES_SCOPED,
                      static_cast<uint32_t>(payload.size()));
  PutBytes(b, payload.data(), payload.size());
}

// CALLS section (the outbound-call oracle, schema §2.6 / §3.5). X6 populates a
// real DYNAMIC outbound-call list — one CallEdge per traced callee observed
// while this frame was active — but only its target_va is known from the
// prologue/epilog shadow stack. The per-call args/ret/mem_writes (the full
// replay oracle mode (b) needs) require per-bl/bctrl instrumentation (X7), so
// each CallEdge here is the 12-byte header only (arg_mask=0, ret_mask=0,
// nwrites=0). This is byte-identical to a codec CallEdge with empty
// args/ret/mem_writes (see codec.py _pack_one_call), so it round-trips, and the
// target_va list is directly usable for discovery's dyn_call_edges. Returns
// true iff a section was emitted.
bool AppendCallsSection(std::vector<uint8_t>& b, const PendingRecord& rec) {
  if (rec.outbound_calls.empty()) return false;
  std::vector<uint8_t> payload;
  PutU32(payload, static_cast<uint32_t>(rec.outbound_calls.size()));
  for (uint32_t target_va : rec.outbound_calls) {
    // mtr_call_hdr_t: src_offset u32, target_va u32, arg_mask u16, ret_mask u8,
    // nwrites u8 (== "<IIHBB", 12 bytes). No trailing args/ret/writes.
    PutU32(payload, 0);          // src_offset (unknown without bl-site capture)
    PutU32(payload, target_va);  // RESOLVED callee VA (the traced child's entry)
    PutU16(payload, 0);          // arg_mask (no args captured)
    PutU8(payload, 0);           // ret_mask (no ret captured)
    PutU8(payload, 0);           // nwrites
  }
  AppendSectionPrefix(b, MTR_SEC_CALLS, 0,
                      static_cast<uint32_t>(payload.size()));
  PutBytes(b, payload.data(), payload.size());
  return true;
}

// X6-capture-fix (DEEPER CAPTURE / Tier C): ACCESS_LOG section (schema §3.2 type
// 10). One mtr_access_entry_t {addr u32, size u8, is_write u8, _pad u16} per
// observed access, in execution order. Records what the call ACTUALLY touched, so
// replay can prove its captured read graph is complete (DECISION D1). Returns
// true iff a section was emitted.
bool AppendAccessLogSection(std::vector<uint8_t>& b, const PendingRecord& rec) {
  if (rec.access_log.empty()) return false;
  std::vector<uint8_t> payload;
  PutU32(payload, static_cast<uint32_t>(rec.access_log.size()));
  for (const auto& a : rec.access_log) {
    // mtr_access_entry_t == "<IBBH" (8 bytes): addr, size, is_write, _pad.
    PutU32(payload, a.va);
    PutU8(payload, a.size);
    PutU8(payload, a.is_write);
    PutU16(payload, 0);  // _pad
  }
  AppendSectionPrefix(b, MTR_SEC_ACCESS_LOG, 0,
                      static_cast<uint32_t>(payload.size()));
  PutBytes(b, payload.data(), payload.size());
  return true;
}

// X6-capture-fix (ENTRY-PIN): a NOTE section (schema §3.2 type 11) carrying a
// short UTF-8 diagnostic tag. Used to mark a TAIL-exit record (`TAIL:<hex
// func_va>`) so the replay consumer knows regs_out is an args-to-tail-callee
// handoff, not a clean return. NOTE is non-authoritative + skip-safe, so this is
// additive and never disturbs an old reader. codec.py NOTE payload = u32 len +
// utf8 bytes.
void AppendNoteSection(std::vector<uint8_t>& b, const std::string& text) {
  std::vector<uint8_t> payload;
  PutU32(payload, static_cast<uint32_t>(text.size()));
  PutBytes(payload, text.data(), text.size());
  AppendSectionPrefix(b, MTR_SEC_NOTE, 0,
                      static_cast<uint32_t>(payload.size()));
  PutBytes(b, payload.data(), payload.size());
}

// Serialize a completed record into the thread's out buffer, byte-for-byte to
// the mtr_format.h layout. Returns the number of sections written.
void SerializeRecord(std::vector<uint8_t>& out, PPCContext* ctx,
                     const PendingRecord& rec, const mtr_regs_t& regs_out,
                     uint32_t insn_count, uint64_t call_seq) {
  MiloTraceState& st = State();

  std::vector<uint8_t> sections;
  uint16_t section_count = 0;
  uint8_t mem_flags = 0;

  // REGS_IN / REGS_OUT (always present).
  AppendRegsSection(sections, MTR_SEC_REGS_IN, rec.regs_in);
  ++section_count;
  AppendRegsSection(sections, MTR_SEC_REGS_OUT, regs_out);
  ++section_count;

  // mem_in: stack node -> STACK section; chased nodes -> MEM_NODE.
  for (const auto& node : rec.nodes) {
    if (node.is_stack) {
      AppendStackSection(sections, node);
      mem_flags |= MTR_MF_TIER_A;
    } else {
      AppendMemNodeSection(sections, node);
      mem_flags |= MTR_MF_TIER_B;
    }
    ++section_count;
  }

  // WRITES (mem_out delta, Method A).
  bool any_writes = false;
  AppendWritesSection(sections, ctx, rec, any_writes);
  if (any_writes) ++section_count;
  // Method A => scoped, so MTR_MF_WRITES_COMPLETE stays clear.

  // CALLS (X6 outbound-call oracle — target_va list, dynamic). Populates the
  // calls[] axis for non-leaf records, which the override-wrap path left empty.
  if (AppendCallsSection(sections, rec)) ++section_count;

  // ACCESS_LOG (X6-capture-fix Tier C — observed (va,size,is_write) reads/writes
  // during the call). Present only for exact_mem records.
  if (AppendAccessLogSection(sections, rec)) {
    ++section_count;
    mem_flags |= MTR_MF_TIER_C;
  }

  // X6-capture-fix: surface a too-shallow chase + a tail handoff so the consumer
  // (and the trust gate) can tell capture-completeness limits from clean records.
  if (rec.truncated_chase) mem_flags |= MTR_MF_TRUNCATED;
  if (rec.tail_exit) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "TAIL:%08x", rec.func_va);
    AppendNoteSection(sections, std::string(buf));
    ++section_count;
  }

  // Record header (mtr_header_t, 42 bytes, framing-LE).
  std::vector<uint8_t> body;
  uint32_t record_len = static_cast<uint32_t>(
      sizeof(mtr_header_t) + sections.size() + sizeof(mtr_footer_t));
  PutU32(body, MTR_MAGIC);
  PutU32(body, record_len);
  PutU64(body, call_seq);
  PutU32(body, rec.func_va);
  PutU32(body, rec.caller_va);
  PutU32(body, rec.thread_id);
  PutU16(body, rec.depth_hint);
  PutU8(body, static_cast<uint8_t>(st.config.arch));
  PutU8(body, mem_flags);
  PutU8(body, static_cast<uint8_t>(st.config.capture_method));
  PutU8(body, 0);  // _pad0
  PutU32(body, insn_count);
  PutU16(body, section_count);
  PutU16(body, 0);  // reserved

  PutBytes(body, sections.data(), sections.size());

  // Footer: crc32 over [magic..last section], then magic2.
  // Compute CRC-32 (zlib/IEEE) over body, matching Python zlib.crc32.
  static const auto crc32_ieee = [](const uint8_t* data, size_t n) -> uint32_t {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
      c ^= data[i];
      for (int k = 0; k < 8; ++k) {
        c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
      }
    }
    return c ^ 0xFFFFFFFFu;
  };
  uint32_t crc_val = crc32_ieee(body.data(), body.size());

  out.insert(out.end(), body.begin(), body.end());
  PutU32(out, crc_val);
  PutU32(out, MTR_MAGIC2);
}

// File header (mtr_file_header_t, 48 bytes). schema §3.3.
void WriteFileHeader(std::ofstream& file, const MiloTraceState& st) {
  std::vector<uint8_t> b;
  PutU32(b, MTR_FILE_MAGIC);
  PutU16(b, MTR_SCHEMA_VERSION);
  PutU8(b, static_cast<uint8_t>(st.config.arch));
  PutU8(b, MTR_FRAMING_LE);
  PutU64(b, st.session_id);
  PutU64(b, 0);  // created_unix (provenance only)
  uint8_t sha[20] = {0};
  // Parse a hex sha1 if provided (40 hex chars -> 20 bytes); else zeros.
  if (st.config.target_sha1.size() >= 40) {
    auto hexval = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    bool ok = true;
    for (int i = 0; i < 20 && ok; ++i) {
      int hi = hexval(st.config.target_sha1[i * 2]);
      int lo = hexval(st.config.target_sha1[i * 2 + 1]);
      if (hi < 0 || lo < 0) {
        ok = false;
        break;
      }
      sha[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (!ok) std::memset(sha, 0, sizeof(sha));
  }
  b.insert(b.end(), sha, sha + 20);
  PutU8(b, static_cast<uint8_t>(st.config.capture_method));
  PutU8(b, 0);  // pool_present
  PutU16(b, 0);  // reserved
  file.write(reinterpret_cast<const char*>(b.data()),
             static_cast<std::streamsize>(b.size()));
}

// Flush one thread's serialized records to the sink under the global lock.
void FlushThreadBufferLocked(MiloTraceState& st, ThreadBuffer* tb) {
  if (tb->out.empty() || st.file_failed || !st.file.is_open()) {
    tb->out.clear();
    return;
  }
  st.file.write(reinterpret_cast<const char*>(tb->out.data()),
                static_cast<std::streamsize>(tb->out.size()));
  if (!st.file) st.file_failed = true;
  tb->out.clear();
}

// Flush a thread buffer when it grows past a threshold (amortize lock cost).
constexpr size_t kFlushThreshold = 256 * 1024;

}  // namespace

// ===========================================================================
// Public API.
// ===========================================================================

void MiloTraceBegin(const MiloTraceConfig& config) {
  MiloTraceState& st = State();
  std::lock_guard<std::mutex> lock(st.mutex);

  if (config.out_path.empty()) {
    st.active.store(false, std::memory_order_release);
    return;
  }

  st.config = config;
  st.file.open(config.out_path, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!st.file.is_open()) {
    XELOGW("milo-trace: failed to open sink '{}'", config.out_path);
    st.active.store(false, std::memory_order_release);
    return;
  }
  st.file_failed = false;
  static std::atomic<uint64_t> s_session{0};
  st.session_id = s_session.fetch_add(1, std::memory_order_relaxed) + 1;
  st.call_seq.store(0, std::memory_order_relaxed);

  WriteFileHeader(st.file, st);

  // Load the address manifest. The parser takes no lock (it only mutates
  // st.traced / st.addr_cfg, which Begin owns here), so calling it while holding
  // st.mutex does not deadlock. An empty/absent manifest => trace everything
  // (the override-wrap proto gates via the override list instead).
  if (!config.manifest_path.empty()) {
    MiloTraceLoadManifest(config.manifest_path);
  }

  st.active.store(true, std::memory_order_release);
  XELOGI("milo-trace: capture started -> '{}' ({} traced addrs)",
         config.out_path, st.traced.size());
}

void MiloTraceEnd(const char* reason) {
  MiloTraceState& st = State();
  std::lock_guard<std::mutex> lock(st.mutex);
  if (!st.active.load(std::memory_order_acquire)) return;

  // Drain EVERY live per-thread buffer (the guest thread that produced the
  // records is almost never the thread calling End). Records are flushed at a
  // 256 KB threshold during the run, so a short capture (an entire boot run)
  // typically sits entirely unflushed in the guest thread's buffer until here.
  // Buffers belonging to threads that exited before End are still drained
  // (ThreadBuffers are leaked, not freed, at thread teardown).
  {
    std::lock_guard<std::mutex> reg(st.registry_mutex);
    for (ThreadBuffer* tb : st.live_buffers) {
      FlushThreadBufferLocked(st, tb);
    }
  }

  if (st.file.is_open()) {
    st.file.flush();
    st.file.close();
  }
  st.active.store(false, std::memory_order_release);
  XELOGI("milo-trace: capture ended ({})", reason ? reason : "unknown");
}

bool MiloTraceIsActive() {
  return State().active.load(std::memory_order_acquire);
}

bool MiloTraceShouldTrace(uint32_t start_addr) {
  MiloTraceState& st = State();
  if (!st.active.load(std::memory_order_acquire)) return false;
  if (st.traced.empty()) return true;  // empty set => trace everything
  return st.traced.count(start_addr) != 0;
}

MiloTraceAddrConfig MiloTraceConfigFor(uint32_t start_addr) {
  MiloTraceState& st = State();
  auto it = st.addr_cfg.find(start_addr);
  if (it != st.addr_cfg.end()) return it->second;
  return st.default_cfg;
}

void MiloTraceOnEntryCtx(void* ppc_context, uint32_t start_addr) {
  if (!MiloTraceShouldTrace(start_addr)) return;
  PPCContext* ctx = reinterpret_cast<PPCContext*>(ppc_context);
  MiloTraceState& st = State();
  MiloTraceAddrConfig cfg = MiloTraceConfigFor(start_addr);

  ThreadBuffer* tb = GetThreadBuffer();

  // X6: this is an outbound call FROM the frame currently on top of the shadow
  // stack (its immediate caller, if that caller is itself traced). Record the
  // edge on the parent so its record carries a real dynamic outbound-call list.
  if (!tb->pending.empty()) {
    tb->pending.back().outbound_calls.push_back(start_addr);
  }

  PendingRecord rec;
  rec.func_va = start_addr;
  rec.thread_id = ctx->thread_id;
  rec.caller_va = static_cast<uint32_t>(ctx->lr) - 4;  // = bl site
  rec.depth_hint = static_cast<uint16_t>(tb->pending.size());
  rec.regs_in = SnapshotRegs(ctx);
  // X6-capture-fix: EXACT-MEM mode if either the session default or this addr's
  // manifest config asks for it. Drives the wider chase + Tier-C ACCESS_LOG.
  rec.exact_mem = st.config.exact_mem || cfg.exact_mem;

  CaptureStackWindow(ctx, cfg, rec);
  CaptureArgChase(ctx, cfg, rec, rec.truncated_chase);

  tb->pending.push_back(std::move(rec));
  (void)st;
}

// X6-capture-fix (ENTRY-PIN): the shared exit finalizer. `tail` distinguishes the
// TRUE epilog (real return — regs_out is the post-function state) from a TAIL
// site (the frame jumps away mid-function — regs_out is the args-to-tail-callee
// state). On a tail site the record is tagged so the consumer never matches
// regs_out as a clean return; on either path the exact-mem read-region capture
// closes the read graph from the observed accesses.
void MiloTraceFinishExit(PPCContext* ctx, uint32_t start_addr, bool tail) {
  MiloTraceState& st = State();
  ThreadBuffer* tb = GetThreadBuffer();
  if (tb->pending.empty()) return;

  // Match by func_va from the top of the shadow stack (defensive against a
  // missed entry/exit pairing — pop the matching frame).
  PendingRecord rec = std::move(tb->pending.back());
  tb->pending.pop_back();
  if (rec.func_va != start_addr) {
    // Mismatched pairing; the record is still emitted with its captured entry.
  }
  rec.tail_exit = tail;

  // X6-capture-fix (DEEPER CAPTURE / Tier C): fold the observed reads into the
  // captured node set BEFORE the post-image diff, so a member-deref page the
  // heuristic chase missed is part of mem_in (and its pre/post snapshot lines up).
  CaptureObservedReads(ctx, rec);

  mtr_regs_t regs_out = SnapshotRegs(ctx);
  uint64_t seq = st.call_seq.fetch_add(1, std::memory_order_relaxed);

  // X6: populate insn_count with the function's static instruction count (a
  // meaningful non-zero value for non-leaf records; the override-wrap path left
  // it 0). A true dynamic executed-instruction count is X7-class.
  uint32_t insn_count = StaticInsnCount(ctx, rec.func_va);

  SerializeRecord(tb->out, ctx, rec, regs_out, insn_count, seq);

  if (tb->out.size() >= kFlushThreshold) {
    std::lock_guard<std::mutex> lock(st.mutex);
    FlushThreadBufferLocked(st, tb);
  }
}

void MiloTraceOnExitCtx(void* ppc_context, uint32_t start_addr) {
  MiloTraceFinishExit(reinterpret_cast<PPCContext*>(ppc_context), start_addr,
                      /*tail=*/false);
}

void MiloTraceOnEntry(void* raw_context, uint64_t start_addr) {
  // CallNative ABI: raw_context is ThreadState** (x64_tracers.cc:55).
  auto* ts = *reinterpret_cast<ThreadState**>(raw_context);
  MiloTraceOnEntryCtx(ts->context(), static_cast<uint32_t>(start_addr));
}

void MiloTraceOnExit(void* raw_context, uint64_t start_addr) {
  auto* ts = *reinterpret_cast<ThreadState**>(raw_context);
  MiloTraceFinishExit(ts->context(), static_cast<uint32_t>(start_addr),
                      /*tail=*/false);
}

void MiloTraceOnExitTail(void* raw_context, uint64_t start_addr) {
  auto* ts = *reinterpret_cast<ThreadState**>(raw_context);
  MiloTraceFinishExit(ts->context(), static_cast<uint32_t>(start_addr),
                      /*tail=*/true);
}

// X6-capture-fix (DEEPER CAPTURE / Tier C): the per-load/store JIT instrumentation
// callback. Emitted (when the per-function exact-mem flag is set) at every guest
// memory access; appends (va,size,is_write) to the top pending record's access
// log. Cheap: a relaxed active check + a vector push under no lock (per-thread).
// Only records while a pending record is on this thread's shadow stack and that
// record is in exact_mem mode.
void MiloTraceMemAccess(void* raw_context, uint32_t address, uint32_t size,
                        uint32_t is_write) {
  if (!MiloTraceIsActive()) return;
  ThreadBuffer* tb = tls_buffer;  // do NOT create a buffer off the hot path
  if (!tb || tb->pending.empty()) return;
  PendingRecord& rec = tb->pending.back();
  if (!rec.exact_mem) return;
  if (rec.access_log.size() >= kMaxAccessLog) {
    rec.truncated_chase = true;  // log full => may be under-captured
    return;
  }
  rec.access_log.push_back(
      {address, static_cast<uint8_t>(size), static_cast<uint8_t>(is_write)});
}

int MiloTraceLoadManifest(std::string_view path) {
  MiloTraceState& st = State();
  std::ifstream f{std::string(path), std::ios::in | std::ios::binary};
  if (!f.is_open()) {
    XELOGW("milo-trace: manifest not found: '{}'", std::string(path));
    return -1;
  }
  std::string text((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());

  rapidjson::Document doc;
  doc.Parse(text.c_str());
  if (doc.HasParseError()) {
    XELOGW("milo-trace: manifest parse error at {}: {}",
           doc.GetErrorOffset(),
           rapidjson::GetParseError_En(doc.GetParseError()));
    return -1;
  }
  if (!doc.IsObject() || !doc.HasMember("addresses") ||
      !doc["addresses"].IsArray()) {
    XELOGW("milo-trace: manifest missing 'addresses' array");
    return -1;
  }

  auto parse_u32 = [](const rapidjson::Value& v, uint32_t& out) -> bool {
    if (v.IsString()) {
      out = static_cast<uint32_t>(strtoul(v.GetString(), nullptr, 0));
      return true;
    }
    if (v.IsUint()) {
      out = v.GetUint();
      return true;
    }
    if (v.IsInt()) {
      out = static_cast<uint32_t>(v.GetInt());
      return true;
    }
    return false;
  };

  int n = 0;
  for (auto& entry : doc["addresses"].GetArray()) {
    if (!entry.IsObject() || !entry.HasMember("addr")) continue;
    uint32_t addr = 0;
    if (!parse_u32(entry["addr"], addr)) continue;
    st.traced.insert(addr);

    MiloTraceAddrConfig cfg = st.default_cfg;
    if (entry.HasMember("exact_mem") && entry["exact_mem"].IsBool()) {
      cfg.exact_mem = entry["exact_mem"].GetBool();
    }
    // Optional Tier-A window override.
    if (entry.HasMember("mem_window_back")) {
      uint32_t v;
      if (parse_u32(entry["mem_window_back"], v)) cfg.mem_window_back = v;
    }
    if (entry.HasMember("mem_window_fwd")) {
      uint32_t v;
      if (parse_u32(entry["mem_window_fwd"], v)) cfg.mem_window_fwd = v;
    }
    st.addr_cfg[addr] = cfg;
    ++n;
  }
  XELOGI("milo-trace: manifest loaded {} addresses from '{}'", n,
         std::string(path));
  return n;
}

const std::unordered_set<uint32_t>& MiloTraceTracedAddresses() {
  return State().traced;
}

}  // namespace cpu
}  // namespace xe
