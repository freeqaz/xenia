/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 */

#ifndef XENIA_DC3_NUI_PATCH_RESOLVER_H_
#define XENIA_DC3_NUI_PATCH_RESOLVER_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace xe {

class Memory;

namespace dc3 {

struct Dc3NuiPatchSpec {
  uint32_t address;
  uint32_t insn0;
  uint32_t insn1;
  const char* name;
};

struct Dc3TextSectionInfo {
  bool have_range = false;
  uint32_t start = 0;
  uint32_t end = 0;
  bool have_fingerprint = false;
  uint64_t fingerprint = 0;
};

enum class Dc3PatchResolveMethod {
  kPatchManifest,
  kCatalogAddress,
  kSymbolMap,
  kSignatureStub,
};

struct Dc3ResolvedNuiPatch {
  Dc3NuiPatchSpec spec;
  uint32_t resolved_address = 0;
  Dc3PatchResolveMethod resolve_method = Dc3PatchResolveMethod::kCatalogAddress;
  bool resolved = false;
  bool strict_rejected = false;
};

struct Dc3NuiSymbolManifest {
  std::unordered_map<std::string, uint32_t> text_symbols;
};

struct Dc3NuiPatchManifest {
  std::string build_label;
  std::optional<uint64_t> text_fingerprint;
  std::optional<uint64_t> runtime_text_fingerprint;
  std::unordered_map<std::string, uint32_t> targets;
  std::unordered_map<std::string, uint32_t> crt_sentinels;
};

struct Dc3FingerprintCache {
  std::optional<uint64_t> original;
  std::optional<uint64_t> decomp;
};

bool Dc3TryParseHexU64(std::string_view str, uint64_t* out_value);
std::optional<std::filesystem::path> Dc3AutoProbeNuiSymbolMapPath();
std::optional<std::filesystem::path> Dc3AutoProbeFingerprintCachePath();
std::optional<std::filesystem::path> Dc3AutoProbePatchManifestPath();

std::optional<Dc3NuiSymbolManifest> Dc3LoadNuiSymbolManifest(
    const std::filesystem::path& path);
std::optional<Dc3FingerprintCache> Dc3LoadFingerprintCacheFile(
    const std::filesystem::path& path);
std::optional<Dc3NuiPatchManifest> Dc3LoadNuiPatchManifest(
    const std::filesystem::path& path);

bool Dc3PatchTargetInText(const Dc3TextSectionInfo& text, uint32_t address,
                          uint32_t size = 8);
const char* Dc3PatchResolveMethodName(Dc3PatchResolveMethod method);

Dc3ResolvedNuiPatch Dc3ResolveNuiPatchTarget(
    const Dc3NuiPatchSpec& spec, const Dc3TextSectionInfo& text_info,
    const Dc3NuiPatchManifest* patch_manifest,
    const Dc3NuiSymbolManifest* symbol_manifest, std::string_view resolver_mode,
    Memory* memory, bool enable_signature_resolver);

}  // namespace dc3
}  // namespace xe

#endif  // XENIA_DC3_NUI_PATCH_RESOLVER_H_
