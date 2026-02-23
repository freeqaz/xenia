#include "xenia/dc3_runtime_telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DEFINE_bool(dc3_runtime_telemetry_enable, false,
            "DC3: enable structured runtime telemetry (JSONL) for DC3 bring-up "
            "analysis and parity tooling.",
            "DC3");
DEFINE_string(dc3_runtime_telemetry_path, "",
              "DC3: append-only JSONL telemetry output path. Ignored unless "
              "--dc3_runtime_telemetry_enable=true.",
              "DC3");
DEFINE_bool(dc3_runtime_telemetry_include_ppc_words, false,
            "DC3: reserved for heavier telemetry payloads (currently unused).",
            "DC3");

namespace xe {

namespace {

using Clock = std::chrono::steady_clock;

struct UnresolvedKey {
  uint32_t guest_addr = 0;
  uint32_t callsite_pc = 0;
  std::string reason;

  bool operator==(const UnresolvedKey& other) const {
    return guest_addr == other.guest_addr && callsite_pc == other.callsite_pc &&
           reason == other.reason;
  }
};

struct UnresolvedKeyHash {
  size_t operator()(const UnresolvedKey& key) const {
    size_t h1 = std::hash<uint32_t>{}(key.guest_addr);
    size_t h2 = std::hash<uint32_t>{}(key.callsite_pc);
    size_t h3 = std::hash<std::string>{}(key.reason);
    size_t h = h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    return h ^ (h3 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
  }
};

struct Dc3TelemetryState {
  std::mutex mutex;
  bool active = false;
  bool writer_failed = false;
  std::ofstream file;

  std::string title_id;
  std::string build_kind;
  std::string resolver_mode;
  bool signature_resolver = false;
  bool guest_overrides = false;
  std::string session_id;

  Clock::time_point start_time;
  Clock::time_point last_flush_time;

  // Aggregated counters flushed periodically.
  std::unordered_map<uint32_t, uint64_t> nui_override_hit_counts;
  std::unordered_map<uint32_t, std::string> nui_override_names;
  std::unordered_map<UnresolvedKey, uint64_t, UnresolvedKeyHash> unresolved_stub_hits;
  std::unordered_map<uint32_t, uint64_t> hot_loop_pc_counts;

  // Totals for summary.
  uint64_t total_nui_override_hits = 0;
  uint64_t total_unresolved_stub_hits = 0;
  uint64_t total_hot_loop_samples = 0;

  // Last-known NUI resolver/apply summaries.
  bool have_nui_resolver_summary = false;
  int resolver_manifest_hits = 0;
  int resolver_symbol_hits = 0;
  int resolver_signature_hits = 0;
  int resolver_catalog_hits = 0;
  int resolver_strict_rejects = 0;
  int resolver_total = 0;

  bool have_nui_patch_summary = false;
  int nui_patched = 0;
  int nui_overridden = 0;
  int nui_skipped = 0;
  int nui_total = 0;
  std::string nui_layout;

  // Reset all fields except mutex (non-copyable).
  void reset() {
    active = false;
    writer_failed = false;
    if (file.is_open()) file.close();
    title_id.clear();
    build_kind.clear();
    resolver_mode.clear();
    signature_resolver = false;
    guest_overrides = false;
    session_id.clear();
    start_time = {};
    last_flush_time = {};
    nui_override_hit_counts.clear();
    nui_override_names.clear();
    unresolved_stub_hits.clear();
    hot_loop_pc_counts.clear();
    total_nui_override_hits = 0;
    total_unresolved_stub_hits = 0;
    total_hot_loop_samples = 0;
    have_nui_resolver_summary = false;
    resolver_manifest_hits = 0;
    resolver_symbol_hits = 0;
    resolver_signature_hits = 0;
    resolver_catalog_hits = 0;
    resolver_strict_rejects = 0;
    resolver_total = 0;
    have_nui_patch_summary = false;
    nui_patched = 0;
    nui_overridden = 0;
    nui_skipped = 0;
    nui_total = 0;
    nui_layout.clear();
  }
};

Dc3TelemetryState& GetTelemetryState() {
  static Dc3TelemetryState state;
  return state;
}

std::atomic<uint64_t> g_dc3_session_counter{0};
std::atomic<bool> g_dc3_telemetry_active{false};

uint64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             Clock::now().time_since_epoch())
      .count();
}

uint64_t SessionRelativeMs(const Dc3TelemetryState& state, Clock::time_point now) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - state.start_time)
          .count());
}

void AppendJsonEscaped(std::string& out, std::string_view s) {
  for (char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += fmt::format("\\u{:04X}", static_cast<unsigned char>(c));
        } else {
          out.push_back(c);
        }
        break;
    }
  }
}

std::string JsonBase(const Dc3TelemetryState& state, Clock::time_point now,
                     std::string_view event_name) {
  std::string s;
  s.reserve(256);
  s += "{";
  s += fmt::format("\"ts_ms\":{},", SessionRelativeMs(state, now));
  s += "\"title_id\":\"";
  AppendJsonEscaped(s, state.title_id);
  s += "\",\"build_kind\":\"";
  AppendJsonEscaped(s, state.build_kind);
  s += "\",\"event\":\"";
  AppendJsonEscaped(s, event_name);
  s += "\",\"session_id\":\"";
  AppendJsonEscaped(s, state.session_id);
  s += "\",\"run_mode\":{";
  s += "\"resolver_mode\":\"";
  AppendJsonEscaped(s, state.resolver_mode);
  s += "\",";
  s += fmt::format("\"signature_resolver\":{},\"guest_overrides\":{}",
                   state.signature_resolver ? 1 : 0,
                   state.guest_overrides ? 1 : 0);
  s += "}";
  return s;
}

void WriteLineLocked(Dc3TelemetryState& state, std::string line) {
  if (!state.active || state.writer_failed || !state.file.is_open()) {
    return;
  }
  state.file << line << '\n';
  if (!state.file.good()) {
    state.writer_failed = true;
    XELOGW("DC3 telemetry: write failed, disabling telemetry for this session");
  }
}

void FlushAggregatesLocked(Dc3TelemetryState& state, Clock::time_point now) {
  if (!state.active || state.writer_failed) {
    return;
  }
  const uint64_t window_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last_flush_time)
          .count());

  for (const auto& [guest_addr, count_delta] : state.nui_override_hit_counts) {
    std::string line = JsonBase(state, now, "dc3_nui_override_hit");
    line += ",\"name\":\"";
    auto it = state.nui_override_names.find(guest_addr);
    if (it != state.nui_override_names.end()) {
      AppendJsonEscaped(line, it->second);
    } else {
      AppendJsonEscaped(line, "<unknown>");
    }
    line += "\"";
    line += fmt::format(",\"guest_addr\":\"{:08X}\",\"count_delta\":{}",
                        guest_addr, count_delta);
    line += "}";
    WriteLineLocked(state, std::move(line));
  }
  state.nui_override_hit_counts.clear();

  for (const auto& [key, count_delta] : state.unresolved_stub_hits) {
    std::string line = JsonBase(state, now, "dc3_unresolved_call_stub_hit");
    line += ",\"reason\":\"";
    AppendJsonEscaped(line, key.reason);
    line += "\"";
    line += fmt::format(",\"guest_addr\":\"{:08X}\",\"count_delta\":{}",
                        key.guest_addr, count_delta);
    if (key.callsite_pc) {
      line += fmt::format(",\"callsite_pc\":\"{:08X}\"", key.callsite_pc);
    }
    line += "}";
    WriteLineLocked(state, std::move(line));
  }
  state.unresolved_stub_hits.clear();

  std::vector<std::pair<uint32_t, uint64_t>> hot_entries;
  hot_entries.reserve(state.hot_loop_pc_counts.size());
  for (const auto& kv : state.hot_loop_pc_counts) {
    hot_entries.push_back(kv);
  }
  std::sort(hot_entries.begin(), hot_entries.end(),
            [](const auto& a, const auto& b) {
              if (a.second != b.second) return a.second > b.second;
              return a.first < b.first;
            });
  if (hot_entries.size() > 20) {
    hot_entries.resize(20);
  }
  for (const auto& [guest_pc, count] : hot_entries) {
    std::string line = JsonBase(state, now, "dc3_hot_loop_pc");
    line += fmt::format(",\"guest_pc\":\"{:08X}\",\"count\":{},\"window_ms\":{}",
                        guest_pc, count, window_ms);
    line += "}";
    WriteLineLocked(state, std::move(line));
  }
  state.hot_loop_pc_counts.clear();
  state.last_flush_time = now;
}

void MaybeFlushAggregatesLocked(Dc3TelemetryState& state, Clock::time_point now) {
  if (!state.active) {
    return;
  }
  if (now - state.last_flush_time >= std::chrono::milliseconds(1000)) {
    FlushAggregatesLocked(state, now);
  }
}

void ResetSessionStateLocked(Dc3TelemetryState& state) {
  state.active = false;
  state.writer_failed = false;
  if (state.file.is_open()) {
    state.file.close();
  }
  state.title_id.clear();
  state.build_kind.clear();
  state.resolver_mode.clear();
  state.signature_resolver = false;
  state.guest_overrides = false;
  state.session_id.clear();
  state.start_time = Clock::time_point{};
  state.last_flush_time = Clock::time_point{};
  state.nui_override_hit_counts.clear();
  state.nui_override_names.clear();
  state.unresolved_stub_hits.clear();
  state.hot_loop_pc_counts.clear();
  state.total_nui_override_hits = 0;
  state.total_unresolved_stub_hits = 0;
  state.total_hot_loop_samples = 0;
  state.have_nui_resolver_summary = false;
  state.resolver_manifest_hits = 0;
  state.resolver_symbol_hits = 0;
  state.resolver_signature_hits = 0;
  state.resolver_catalog_hits = 0;
  state.resolver_strict_rejects = 0;
  state.resolver_total = 0;
  state.have_nui_patch_summary = false;
  state.nui_patched = 0;
  state.nui_overridden = 0;
  state.nui_skipped = 0;
  state.nui_total = 0;
  state.nui_layout.clear();
}

}  // namespace

void Dc3RuntimeTelemetryBeginSession(const Dc3RuntimeTelemetryConfig& config) {
  auto& state = GetTelemetryState();
  std::lock_guard<std::mutex> lock(state.mutex);

  if (state.active) {
    // End previous session defensively if one leaked.
    auto now = Clock::now();
    FlushAggregatesLocked(state, now);
    ResetSessionStateLocked(state);
  }

  if (!cvars::dc3_runtime_telemetry_enable || cvars::dc3_runtime_telemetry_path.empty()) {
    g_dc3_telemetry_active.store(false, std::memory_order_release);
    return;
  }

  state.file.open(cvars::dc3_runtime_telemetry_path,
                  std::ios::out | std::ios::app);
  if (!state.file.is_open()) {
    XELOGW("DC3 telemetry: failed to open '{}'",
           cvars::dc3_runtime_telemetry_path);
    g_dc3_telemetry_active.store(false, std::memory_order_release);
    return;
  }

  state.title_id = config.title_id;
  state.build_kind = config.build_kind;
  state.resolver_mode = config.resolver_mode;
  state.signature_resolver = config.signature_resolver;
  state.guest_overrides = config.guest_overrides;
  const uint64_t seq = g_dc3_session_counter.fetch_add(1, std::memory_order_relaxed) + 1;
  state.session_id = fmt::format("{}-{}-{}", config.title_id, NowMs(), seq);
  state.start_time = Clock::now();
  state.last_flush_time = state.start_time;
  state.active = true;
  state.writer_failed = false;
  g_dc3_telemetry_active.store(true, std::memory_order_release);

  std::string line = JsonBase(state, state.start_time, "dc3_boot_milestone");
  line += ",\"name\":\"session_begin\"}";
  WriteLineLocked(state, std::move(line));
}

void Dc3RuntimeTelemetryEndSession(const char* reason, int64_t timeout_ms) {
  auto& state = GetTelemetryState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.active) {
    g_dc3_telemetry_active.store(false, std::memory_order_release);
    return;
  }

  auto now = Clock::now();
  FlushAggregatesLocked(state, now);

  std::string line = JsonBase(state, now, "dc3_summary");
  line += fmt::format(",\"reason\":\"{}\"", reason ? reason : "unknown");
  if (timeout_ms >= 0) {
    line += fmt::format(",\"timeout_ms\":{}", timeout_ms);
  }
  line += fmt::format(
      ",\"total_nui_override_hits\":{},\"total_unresolved_stub_hits\":{},"
      "\"total_hot_loop_samples\":{}",
      state.total_nui_override_hits, state.total_unresolved_stub_hits,
      state.total_hot_loop_samples);
  if (state.have_nui_resolver_summary) {
    line += fmt::format(
        ",\"nui_resolver\":{{\"manifest_hits\":{},\"symbol_hits\":{},"
        "\"signature_hits\":{},\"catalog_hits\":{},\"strict_rejects\":{},"
        "\"total\":{}}}",
        state.resolver_manifest_hits, state.resolver_symbol_hits,
        state.resolver_signature_hits, state.resolver_catalog_hits,
        state.resolver_strict_rejects, state.resolver_total);
  }
  if (state.have_nui_patch_summary) {
    line += fmt::format(
        ",\"nui_patch\":{{\"patched\":{},\"overridden\":{},\"skipped\":{},"
        "\"total\":{},\"layout\":\"{}\"}}",
        state.nui_patched, state.nui_overridden, state.nui_skipped,
        state.nui_total, state.nui_layout);
  }
  line += "}";
  WriteLineLocked(state, std::move(line));

  if (state.file.is_open()) {
    state.file.flush();
    state.file.close();
  }
  ResetSessionStateLocked(state);
  g_dc3_telemetry_active.store(false, std::memory_order_release);
}

bool Dc3RuntimeTelemetryIsActive() {
  return g_dc3_telemetry_active.load(std::memory_order_acquire);
}

void Dc3RuntimeTelemetryRecordBootMilestone(std::string_view name, uint32_t guest_pc) {
  if (!Dc3RuntimeTelemetryIsActive()) return;
  auto& state = GetTelemetryState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.active) return;
  auto now = Clock::now();
  MaybeFlushAggregatesLocked(state, now);
  std::string line = JsonBase(state, now, "dc3_boot_milestone");
  line += ",\"name\":\"";
  AppendJsonEscaped(line, name);
  line += "\"";
  if (guest_pc) {
    line += fmt::format(",\"guest_pc\":\"{:08X}\"", guest_pc);
  }
  line += "}";
  WriteLineLocked(state, std::move(line));
}

void Dc3RuntimeTelemetryRecordNuiResolverSummary(
    std::string_view resolver_mode, int manifest_hits, int symbol_hits,
    int signature_hits, int catalog_hits, int strict_rejects, int total) {
  if (!Dc3RuntimeTelemetryIsActive()) return;
  auto& state = GetTelemetryState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.active) return;
  state.have_nui_resolver_summary = true;
  state.resolver_mode = std::string(resolver_mode);
  state.resolver_manifest_hits = manifest_hits;
  state.resolver_symbol_hits = symbol_hits;
  state.resolver_signature_hits = signature_hits;
  state.resolver_catalog_hits = catalog_hits;
  state.resolver_strict_rejects = strict_rejects;
  state.resolver_total = total;
  auto now = Clock::now();
  MaybeFlushAggregatesLocked(state, now);
}

void Dc3RuntimeTelemetryRecordNuiOverrideRegistered(std::string_view name,
                                                    uint32_t resolved_addr,
                                                    std::string_view method) {
  if (!Dc3RuntimeTelemetryIsActive()) return;
  auto& state = GetTelemetryState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.active) return;
  state.nui_override_names[resolved_addr] = std::string(name);
  auto now = Clock::now();
  MaybeFlushAggregatesLocked(state, now);
  std::string line = JsonBase(state, now, "dc3_nui_override_registered");
  line += ",\"name\":\"";
  AppendJsonEscaped(line, name);
  line += "\"";
  line += fmt::format(",\"resolved_addr\":\"{:08X}\",\"resolve_method\":\"",
                      resolved_addr);
  AppendJsonEscaped(line, method);
  line += "\"}";
  WriteLineLocked(state, std::move(line));
}

void Dc3RuntimeTelemetryRecordNuiPatchSummary(int patched, int overridden,
                                              int skipped, int total,
                                              std::string_view layout) {
  if (!Dc3RuntimeTelemetryIsActive()) return;
  auto& state = GetTelemetryState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.active) return;
  state.have_nui_patch_summary = true;
  state.nui_patched = patched;
  state.nui_overridden = overridden;
  state.nui_skipped = skipped;
  state.nui_total = total;
  state.nui_layout = std::string(layout);
  auto now = Clock::now();
  MaybeFlushAggregatesLocked(state, now);
}

void Dc3RuntimeTelemetryRecordNuiOverrideHit(uint32_t guest_addr) {
  if (!Dc3RuntimeTelemetryIsActive()) return;
  auto& state = GetTelemetryState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.active) return;
  state.nui_override_hit_counts[guest_addr]++;
  state.total_nui_override_hits++;
  MaybeFlushAggregatesLocked(state, Clock::now());
}

void Dc3RuntimeTelemetryRecordUnresolvedCallStubHit(std::string_view reason,
                                                    uint32_t guest_addr,
                                                    uint32_t callsite_pc) {
  if (!Dc3RuntimeTelemetryIsActive()) return;
  auto& state = GetTelemetryState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.active) return;
  UnresolvedKey key;
  key.guest_addr = guest_addr;
  key.callsite_pc = callsite_pc;
  key.reason = std::string(reason);
  state.unresolved_stub_hits[std::move(key)]++;
  state.total_unresolved_stub_hits++;
  if (callsite_pc) {
    state.hot_loop_pc_counts[callsite_pc]++;
    state.total_hot_loop_samples++;
  }
  MaybeFlushAggregatesLocked(state, Clock::now());
}

void Dc3RuntimeTelemetryFlushAggregatesIfDue() {
  if (!Dc3RuntimeTelemetryIsActive()) return;
  auto& state = GetTelemetryState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.active) return;
  MaybeFlushAggregatesLocked(state, Clock::now());
}

void Dc3RuntimeTelemetryFlushAggregatesNow() {
  if (!Dc3RuntimeTelemetryIsActive()) return;
  auto& state = GetTelemetryState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.active) return;
  FlushAggregatesLocked(state, Clock::now());
}

}  // namespace xe
