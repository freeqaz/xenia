/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Kinect (NUI) HLE: masked word signatures for statically linked SDK code.
 ******************************************************************************
 */

#include "xenia/kernel/nui/nui_signature.h"

#include "xenia/base/memory.h"

namespace xe {
namespace kernel {
namespace nui {

namespace {

bool MatchAt(const uint8_t* p, const NuiSigWord* words, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    if ((xe::load_and_swap<uint32_t>(p + i * 4) & words[i].mask) !=
        words[i].value) {
      return false;
    }
  }
  return true;
}

}  // namespace

uint32_t FindSignature(const uint8_t* text, uint32_t text_address,
                       uint32_t size, const NuiSdkSignature& sig,
                       uint32_t* address) {
  uint32_t found = 0;
  if (!sig.word_count || size < sig.word_count * 4) {
    return 0;
  }
  const uint32_t first_value = sig.words[0].value;
  const uint32_t first_mask = sig.words[0].mask;
  const uint32_t last = size - sig.word_count * 4;
  for (uint32_t off = 0; off <= last; off += 4) {
    if ((xe::load_and_swap<uint32_t>(text + off) & first_mask) !=
        first_value) {
      continue;
    }
    if (!MatchAt(text + off, sig.words, sig.word_count)) {
      continue;
    }
    if (sig.follow_index >= 0) {
      const uint32_t insn_off = off + uint32_t(sig.follow_index) * 4;
      const uint32_t insn = xe::load_and_swap<uint32_t>(text + insn_off);
      if ((insn >> 26) != 18 || (insn & 2)) {
        continue;  // not a relative b/bl
      }
      int32_t li = int32_t(insn & 0x03FFFFFC);
      if (li & 0x02000000) {
        li -= 0x04000000;
      }
      const int64_t target_off = int64_t(insn_off) + li;
      if (target_off < 0 ||
          target_off + int64_t(sig.target_count) * 4 > int64_t(size) ||
          !MatchAt(text + target_off, sig.target, sig.target_count)) {
        continue;
      }
    }
    if (found++ == 0) {
      *address = text_address + off;
    }
  }
  return found;
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
