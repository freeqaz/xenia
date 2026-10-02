/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914) guest layout + host-side readers (NOT
 * upstream). Shared by the RB3 title module's host threads.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_RB3_RB3_GUEST_H_
#define XENIA_TITLES_RB3_RB3_GUEST_H_

#include <cstdint>
#include <string>

namespace xe {
class Memory;
namespace titles {
namespace rb3 {

// Fixed addresses, identical in retail TU5 and the RB3DX repack (the RB3DX
// image differs from TU5 in 53 words, none of them these; RB3Enhanced's
// ports_xbox360.h pins the same values).
constexpr uint32_t kTheBandUI = 0x82DFD2B0;       // BandUI object (not a ptr)
constexpr uint32_t kMainDirPtr = 0x82E054B8;      // ObjectDir::sMainDir
constexpr uint32_t kTheBandUserMgr = 0x82E023B8;  // BandUserMgr*
constexpr uint32_t kJoypadData = 0x82CCB2C8;      // gJoypadData[4], 0xD4 each
constexpr uint32_t kJoypadStride = 0xD4;
constexpr uint32_t kJoypadUserOffset = 0x44;      // JoypadData::mUser
// UIManager (BandUI) fields.
constexpr uint32_t kUiTransitionState = 0x10;
constexpr uint32_t kUiCurrentScreen = 0x2C;
constexpr uint32_t kUiTransitionScreen = 0x30;
// UIScreen: name char* (Hmx::Object non-virtual base).
constexpr uint32_t kScreenName = 0x18;

// Host-side reads of guest memory from a host thread. Every read is gated on
// the guest heap granting read access to the page, so a stale pointer never
// faults the host; an unreadable word reads as 0.
class GuestReader {
 public:
  explicit GuestReader(Memory* memory);

  bool Readable(uint32_t address) const;
  uint32_t R32(uint32_t address) const;
  uint32_t R8(uint32_t address) const;
  // A printable NUL-terminated string of at most max_len chars; "" if the
  // address is unreadable or the bytes are not printable ASCII.
  std::string Str(uint32_t address, uint32_t max_len = 48) const;
  // A screen/object name: a clean identifier ([A-Za-z0-9_]), else "".
  std::string Identifier(uint32_t address) const;

  // ObjectDir::sMainDir name lookup: the Hmx::Object pointer the directory
  // stores for `name` (for classes with a virtual Object base, that is the
  // vbase at the object's tail), or 0.
  uint32_t FindMainDirObject(const char* name) const;

  uint8_t* membase() const { return membase_; }
  Memory* memory() const { return memory_; }

 private:
  Memory* memory_;
  uint8_t* membase_;
};

// The BandUI's current screen name ("" if the RB3 layout does not read
// plausibly). Validated strictly, so it is safe to call for any title.
std::string ReadCurrentScreenName(const GuestReader& reader);
// The same for callers outside the RB3 module (e.g. a scripted-input title
// adapter): reads only pages the guest heap reports readable or committed.
std::string ReadCurrentScreenName(Memory* memory);
// UIManager::mTransitionState (0 = idle).
uint32_t ReadTransitionState(const GuestReader& reader);

}  // namespace rb3
}  // namespace titles
}  // namespace xe

#endif  // XENIA_TITLES_RB3_RB3_GUEST_H_
