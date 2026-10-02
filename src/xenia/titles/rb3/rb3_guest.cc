/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914) host-side guest readers (NOT upstream).
 ******************************************************************************
 */

#include "xenia/titles/rb3/rb3_guest.h"

#include <cctype>

#include "xenia/base/memory.h"
#include "xenia/memory.h"

namespace xe {
namespace titles {
namespace rb3 {

GuestReader::GuestReader(Memory* memory, bool trust_image_windows)
    : memory_(memory),
      membase_(memory->virtual_membase()),
      trust_image_windows_(trust_image_windows) {}

bool GuestReader::Readable(uint32_t address) const {
  if (address < 0x1000) {
    return false;
  }
  if (trust_image_windows_ &&
      ((address >= 0x82000000u && address < 0x83000000u) ||
       (address >= 0x84000000u && address < 0x84860000u))) {
    return true;
  }
  // Readable means the guest heap grants read access. "Committed" is not
  // enough: thread stacks are bracketed by committed kMemoryProtectNoAccess
  // guard pages (XThread::AllocateStack), and a thread that has not run yet
  // has r1 == stack_base, the first guard byte -- a back-chain walk from it
  // used to fault the host (soft-faulted reads with guest lr = r1 = 0, found
  // by Lane D).
  auto* heap = memory_->LookupHeap(address);
  uint32_t protect = 0;
  return heap && heap->QueryProtect(address, &protect) &&
         (protect & kMemoryProtectRead) != 0;
}

uint32_t GuestReader::R32(uint32_t address) const {
  return Readable(address) ? xe::load_and_swap<uint32_t>(membase_ + address)
                           : 0;
}

uint32_t GuestReader::R8(uint32_t address) const {
  return Readable(address) ? membase_[address] : 0;
}

std::string GuestReader::Str(uint32_t address, uint32_t max_len) const {
  std::string s;
  for (uint32_t i = 0; i < max_len; ++i) {
    if (!Readable(address + i)) {
      return std::string();
    }
    char c = static_cast<char>(membase_[address + i]);
    if (!c) {
      return s;
    }
    if (c < 0x20 || c > 0x7E) {
      return std::string();
    }
    s += c;
  }
  return std::string();
}

std::string GuestReader::Identifier(uint32_t address) const {
  std::string s = Str(address, 64);
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
      return std::string();
    }
  }
  return s;
}

uint32_t GuestReader::FindMainDirObject(const char* name) const {
  // ObjectDir: KeylessHash @+0x8 = {Entry* mEntries @+0, int mSize @+4};
  // Entry = {const char* name, Hmx::Object* obj}.
  uint32_t dir = R32(kMainDirPtr);
  if (!dir) {
    return 0;
  }
  uint32_t entries = R32(dir + 0x8);
  int size = static_cast<int>(R32(dir + 0xC));
  if (!entries || size <= 0 || size >= 200000) {
    return 0;
  }
  for (int i = 0; i < size; ++i) {
    uint32_t name_p = R32(entries + i * 8);
    uint32_t obj = R32(entries + i * 8 + 4);
    if (name_p && obj && Str(name_p) == name) {
      return obj;
    }
  }
  return 0;
}

std::string ReadCurrentScreenName(const GuestReader& reader) {
  uint32_t screen = reader.R32(kTheBandUI + kUiCurrentScreen);
  if (!screen || screen >= 0xF0000000u) {
    return std::string();
  }
  uint32_t name = reader.R32(screen + kScreenName);
  if (!name || name >= 0xF0000000u) {
    return std::string();
  }
  return reader.Identifier(name);
}

std::string ReadCurrentScreenName(Memory* memory) {
  return memory ? ReadCurrentScreenName(GuestReader(memory)) : std::string();
}

uint32_t ReadTransitionState(const GuestReader& reader) {
  return reader.R32(kTheBandUI + kUiTransitionState);
}

}  // namespace rb3
}  // namespace titles
}  // namespace xe
