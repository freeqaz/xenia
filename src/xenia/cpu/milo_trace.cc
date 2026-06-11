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
#include "xenia/cpu/cpu_flags.h"
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

// A contiguous changed span in a captured interval (the Method-A write delta).
// Defined up here so CallSiteEffect (in PendingRecord) can carry a callee's own
// write delta. ComputeWriteSpans / AppendWritesSection (below) produce/consume it.
struct WriteSpan {
  uint32_t addr;
  std::vector<uint8_t> bytes;
};

// X8-CE (call effects): one finalized-or-pending outbound call observed while the
// owning FULL frame was on top of the shadow stack. X7 carried only `target_va`
// (a header-only CallEdge); X8-CE additionally captures the args-to-callee (at the
// bl) AND, once the callee returns, its return regs (r3/f1) + the parent frame's
// write delta over the call boundary — the per-call oracle EFFECT the a2i replay
// path injects. A `finalized` entry serializes EXACTLY like PutTailCallEntry
// (arg_mask r3..r10 + f1..f8, ret_mask R3|F1, nwrites spans); an unfinalized one
// keeps today's header-only bytes (honest absence — reads like the X7 ret=0/wr=0).
struct CallSiteEffect {
  uint32_t target_va = 0;
  uint32_t args_gpr[8] = {0};   // r3..r10 at the call boundary
  uint64_t args_fpr[8] = {0};   // f1..f8 at the call boundary (raw u64)
  bool has_args = false;
  bool has_ret = false;
  bool finalized = false;
  uint32_t ret_r3 = 0;
  uint64_t ret_f1 = 0;
  std::vector<WriteSpan> writes;
};

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
  // X6/X8-CE: ordered outbound calls observed while this frame was on top of the
  // shadow stack. Each nested OnEntry appends a CallSiteEffect to its parent's
  // list (X6 logged target_va only; X8-CE captures args at the bl and, on the
  // callee's exit, finalizes the entry with its ret regs + the parent's write
  // delta — see CallSiteEffect). With a trace-everything session (empty manifest)
  // or --milo_trace_call_effects this is the complete dynamic guest call list of
  // the FULL frame; with a manifest gate and no call_effects it covers only
  // manifest-traced callees.
  std::vector<CallSiteEffect> outbound_calls;

  // X8-CE: LIGHT-frame bookkeeping. A LIGHT frame is an un-manifested callee of a
  // traced ancestor, pushed only when --milo_trace_call_effects is on; it does NO
  // full capture — it exists solely to detect its own exit so it can finalize the
  // anchor (FULL) frame's CALLS[] entry with this callee's ret + write delta.
  bool light = false;
  // Descendants of a LIGHT frame do not get their own frames; the LIGHT frame on
  // top just counts them so the matching exits are balanced (LIFO).
  uint32_t skip_depth = 0;
  // For a LIGHT frame (or a FULL child whose entry was appended to its parent):
  // index into the anchor frame's outbound_calls of the entry this frame
  // finalizes on exit; -1 if none.
  int parent_call_index = -1;
  // For a LIGHT frame: index (in the pending shadow stack) of the anchor FULL
  // frame whose CALLS[] entry + node intervals this frame finalizes/diffs.
  size_t anchor_index = 0;
  // For a LIGHT frame: snapshot of the anchor frame's node intervals at the call
  // boundary (parallel to anchor.nodes). The write delta finalized on this
  // frame's exit is current-guest-bytes vs this snapshot. Captured at push time.
  std::vector<std::vector<uint8_t>> boundary_pre;
  // X8-CE: a FULL frame that consumed a tail continuation (a light tail chain
  // handed off to this manifested target) finalizes the parent's entry with the
  // CUMULATIVE anchor boundary diff (light chain + this frame), not its own
  // mem_out. True when boundary_pre above is the inherited anchor snapshot.
  bool inherited_boundary = false;

  // X6-capture-fix (DEEPER CAPTURE): this record is in EXACT-MEM mode (wider
  // Tier-B chase + Tier-C ACCESS_LOG). Set from MiloTraceConfig.exact_mem or a
  // per-addr cfg.exact_mem.
  bool exact_mem = false;

  // X6-capture-fix (ENTRY-PIN): the frame exited via a TAIL-call (the function
  // jumps away mid-frame), so regs_out is the args-to-tail-callee state, not a
  // clean return. The record is tagged with a `TAIL:` NOTE so the replay
  // consumer routes it correctly.
  bool tail_exit = false;

  // X7 (tail-CALLS oracle): on a TAIL bctr exit, the resolved tail-callee VA
  // (ctx->ctr at the bctr). The function's single outbound call is the tail call
  // itself; we emit a CALLS entry for this target carrying the args-to-tail-
  // callee (r3..r10, f1..f13 at the bctr, from regs_out) + the function's own
  // captured write delta (mem_writes attributable to it before the handoff). 0
  // when not a tail exit or the tail target is not a plausible code pointer.
  uint32_t tail_target_va = 0;

  // X6-capture-fix (DEEPER CAPTURE): the Tier-B chase hit a node/byte budget, so
  // the read graph may be incomplete (=> MTR_MF_TRUNCATED).
  bool truncated_chase = false;

  // X8-CE: the effect-carrying CALLS[] budget (kMaxCallEffects / bytes) was hit,
  // so some forward calls in this record are header-only despite call_effects
  // being on. Serialized as a CE_TRUNC NOTE so the consumer can tell a budget cap
  // from a genuine HLE/import header-only call.
  bool ce_truncated = false;
  // Running total of attached effect bytes (args + ret + write-span data) for the
  // budget check.
  size_t ce_bytes = 0;

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

// X8-CE budgets (bound the LIGHT-frame call-effect work on hot functions).
//  - kMaxCallEffects: at most this many effect-carrying (args/ret/writes) CALLS[]
//    entries per record. Beyond it, the call is recorded header-only + a CE_TRUNC
//    NOTE rides the record. 48 covers every non-leaf in the powered cohort
//    (W4 probe: nt<=7) with headroom; a hot dispatcher truncates honestly.
//  - kMaxCallEffectBytes: total attached effect bytes (args+ret+write-span data)
//    per record. 64 KB matches the access-log budget; protects a pathological
//    record from unbounded write-span capture.
//  - kMaxFinalizeWrites: u8 nwrites ceiling per finalized entry (the schema
//    field is u8); a callee that scribbles more spans is capped + flagged.
constexpr size_t kMaxCallEffects = 48;
constexpr size_t kMaxCallEffectBytes = 64 * 1024;
constexpr size_t kMaxFinalizeWrites = 255;

// X8-CE: a tail-call continuation token. A LIGHT frame C that tail-calls T fires
// its exit thunk with is_tail at the handoff (regs are the args-to-T, NOT the
// return). The token transfers C's call entry + boundary snapshot to T's frame so
// T's REAL exit finalizes the parent's entry with the true return + the cumulative
// boundary diff (C+T). Consumed by the IMMEDIATELY-following OnEntry (the tail
// jump lands on T's entry); cleared by any other event (stale handoff, e.g. an
// HLE tail target with no entry thunk -> the entry stays header-only, honest).
struct TailCont {
  bool pending = false;
  int parent_call_index = -1;
  size_t anchor_index = 0;
  std::vector<std::vector<uint8_t>> boundary_pre;
};

// Per-thread shadow stack of pending records (nested calls) + an output buffer.
struct ThreadBuffer {
  std::vector<PendingRecord> pending;  // shadow stack
  std::vector<uint8_t> out;            // serialized records awaiting flush
  TailCont tail_cont;                  // X8-CE tail-chain handoff (one in flight)
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

// Method-A write delta core: diff each captured interval (nodes[i] current guest
// bytes) vs its entry snapshot `pre[i]`, returning the changed spans (kind=0
// snapshot). schema §2.5. Factored out (X8-CE) so BOTH the record's own mem_out
// (nodes vs rec.pre_snapshot) AND a LIGHT-frame's call-boundary delta (anchor
// nodes vs the boundary_pre snapshot) walk identical logic — the producer and
// replay agree about what was written by construction.
std::vector<WriteSpan> ComputeWriteSpansFrom(
    PPCContext* ctx, const std::vector<CaptureNode>& nodes,
    const std::vector<std::vector<uint8_t>>& pre) {
  std::vector<WriteSpan> spans;
  // Walk only the overlap (pre and nodes are parallel; a boundary_pre captured at
  // call time may be shorter than nodes if the anchor grew between, so clamp).
  size_t count = nodes.size() < pre.size() ? nodes.size() : pre.size();
  for (size_t i = 0; i < count; ++i) {
    const CaptureNode& node = nodes[i];
    const std::vector<uint8_t>& pre_i = pre[i];
    uint32_t n = static_cast<uint32_t>(pre_i.size());
    // Re-read the post image at the same VA.
    std::vector<uint8_t> post;
    if (!ReadGuest(ctx, node.base, n, post)) continue;
    // ReadGuest clamps to the mapped run, so the post image may be shorter than
    // the entry snapshot (e.g. the stack contracted). Only diff the overlap.
    if (post.size() < n) n = static_cast<uint32_t>(post.size());
    // Walk for contiguous changed runs.
    uint32_t j = 0;
    while (j < n) {
      if (post[j] != pre_i[j]) {
        uint32_t start = j;
        while (j < n && post[j] != pre_i[j]) ++j;
        WriteSpan s;
        s.addr = node.base + start;
        s.bytes.assign(post.begin() + start, post.begin() + j);
        spans.push_back(std::move(s));
      } else {
        ++j;
      }
    }
  }
  return spans;
}

// Method-A write delta: the record's own mem_out (nodes vs entry snapshot).
// Shared by AppendWritesSection (the mem_out delta) and the X7 tail-CALLS entry
// (the tail call's attributable mem_writes), so the two never disagree about what
// the function wrote.
std::vector<WriteSpan> ComputeWriteSpans(PPCContext* ctx,
                                         const PendingRecord& rec) {
  return ComputeWriteSpansFrom(ctx, rec.nodes, rec.pre_snapshot);
}

// Method-A write delta: emit one WRITES section from precomputed changed spans
// (kind=0 snapshot). schema §2.5. The caller (SerializeRecord) computes the
// spans once via ComputeWriteSpans so the X7 tail-CALLS entry reuses the SAME
// delta.
void AppendWritesSection(std::vector<uint8_t>& b, const PendingRecord& rec,
                         const std::vector<WriteSpan>& spans, bool& any_writes) {
  (void)rec;
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

// Emit one CALLS entry header (mtr_call_hdr_t: src_offset u32, target_va u32,
// arg_mask u16, ret_mask u8, nwrites u8 == "<IIHBB", 12 bytes). The variable
// args/ret/writes that follow are gated by the masks (schema §3.5).
void PutCallHeaderOnly(std::vector<uint8_t>& payload, uint32_t src_offset,
                       uint32_t target_va) {
  PutU32(payload, src_offset);
  PutU32(payload, target_va);
  PutU16(payload, 0);  // arg_mask (no args captured)
  PutU8(payload, 0);   // ret_mask (no ret captured)
  PutU8(payload, 0);   // nwrites
}

// X7: emit the tail-CALLS entry — a full CallEdge for a TAIL bctr's resolved
// tail-callee. arg_mask covers r3..r10 (bits 0..7) + f1..f8 (bits 8..15); the
// args are the args-to-tail-callee at the bctr (== regs_out, the X6 entry-pin
// snapshot at the tail site). nwrites carries the function's own captured write
// delta (the member store a setter performed before the handoff). Encoded EXACTLY
// per schema §3.5 (codec.py _pack_one_call): header, then present GPR args (u32
// in reg order), present FPR args (u64 in reg order), then ret (none here), then
// nwrites WRITES entries.
//
// FPR arg width: arg_mask is u16, so the high byte (bits 8..15) carries f-args —
// i.e. f1..f8 (the codec's _CALL_FPR_ARGS is masked to this 8-bit window). A
// PPC32 setter passes its float in f1, well inside this set, so capturing
// f1..f8 covers every member-setter tail handoff.
void PutTailCallEntry(std::vector<uint8_t>& payload, uint32_t target_va,
                      const mtr_regs_t& regs_out,
                      const std::vector<WriteSpan>& spans) {
  // arg_mask: r3..r10 -> bits 0..7 (the full int arg set at the bctr); f1..f8 ->
  // bits 8..15. We mark the full representable ABI arg set present so the
  // consumer's tail oracle has the args-to-tail-callee register file.
  uint16_t arg_mask = 0;
  for (int i = 0; i < 8; ++i) arg_mask |= static_cast<uint16_t>(1u << i);
  for (int i = 0; i < 8; ++i) arg_mask |= static_cast<uint16_t>(1u << (8 + i));

  uint8_t nwrites = static_cast<uint8_t>(
      spans.size() > 0xFF ? 0xFF : spans.size());

  PutU32(payload, 0);          // src_offset (the tail handoff is at the bctr)
  PutU32(payload, target_va);  // RESOLVED tail-callee VA (ctx->ctr at the bctr)
  PutU16(payload, arg_mask);
  PutU8(payload, 0);           // ret_mask (the tail-callee's ret is its own, not
                               //  observed at the handoff — none captured)
  PutU8(payload, nwrites);

  // GPR args r3..r10 (u32), in reg order (bit order).
  for (int reg = 3; reg <= 10; ++reg) {
    PutU32(payload, regs_out.gpr[reg]);
  }
  // FPR args f1..f8 (raw u64), in reg order.
  for (int reg = 1; reg <= 8; ++reg) {
    PutU64(payload, regs_out.fpr[reg]);
  }
  // No ret regs (ret_mask == 0).
  // WRITES: mtr_write_entry_t {addr u32, len u16, kind u8, _pad u8} + data[len].
  for (uint8_t i = 0; i < nwrites; ++i) {
    const WriteSpan& s = spans[i];
    PutU32(payload, s.addr);
    PutU16(payload, static_cast<uint16_t>(s.bytes.size()));
    PutU8(payload, 0);  // kind = snapshot
    PutU8(payload, 0);  // _pad
    PutBytes(payload, s.bytes.data(), s.bytes.size());
  }
}

// X8-CE: emit one FINALIZED forward-call CALLS entry — a full CallEdge for a
// forward `bl` whose callee returned while the FULL frame was on the shadow
// stack. arg_mask covers r3..r10 + the present f1..f8 (the args-to-callee captured
// at the bl); ret_mask = R3|F1 (the callee's return regs, r3 THEN f1 order, per
// codec.py _RET_MASK_*); nwrites carries the parent's write delta over the call
// boundary (current anchor bytes vs the boundary snapshot). Encoded EXACTLY like
// PutTailCallEntry / codec.py _pack_one_call: header, GPR args, FPR args, ret r3,
// ret f1, then nwrites WRITES entries. This is the per-call oracle EFFECT the a2i
// consumer injects (engine_trace._build_oracle_calls reads c.ret + c.mem_writes).
void PutFinalizedCallEntry(std::vector<uint8_t>& payload,
                           const CallSiteEffect& eff) {
  // arg_mask: r3..r10 -> bits 0..7 (always marked present — the int arg file at
  // the bl), f1..f8 -> bits 8..15.
  uint16_t arg_mask = 0;
  for (int i = 0; i < 8; ++i) arg_mask |= static_cast<uint16_t>(1u << i);
  for (int i = 0; i < 8; ++i) arg_mask |= static_cast<uint16_t>(1u << (8 + i));

  // ret_mask = R3 (bit0) | F1 (bit1) — the codec narrows to r3/f1/cr.
  const uint8_t ret_mask = 0x1u | 0x2u;

  uint8_t nwrites = static_cast<uint8_t>(eff.writes.size() > kMaxFinalizeWrites
                                             ? kMaxFinalizeWrites
                                             : eff.writes.size());

  PutU32(payload, 0);                // src_offset (the bl site; 0 == not tracked)
  PutU32(payload, eff.target_va);    // the forward callee VA
  PutU16(payload, arg_mask);
  PutU8(payload, ret_mask);
  PutU8(payload, nwrites);

  // GPR args r3..r10 (u32), in reg order (bit order).
  for (int i = 0; i < 8; ++i) PutU32(payload, eff.args_gpr[i]);
  // FPR args f1..f8 (raw u64), in reg order.
  for (int i = 0; i < 8; ++i) PutU64(payload, eff.args_fpr[i]);
  // ret regs, r3 THEN f1 (matches codec.py _unpack_one_call order).
  PutU32(payload, eff.ret_r3);
  PutU64(payload, eff.ret_f1);
  // WRITES: mtr_write_entry_t {addr u32, len u16, kind u8, _pad u8} + data[len].
  for (uint8_t i = 0; i < nwrites; ++i) {
    const WriteSpan& s = eff.writes[i];
    PutU32(payload, s.addr);
    PutU16(payload, static_cast<uint16_t>(s.bytes.size()));
    PutU8(payload, 0);  // kind = snapshot
    PutU8(payload, 0);  // _pad
    PutBytes(payload, s.bytes.data(), s.bytes.size());
  }
}

// CALLS section (the outbound-call oracle, schema §2.6 / §3.5). X6 populated a
// real DYNAMIC outbound-call list — one header-only CallEdge per traced callee
// observed while this frame was active (only target_va known from the shadow
// stack). X8-CE FINALIZES forward-call entries with the callee's args/ret/writes
// (PutFinalizedCallEntry) when --milo_trace_call_effects captured them; an
// unfinalized entry (HLE/import callee, or budget-capped) keeps the header-only
// bytes (honest absence). X7 additionally synthesizes a FULL tail-CALLS entry on
// a TAIL bctr exit: the resolved tail target (ctx->ctr) + the args-to-tail-callee
// (regs_out) + the function's own write delta. The tail entry is emitted LAST (it
// IS the function's terminal outbound branch). Returns true iff a section was
// emitted.
bool AppendCallsSection(std::vector<uint8_t>& b, const PendingRecord& rec,
                        const mtr_regs_t& regs_out,
                        const std::vector<WriteSpan>& spans) {
  const bool emit_tail = rec.tail_exit && rec.tail_target_va != 0;
  if (rec.outbound_calls.empty() && !emit_tail) return false;
  uint32_t count = static_cast<uint32_t>(rec.outbound_calls.size()) +
                   (emit_tail ? 1u : 0u);
  std::vector<uint8_t> payload;
  PutU32(payload, count);
  for (const CallSiteEffect& eff : rec.outbound_calls) {
    if (eff.finalized) {
      PutFinalizedCallEntry(payload, eff);
    } else {
      PutCallHeaderOnly(payload, /*src_offset=*/0, eff.target_va);
    }
  }
  if (emit_tail) {
    PutTailCallEntry(payload, rec.tail_target_va, regs_out, spans);
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

  // WRITES (mem_out delta, Method A). Compute the changed spans ONCE so the
  // CALLS section's X7 tail entry reuses the SAME delta (they must not disagree
  // about what the function wrote).
  std::vector<WriteSpan> write_spans = ComputeWriteSpans(ctx, rec);
  bool any_writes = false;
  AppendWritesSection(sections, rec, write_spans, any_writes);
  if (any_writes) ++section_count;
  // Method A => scoped, so MTR_MF_WRITES_COMPLETE stays clear.

  // CALLS (X6 outbound-call oracle — target_va list, dynamic — plus the X7 tail-
  // CALLS entry on a TAIL bctr exit). Populates the calls[] axis for non-leaf
  // records (which the override-wrap path left empty) AND the single outbound
  // tail call for a vtable-tail-calling setter (X7).
  if (AppendCallsSection(sections, rec, regs_out, write_spans)) ++section_count;

  // ACCESS_LOG (X6-capture-fix Tier C — observed (va,size,is_write) reads/writes
  // during the call). Present only for exact_mem records.
  if (AppendAccessLogSection(sections, rec)) {
    ++section_count;
    mem_flags |= MTR_MF_TIER_C;
  }

  // X6-capture-fix: surface a too-shallow chase + a tail handoff so the consumer
  // (and the trust gate) can tell capture-completeness limits from clean records.
  if (rec.truncated_chase) mem_flags |= MTR_MF_TRUNCATED;
  // X8-CE: the call-effect budget was hit, so some forward calls are header-only
  // despite call_effects being on. A CE_TRUNC NOTE lets the consumer distinguish
  // a budget cap from a genuine HLE/import header-only call. Emitted BEFORE the
  // TAIL note so that — since the codec keeps only the LAST non-symbol NOTE in
  // rec.note — a TAIL record's `note` stays "TAIL:..." (is_tail_record must keep
  // working; the tail entry is the LAST calls[] entry the consumer pops off).
  if (rec.ce_truncated) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "CE_TRUNC:%08x", rec.func_va);
    AppendNoteSection(sections, std::string(buf));
    ++section_count;
  }
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

// X8-CE: fill `eff`'s args-to-callee from the call-boundary register file
// (r3..r10, f1..f8). At a FULL/LIGHT child's OnEntry the context IS the
// args-to-callee state (the bl just transferred control), so the entry register
// file is the call's argument frame.
void CaptureCallArgs(PPCContext* ctx, CallSiteEffect& eff) {
  for (int i = 0; i < 8; ++i) {
    eff.args_gpr[i] = static_cast<uint32_t>(ctx->r[3 + i]);  // r3..r10
  }
  for (int i = 0; i < 8; ++i) {
    uint64_t bits;
    std::memcpy(&bits, &ctx->f[1 + i], sizeof(bits));  // f1..f8 raw u64
    eff.args_fpr[i] = bits;
  }
  eff.has_args = true;
}

// X8-CE: snapshot a frame's captured node intervals (current guest bytes) for the
// call-boundary write diff. Parallel to `frame.nodes`.
std::vector<std::vector<uint8_t>> SnapshotNodeIntervals(PPCContext* ctx,
                                                        const PendingRecord& f) {
  std::vector<std::vector<uint8_t>> snap;
  snap.reserve(f.nodes.size());
  for (const auto& node : f.nodes) {
    std::vector<uint8_t> cur;
    ReadGuest(ctx, node.base, static_cast<uint32_t>(node.data.size()), cur);
    snap.push_back(std::move(cur));
  }
  return snap;
}

// X8-CE: is --milo_trace_call_effects active? (only meaningful when a session is
// active; callers check IsActive separately).
inline bool CallEffectsOn() { return cvars::milo_trace_call_effects; }

void MiloTraceOnEntryCtx(void* ppc_context, uint32_t start_addr) {
  PPCContext* ctx = reinterpret_cast<PPCContext*>(ppc_context);
  MiloTraceState& st = State();

  const bool manifested = MiloTraceShouldTrace(start_addr);
  const bool ce_on = MiloTraceIsActive() && CallEffectsOn();

  // ---- X8-CE LIGHT-frame path: an un-manifested callee of a traced ancestor. ----
  if (!manifested) {
    // No call-effects, or no live session: X7 behavior (the thunk only fired
    // because trace_all widened it; do nothing — byte-identical X7 capture).
    if (!ce_on) return;
    ThreadBuffer* tb = tls_buffer;  // do NOT create a buffer off the hot path
    if (!tb || tb->pending.empty()) {
      if (tb) tb->tail_cont.pending = false;  // stale token, no anchor
      return;
    }
    PendingRecord& top = tb->pending.back();

    // A grandchild of a LIGHT frame: its writes are inside the direct call's
    // boundary diff, so just count it for LIFO balance and do not push a frame.
    if (top.light) {
      tb->tail_cont.pending = false;  // any non-consuming entry clears the token
      top.skip_depth++;
      return;
    }

    // top is a FULL frame. A pending tail continuation transfers a prior light
    // frame's call entry + boundary snapshot to THIS frame (the tail jump landed
    // here) so its real exit finalizes the parent's entry (§3.4).
    if (tb->tail_cont.pending) {
      PendingRecord light;
      light.func_va = start_addr;
      light.light = true;
      light.parent_call_index = tb->tail_cont.parent_call_index;
      light.anchor_index = tb->tail_cont.anchor_index;
      light.boundary_pre = std::move(tb->tail_cont.boundary_pre);
      tb->tail_cont.pending = false;
      tb->pending.push_back(std::move(light));
      return;
    }

    // Budget cap: a hot dispatcher must not grow the effect list without bound.
    // Append a header-only entry, count the descendant (skip its subtree), flag.
    const bool over_count = top.outbound_calls.size() >= kMaxCallEffects;
    const bool over_bytes = top.ce_bytes >= kMaxCallEffectBytes;
    if (over_count || over_bytes) {
      CallSiteEffect eff;
      eff.target_va = start_addr;
      top.outbound_calls.push_back(std::move(eff));  // header-only, unfinalized
      top.ce_truncated = true;
      top.skip_depth++;  // its subtree is skipped (the entry stays header-only)
      return;
    }

    // Normal direct light call: capture args, append the (to-be-finalized) entry
    // to the FULL anchor, snapshot the anchor's node intervals at this boundary,
    // and push a LIGHT frame that will finalize the entry on its own exit.
    CallSiteEffect eff;
    eff.target_va = start_addr;
    CaptureCallArgs(ctx, eff);
    top.ce_bytes += 8 * 4 + 8 * 8 + 12;  // args + ret + header (write bytes TBD)
    size_t anchor_idx = tb->pending.size() - 1;
    int call_idx = static_cast<int>(top.outbound_calls.size());
    top.outbound_calls.push_back(std::move(eff));

    PendingRecord light;
    light.func_va = start_addr;
    light.light = true;
    light.parent_call_index = call_idx;
    light.anchor_index = anchor_idx;
    light.boundary_pre = SnapshotNodeIntervals(ctx, tb->pending[anchor_idx]);
    tb->pending.push_back(std::move(light));
    return;
  }

  // ---- FULL (manifested) frame path (X6/X7 capture unchanged + X8-CE pairing). --
  MiloTraceAddrConfig cfg = MiloTraceConfigFor(start_addr);
  ThreadBuffer* tb = GetThreadBuffer();

  // Any manifested OnEntry that does not consume a pending tail continuation
  // clears it (stale — a manifested tail target finalizes via its own frame, not
  // the light path; the inherited linkage is set below).
  bool inherited_tail = false;
  int inh_parent_call_index = -1;
  size_t inh_anchor_index = 0;
  std::vector<std::vector<uint8_t>> inh_boundary_pre;
  if (ce_on && tb->tail_cont.pending) {
    inherited_tail = true;
    inh_parent_call_index = tb->tail_cont.parent_call_index;
    inh_anchor_index = tb->tail_cont.anchor_index;
    inh_boundary_pre = std::move(tb->tail_cont.boundary_pre);
    tb->tail_cont.pending = false;
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

  // X6/X8-CE: record the outbound-call edge on the parent (if any).
  //  - Parent is FULL: a DIRECT manifested call. Append a (to-be-finalized) entry
  //    with args; this FULL frame's exit fills its ret + own write spans into it.
  //  - Parent is LIGHT (this FULL frame is a grandchild reached via a light call):
  //    do NOT add a direct-call entry — the light frame's boundary diff already
  //    represents this subtree to the anchor. Its own record is still captured.
  //  - inherited tail continuation: finalize the inherited entry (the prior tail
  //    chain's parent), not a fresh one.
  if (inherited_tail && inh_parent_call_index >= 0) {
    rec.parent_call_index = inh_parent_call_index;
    rec.anchor_index = inh_anchor_index;
    rec.boundary_pre = std::move(inh_boundary_pre);
    rec.inherited_boundary = true;
  } else if (!tb->pending.empty()) {
    PendingRecord& parent = tb->pending.back();
    if (!parent.light && ce_on &&
        parent.outbound_calls.size() < kMaxCallEffects &&
        parent.ce_bytes < kMaxCallEffectBytes) {
      CallSiteEffect eff;
      eff.target_va = start_addr;
      CaptureCallArgs(ctx, eff);
      parent.ce_bytes += 8 * 4 + 8 * 8 + 12;
      rec.parent_call_index = static_cast<int>(parent.outbound_calls.size());
      rec.anchor_index = tb->pending.size() - 1;
      parent.outbound_calls.push_back(std::move(eff));
    } else if (!parent.light) {
      // call_effects off (or budget hit): keep the X6 header-only edge so the
      // dynamic outbound-call list is still populated (byte-identical to X7 when
      // ce is off, since no args/ret are attached).
      CallSiteEffect eff;
      eff.target_va = start_addr;
      if (ce_on) parent.ce_truncated = true;
      parent.outbound_calls.push_back(std::move(eff));
    }
  }

  tb->pending.push_back(std::move(rec));
  (void)st;
}

// X6-capture-fix (ENTRY-PIN): the shared exit finalizer. `tail` distinguishes the
// TRUE epilog (real return — regs_out is the post-function state) from a TAIL
// site (the frame jumps away mid-function — regs_out is the args-to-tail-callee
// state). On a tail site the record is tagged so the consumer never matches
// regs_out as a clean return; on either path the exact-mem read-region capture
// closes the read graph from the observed accesses.
// X8-CE: finalize a parent's CALLS[] entry with a callee's return regs + write
// delta. `anchor_index`/`call_index` address the entry in the anchor FULL frame;
// `spans` is the write delta to attach (the FULL child's own mem_out, or the
// LIGHT child's anchor-boundary diff). Updates the anchor's ce_bytes budget.
void FinalizeParentEntry(ThreadBuffer* tb, size_t anchor_index, int call_index,
                         uint32_t ret_r3, uint64_t ret_f1,
                         std::vector<WriteSpan>&& spans) {
  if (call_index < 0 || anchor_index >= tb->pending.size()) return;
  PendingRecord& anchor = tb->pending[anchor_index];
  if (static_cast<size_t>(call_index) >= anchor.outbound_calls.size()) return;
  CallSiteEffect& eff = anchor.outbound_calls[call_index];
  if (eff.finalized) return;  // already finalized (defensive)
  // Cap write spans to the u8 nwrites field; account budget.
  if (spans.size() > kMaxFinalizeWrites) {
    spans.resize(kMaxFinalizeWrites);
    anchor.ce_truncated = true;
  }
  size_t bytes = 0;
  for (const auto& s : spans) bytes += 8 + s.bytes.size();
  anchor.ce_bytes += bytes;
  eff.ret_r3 = ret_r3;
  eff.ret_f1 = ret_f1;
  eff.has_ret = true;
  eff.writes = std::move(spans);
  eff.finalized = true;
}

void MiloTraceFinishExit(PPCContext* ctx, uint32_t start_addr, bool tail) {
  MiloTraceState& st = State();
  ThreadBuffer* tb = GetThreadBuffer();
  if (tb->pending.empty()) return;

  // X8-CE: a skipped descendant of a LIGHT frame (counted at OnEntry). Decrement
  // and return — its subtree is represented by the light frame's boundary diff.
  // LIFO guarantees the matching frame is the top LIGHT frame's counter.
  if (tb->pending.back().light && tb->pending.back().skip_depth > 0) {
    tb->pending.back().skip_depth--;
    return;
  }

  // X8-CE: a LIGHT frame's own exit. It finalizes the anchor's CALLS[] entry with
  // this callee's return + the anchor's call-boundary write delta — UNLESS this is
  // a tail handoff (regs are args-to-tail-callee, not the return), in which case a
  // TailCont token transfers the entry to the tail target's frame (§3.4).
  if (tb->pending.back().light) {
    PendingRecord light = std::move(tb->pending.back());
    tb->pending.pop_back();
    if (tail) {
      // Defer finalization to the tail target's REAL exit (cumulative boundary).
      tb->tail_cont.pending = true;
      tb->tail_cont.parent_call_index = light.parent_call_index;
      tb->tail_cont.anchor_index = light.anchor_index;
      tb->tail_cont.boundary_pre = std::move(light.boundary_pre);
      return;
    }
    if (light.parent_call_index >= 0 &&
        light.anchor_index < tb->pending.size()) {
      const PendingRecord& anchor = tb->pending[light.anchor_index];
      std::vector<WriteSpan> spans =
          ComputeWriteSpansFrom(ctx, anchor.nodes, light.boundary_pre);
      uint32_t ret_r3 = static_cast<uint32_t>(ctx->r[3]);
      uint64_t ret_f1;
      std::memcpy(&ret_f1, &ctx->f[1], sizeof(ret_f1));
      FinalizeParentEntry(tb, light.anchor_index, light.parent_call_index,
                          ret_r3, ret_f1, std::move(spans));
    }
    return;
  }

  // ---- FULL frame exit (X6/X7 record + X8-CE parent finalization). ----
  // Match by func_va from the top of the shadow stack (defensive against a
  // missed entry/exit pairing — pop the matching frame).
  PendingRecord rec = std::move(tb->pending.back());
  tb->pending.pop_back();
  if (rec.func_va != start_addr) {
    // Mismatched pairing; the record is still emitted with its captured entry.
  }
  rec.tail_exit = tail;

  // X7 (tail-CALLS oracle): on a TAIL bctr exit, the resolved tail-callee VA is
  // ctx->ctr (the bctr branches to CTR). The emitter pins this thunk at the tail
  // site (EmitTraceUserCallReturn(is_tail=true)) right before the bctr's
  // jmp(rax), so ctx->ctr already holds the resolved guest target of the indirect
  // tail (the vtable-slot value a setter loaded into CTR). Record it only when it
  // is a plausible code pointer; AppendCallsSection then emits the single
  // outbound tail call (target_va = this CTR) carrying the args-to-tail-callee
  // (regs_out) + the function's own write delta.
  if (tail) {
    uint32_t resolved = static_cast<uint32_t>(ctx->ctr);
    if (resolved >= kPtrLo && resolved < kPtrHi && (resolved & 0x3u) == 0) {
      rec.tail_target_va = resolved;
    }
  }

  // X6-capture-fix (DEEPER CAPTURE / Tier C): fold the observed reads into the
  // captured node set BEFORE the post-image diff, so a member-deref page the
  // heuristic chase missed is part of mem_in (and its pre/post snapshot lines up).
  CaptureObservedReads(ctx, rec);

  mtr_regs_t regs_out = SnapshotRegs(ctx);
  uint64_t seq = st.call_seq.fetch_add(1, std::memory_order_relaxed);

  // X8-CE: finalize this FULL frame's entry on its parent (a DIRECT manifested
  // call) with this record's return regs (r3/f1) + its OWN mem_out write spans.
  // The frame was already popped, so the anchor (parent) sits at its recorded
  // absolute index; on a tail exit regs_out is the args-to-tail-callee handoff
  // (NOT a return), so do not inject a wrong ret — leave the entry header-only
  // (honest), as the X7 tail behavior already did for the entry's perspective.
  if (rec.parent_call_index >= 0 && !tail &&
      rec.anchor_index < tb->pending.size()) {
    std::vector<WriteSpan> spans;
    if (rec.inherited_boundary) {
      // A manifested tail target of a light tail chain: the cumulative write
      // delta is the anchor's boundary diff (chain + this frame), not rec's own
      // mem_out (rec's nodes are this frame's, a DIFFERENT object than the anchor).
      const PendingRecord& anchor = tb->pending[rec.anchor_index];
      spans = ComputeWriteSpansFrom(ctx, anchor.nodes, rec.boundary_pre);
    } else {
      // A direct manifested call: this frame's own mem_out is its attributable
      // write delta (already the Method-A scoped delta the record serializes).
      spans = ComputeWriteSpans(ctx, rec);
    }
    uint32_t ret_r3 = regs_out.gpr[3];
    uint64_t ret_f1 = regs_out.fpr[1];
    FinalizeParentEntry(tb, rec.anchor_index, rec.parent_call_index, ret_r3,
                        ret_f1, std::move(spans));
  }

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
