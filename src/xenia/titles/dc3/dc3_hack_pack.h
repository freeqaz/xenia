#ifndef XENIA_DC3_HACK_PACK_H_
#define XENIA_DC3_HACK_PACK_H_

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace xe {

class Memory;

namespace cpu {
class Processor;
}

namespace kernel {
class UserModule;
}

enum class Dc3HackCategory {
  kCrt,
  kSkeleton,
  kDebug,
  kDecompRuntimeStopgap,
  kImports,
};

const char* Dc3HackCategoryName(Dc3HackCategory category);

struct Dc3HackContext {
  Memory* memory = nullptr;
  cpu::Processor* processor = nullptr;
  kernel::UserModule* module = nullptr;
  bool is_headless = false;
  bool is_decomp_layout = false;
  // Manifest-resolved addresses for hack-pack stubs (display_name -> guest addr).
  // When populated, these override hardcoded fallback addresses.
  const std::unordered_map<std::string, uint32_t>* hack_pack_stubs = nullptr;
  // CRT sentinel addresses from the manifest (__xc_a, __xc_z, __xi_a, __xi_z).
  const std::unordered_map<std::string, uint32_t>* crt_sentinels = nullptr;
  // TODO: Remove xdk_overrides once Xenia APU/NUI backends handle XDK APIs.
  const std::unordered_map<std::string, uint32_t>* xdk_overrides = nullptr;
  // XDK code ranges for prologue scanning (catches unlisted internal functions).
  struct CodeRange {
    uint32_t start;
    uint32_t end;
  };
  const std::vector<CodeRange>* xdk_code_ranges = nullptr;
};

struct Dc3HackApplyResult {
  Dc3HackCategory category = Dc3HackCategory::kDebug;
  int applied = 0;
  int skipped = 0;
  int failed = 0;
  std::vector<std::string> notes;
};

struct Dc3HackPackSummary {
  std::vector<Dc3HackApplyResult> results;
};

// Reads the DC3 IK telemetry scratch slots (populated by the always-firing
// guest byte-patch code-caves) plus the effector-`this` walk, and emits the
// per-frame "DC3:IK [frame N] ..." XELOGI lines.  Call this from a host hook
// that actually fires every frame during gameplay (e.g. Dc3NuiSequencerExtern),
// because the original HolmesClientPoll override never executes (see
// dc3_hack_pack.cc IK-telemetry notes).  Safe to call with a bad/unset state:
// it no-ops when telemetry isn't allocated and guards every guest read.
void ReadDc3IKTelemetry(Memory* memory, uint32_t frame);

void Dc3MaybeCleanStaleContentCache(const std::filesystem::path& content_root);
void Dc3PopulateAddressesFromCatalog(
    const std::unordered_map<std::string, uint32_t>& catalog,
    const std::unordered_map<std::string, uint32_t>& crt_sentinels);
Dc3HackPackSummary ApplyDc3HackPack(const Dc3HackContext& ctx);
Dc3HackApplyResult ApplyDc3SkeletonHackPack(const Dc3HackContext& ctx);
Dc3HackApplyResult ApplyDc3IKTelemetry(const Dc3HackContext& ctx);

}  // namespace xe

#endif  // XENIA_DC3_HACK_PACK_H_
