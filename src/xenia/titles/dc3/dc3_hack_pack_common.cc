/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 hack-pack pieces used by BOTH layouts (NOT upstream).
 *
 * Moved verbatim out of decomp/dc3_hack_pack.cc: the address table
 * definition, the category names, and the content-cache wipe.
 ******************************************************************************
 */

#include "xenia/titles/dc3/dc3_hack_pack.h"

#include <filesystem>
#include <system_error>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/titles/dc3/dc3_addresses.h"

DEFINE_bool(
    dc3_clean_content_cache, true,
    "DC3 (0x373307D9) only: on every launch, recursively delete the whole "
    "<content_root>/373307D9 directory before the title starts. This DESTROYS "
    "DC3 save games, profiles and downloaded content -- it is not a cache "
    "scrub, it is remove_all() on the title's entire content tree. It exists "
    "because the decomp bring-up box wants a clean slate each boot and the "
    "DC3 boot probe's 627-trap baseline assumes it. Anyone who is not doing "
    "decomp bring-up should set this to false.",
    "DC3");

namespace xe {

Dc3Addresses kAddr;

const char* Dc3HackCategoryName(Dc3HackCategory category) {
  switch (category) {
    case Dc3HackCategory::kCrt:
      return "crt";
    case Dc3HackCategory::kSkeleton:
      return "skeleton";
    case Dc3HackCategory::kDebug:
      return "debug";
    case Dc3HackCategory::kDecompRuntimeStopgap:
      return "decomp_stopgap";
    case Dc3HackCategory::kImports:
      return "imports";
    default:
      return "unknown";
  }
}

void Dc3MaybeCleanStaleContentCache(const std::filesystem::path& content_root) {
  if (!cvars::dc3_clean_content_cache) {
    return;
  }
  auto dc3_content = content_root / "373307D9";
  if (std::filesystem::exists(dc3_content)) {
    XELOGW(
        "DC3: --dc3_clean_content_cache is set: recursively DELETING the "
        "entire DC3 content tree at {} (saves and all). Pass "
        "--dc3_clean_content_cache=false to keep it.",
        xe::path_to_utf8(dc3_content));
    std::error_code ec;
    std::filesystem::remove_all(dc3_content, ec);
    if (ec) {
      XELOGW("DC3: Failed to clean content cache at {}: {}",
             xe::path_to_utf8(dc3_content), ec.message());
    }
  }
}

}  // namespace xe
