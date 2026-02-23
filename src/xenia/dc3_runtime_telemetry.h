#ifndef XENIA_DC3_RUNTIME_TELEMETRY_H_
#define XENIA_DC3_RUNTIME_TELEMETRY_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace xe {

struct Dc3RuntimeTelemetryConfig {
  std::string title_id;
  std::string build_kind;
  std::string resolver_mode;
  bool signature_resolver = false;
  bool guest_overrides = false;
};

void Dc3RuntimeTelemetryBeginSession(const Dc3RuntimeTelemetryConfig& config);
void Dc3RuntimeTelemetryEndSession(const char* reason, int64_t timeout_ms = -1);
bool Dc3RuntimeTelemetryIsActive();

void Dc3RuntimeTelemetryRecordBootMilestone(std::string_view name,
                                            uint32_t guest_pc = 0);

void Dc3RuntimeTelemetryRecordNuiResolverSummary(
    std::string_view resolver_mode, int manifest_hits, int symbol_hits,
    int signature_hits, int catalog_hits, int strict_rejects, int total);
void Dc3RuntimeTelemetryRecordNuiOverrideRegistered(std::string_view name,
                                                    uint32_t resolved_addr,
                                                    std::string_view method);
void Dc3RuntimeTelemetryRecordNuiPatchSummary(int patched, int overridden,
                                              int skipped, int total,
                                              std::string_view layout);
void Dc3RuntimeTelemetryRecordNuiOverrideHit(uint32_t guest_addr);

void Dc3RuntimeTelemetryRecordUnresolvedCallStubHit(std::string_view reason,
                                                    uint32_t guest_addr,
                                                    uint32_t callsite_pc);

void Dc3RuntimeTelemetryFlushAggregatesIfDue();
void Dc3RuntimeTelemetryFlushAggregatesNow();

}  // namespace xe

#endif  // XENIA_DC3_RUNTIME_TELEMETRY_H_
