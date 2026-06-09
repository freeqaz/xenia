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
};

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
bool IsGuestPointer(PPCContext* ctx, uint32_t va) {
  if (va < kPtrLo || va >= kPtrHi) return false;
  if (ctx->processor) {
    auto* memory = ctx->processor->memory();
    if (memory) {
      return memory->LookupHeap(va) != nullptr;
    }
  }
  return true;  // heuristic fallback
}

// Copy `len` BE guest bytes at `va` into `out`. Returns false if unmapped.
bool ReadGuest(PPCContext* ctx, uint32_t va, uint32_t len,
               std::vector<uint8_t>& out) {
  if (!IsGuestPointer(ctx, va)) return false;
  out.resize(len);
  std::memcpy(out.data(), GuestPtr(ctx, va), len);  // raw BE guest-native bytes
  return true;
}

// Read a 32-bit big-endian guest word at `va` (for pointer-chase seeds).
uint32_t ReadGuestBE32(PPCContext* ctx, uint32_t va) {
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

// Bounded BFS pointer chase from arg regs r3..r10 (schema §2.3 Tier B). Captures
// up to kMaxChaseNodes / kMaxChaseBytes; merges overlapping intervals coarsely
// (skips a seed already inside a captured node). Vtable special-case: if a node's
// first word points into a mapped region we still capture the node; full vtable
// slot capture + EDGES are an F2-side refinement (left to the pointer-chase lib).
void CaptureArgChase(PPCContext* ctx, const MiloTraceAddrConfig& cfg,
                     PendingRecord& rec) {
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

  size_t qi = 0;
  while (qi < queue.size()) {
    Hop hop = queue[qi++];
    if (static_cast<int>(rec.nodes.size()) >= kMaxChaseNodes) break;
    if (total_bytes >= kMaxChaseBytes) break;

    // Already captured (coarse interval-merge: inside an existing node)?
    bool covered = false;
    for (const auto& n : rec.nodes) {
      if (hop.va >= n.base &&
          hop.va < n.base + static_cast<uint32_t>(n.data.size())) {
        covered = true;
        break;
      }
    }
    if (covered) continue;

    uint32_t want = kNodeWindow;
    if (total_bytes + want > kMaxChaseBytes) {
      want = kMaxChaseBytes - total_bytes;
    }
    if (want < 4) break;

    CaptureNode node;
    node.base = hop.va;
    if (!ReadGuest(ctx, hop.va, want, node.data)) continue;
    node.node_id = static_cast<uint16_t>(rec.nodes.size());

    // Vtable heuristic: [obj+0] points into a mapped (likely .rdata) region.
    uint32_t first_word = ReadGuestBE32(ctx, hop.va);
    if (IsGuestPointer(ctx, first_word)) {
      node.flags |= 1u << 0;  // NODE_IS_VTABLE hint (a refinement; coarse here)
    }

    total_bytes += static_cast<uint32_t>(node.data.size());
    rec.pre_snapshot.push_back(node.data);
    rec.nodes.push_back(std::move(node));

    // Enqueue child pointers for the next hop.
    if (hop.depth + 1 <= kMaxChaseDepth) {
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

  // NOTE: per-thread buffers belonging to threads that already exited are lost;
  // active threads flush on their next OnExit. A whole-process drain on shutdown
  // is an X6-era refinement (a registry of live ThreadBuffers).
  if (tls_buffer) FlushThreadBufferLocked(st, tls_buffer);

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
  PendingRecord rec;
  rec.func_va = start_addr;
  rec.thread_id = ctx->thread_id;
  rec.caller_va = static_cast<uint32_t>(ctx->lr) - 4;  // = bl site
  rec.depth_hint = static_cast<uint16_t>(tb->pending.size());
  rec.regs_in = SnapshotRegs(ctx);

  CaptureStackWindow(ctx, cfg, rec);
  CaptureArgChase(ctx, cfg, rec);

  tb->pending.push_back(std::move(rec));
  (void)st;
}

void MiloTraceOnExitCtx(void* ppc_context, uint32_t start_addr) {
  PPCContext* ctx = reinterpret_cast<PPCContext*>(ppc_context);
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

  mtr_regs_t regs_out = SnapshotRegs(ctx);
  uint64_t seq = st.call_seq.fetch_add(1, std::memory_order_relaxed);

  SerializeRecord(tb->out, ctx, rec, regs_out, /*insn_count=*/0, seq);

  if (tb->out.size() >= kFlushThreshold) {
    std::lock_guard<std::mutex> lock(st.mutex);
    FlushThreadBufferLocked(st, tb);
  }
}

void MiloTraceOnEntry(void* raw_context, uint64_t start_addr) {
  // CallNative ABI: raw_context is ThreadState** (x64_tracers.cc:55).
  auto* ts = *reinterpret_cast<ThreadState**>(raw_context);
  MiloTraceOnEntryCtx(ts->context(), static_cast<uint32_t>(start_addr));
}

void MiloTraceOnExit(void* raw_context, uint64_t start_addr) {
  auto* ts = *reinterpret_cast<ThreadState**>(raw_context);
  MiloTraceOnExitCtx(ts->context(), static_cast<uint32_t>(start_addr));
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
