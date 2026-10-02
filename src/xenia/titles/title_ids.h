/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Title IDs of the titles this fork carries per-title hooks for (NOT upstream).
 ******************************************************************************
 */

#ifndef XENIA_TITLES_TITLE_IDS_H_
#define XENIA_TITLES_TITLE_IDS_H_

#include <cstdint>

namespace xe {
namespace titles {

// Dance Central 3.
constexpr uint32_t kTitleDc3 = 0x373307D9;
// Rock Band 3 (retail TU5 and the RB3DX/RB3E lineage share it).
constexpr uint32_t kTitleRb3 = 0x45410914;

}  // namespace titles
}  // namespace xe

#endif  // XENIA_TITLES_TITLE_IDS_H_
