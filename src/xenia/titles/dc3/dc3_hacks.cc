/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 (title 373307D9) hack registry (NOT upstream). See dc3_hacks.h.
 ******************************************************************************
 */

#include "xenia/titles/dc3/dc3_hacks.h"

#include <array>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"

DEFINE_string(dc3_disable_hacks, "",
              "DC3 (original debug.xex): comma-separated hack ids to NOT "
              "apply, for one-hack-at-a-time removal A/Bs. An entry ending in "
              "'*' is a prefix match ('splash.*'). Every id and what it masks "
              "is listed in docs/fork/dc3/PATCH_MANIFEST.md; each applied hack "
              "logs 'DC3 HACK on: <id>'. An unknown id is a launch error.",
              "DC3");

namespace xe {
namespace dc3 {

namespace {

// Every hack id that is decided outside the launch hooks (runtime writers and
// guest calls), plus the launch-time ones, so a disable list can be checked
// before the runtime ones are first reached. The SmartGlass table adds its
// per-function ids ("xbc.<Function>") as it is registered.
constexpr const char* kKnownIds[] = {
    // Launch-time image patches and overrides (dc3_title.cc).
    "mmio.soft_fault_range",
    "content.wipe",
    "saveload.activate",
    "speech.grammar_unload",
    // Runtime, scripted-input adapter (dc3_scripted_input.cc); only with
    // --dc3_headless_autonav.
    "input.attract_press",
};

struct DisableList {
  bool parsed = false;
  std::vector<std::string> exact;
  std::vector<std::string> prefixes;
};

std::mutex g_mutex;
DisableList g_disable;
std::set<std::string> g_queried;
std::set<std::string> g_fired;

void ParseLocked() {
  if (g_disable.parsed) {
    return;
  }
  g_disable.parsed = true;
  std::stringstream ss(cvars::dc3_disable_hacks);
  std::string item;
  while (std::getline(ss, item, ',')) {
    size_t a = item.find_first_not_of(" \t");
    size_t b = item.find_last_not_of(" \t");
    if (a == std::string::npos) {
      continue;
    }
    item = item.substr(a, b - a + 1);
    if (item.back() == '*') {
      g_disable.prefixes.push_back(item.substr(0, item.size() - 1));
    } else {
      g_disable.exact.push_back(item);
    }
  }
}

bool DisabledLocked(std::string_view id) {
  ParseLocked();
  for (const auto& e : g_disable.exact) {
    if (e == id) {
      return true;
    }
  }
  for (const auto& p : g_disable.prefixes) {
    if (id.substr(0, p.size()) == p) {
      return true;
    }
  }
  return false;
}

struct OverrideRecord {
  uint32_t address = 0;
  std::string id;
  std::atomic<uint64_t> hits{0};
  uint64_t logged_hits = ~uint64_t(0);
  bool logged_resolved = false;
};
// Filled during the launch hooks (single-threaded), read by handlers later.
// A fixed array so a handler never sees a reallocation.
constexpr size_t kMaxOverrides = 128;
std::array<OverrideRecord, kMaxOverrides> g_overrides;
std::atomic<size_t> g_override_count{0};

}  // namespace

bool HackEnabled(std::string_view id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_queried.emplace(id);
  return !DisabledLocked(id);
}

bool HackGate(std::string_view id, std::string_view what) {
  bool enabled = HackEnabled(id);
  if (enabled) {
    XELOGI("DC3 HACK on: {} ({})", id, what);
  } else {
    XELOGW("DC3 HACK off: {} (disabled by --dc3_disable_hacks; {})", id,
           what);
  }
  return enabled;
}

void HackFired(std::string_view id) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_fired.emplace(id).second) {
      return;
    }
  }
  XELOGI("DC3 HACK fired: {}", id);
}

void HackValidateDisableList() {
  std::lock_guard<std::mutex> lock(g_mutex);
  ParseLocked();
  std::set<std::string> known(g_queried);
  for (const char* id : kKnownIds) {
    known.emplace(id);
  }
  int bad = 0;
  for (const auto& e : g_disable.exact) {
    if (!known.count(e)) {
      XELOGE("DC3 HACK: UNKNOWN id '{}' in --dc3_disable_hacks (TAINTED: "
             "this run disables nothing for it)",
             e);
      ++bad;
    }
  }
  for (const auto& p : g_disable.prefixes) {
    bool any = false;
    for (const auto& k : known) {
      if (k.substr(0, p.size()) == p) {
        any = true;
        break;
      }
    }
    if (!any) {
      XELOGE("DC3 HACK: prefix '{}*' in --dc3_disable_hacks matches no hack "
             "(TAINTED)",
             p);
      ++bad;
    }
  }
  if (!cvars::dc3_disable_hacks.empty()) {
    XELOGI("DC3 HACK: --dc3_disable_hacks='{}' ({} unknown entries)",
           cvars::dc3_disable_hacks, bad);
  }
}

void HackNoteOverride(uint32_t guest_address, std::string_view id) {
  size_t n = g_override_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    if (g_overrides[i].address == guest_address) {
      g_overrides[i].id = std::string(id);
      return;
    }
  }
  if (n >= kMaxOverrides) {
    XELOGW("DC3 HACK: override audit table full; {:08X} {} not audited",
           guest_address, id);
    return;
  }
  g_overrides[n].address = guest_address;
  g_overrides[n].id = std::string(id);
  g_override_count.store(n + 1, std::memory_order_release);
}

void HackCountOverrideHit(uint32_t guest_address) {
  size_t n = g_override_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    if (g_overrides[i].address == guest_address) {
      g_overrides[i].hits.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

void HackLogOverrideAudit(cpu::Processor* processor, bool only_if_changed) {
  size_t n = g_override_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    auto& r = g_overrides[i];
    uint64_t hits = r.hits.load(std::memory_order_relaxed);
    // QueryFunction only returns functions that went through
    // ResolveFunction (entry table READY). A direct `bl` to an override is
    // emitted as CallExtern and never resolves, so a resolved override was
    // reached through the indirection table (bctrl/vtable/function pointer)
    // or a host Execute. Before the cpu override fix, DemandFunction compiled
    // the ORIGINAL guest body for that path, so indirect callers bypassed
    // the handler.
    bool resolved = processor && processor->QueryFunction(r.address);
    if (only_if_changed && hits == r.logged_hits &&
        resolved == r.logged_resolved) {
      continue;
    }
    r.logged_hits = hits;
    r.logged_resolved = resolved;
    // Since the core compiles an overridden function as "call the handler;
    // return" (cpu fix, Lane D), a resolved override with hits is simply
    // effective on both paths. Resolved with ZERO hits means indirect
    // callers were served the guest body: the pre-fix hole.
    const char* verdict =
        hits ? "effective"
             : (resolved ? "INERT (resolved, handler never ran: guest body)"
                         : "not reached");
    XELOGI("DC3 HACK audit: {} {:08X} handler_hits={} resolved_indirect={} "
           "-> {}",
           r.id, r.address, hits, resolved ? 1 : 0, verdict);
  }
}

void HackRegisterOverride(cpu::Processor* processor, uint32_t guest_address,
                          void (*handler)(cpu::ppc::PPCContext*,
                                          kernel::KernelState*),
                          std::string_view id, std::string name) {
  processor->RegisterGuestFunctionOverride(guest_address, handler,
                                           std::move(name));
  HackNoteOverride(guest_address, id);
}

}  // namespace dc3
}  // namespace xe
