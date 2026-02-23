#ifndef XENIA_DC3_HACK_PACK_H_
#define XENIA_DC3_HACK_PACK_H_

#include <filesystem>
#include <string>
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

void Dc3MaybeCleanStaleContentCache(const std::filesystem::path& content_root);
Dc3HackPackSummary ApplyDc3HackPack(const Dc3HackContext& ctx);
Dc3HackApplyResult ApplyDc3SkeletonHackPack(const Dc3HackContext& ctx);

}  // namespace xe

#endif  // XENIA_DC3_HACK_PACK_H_
