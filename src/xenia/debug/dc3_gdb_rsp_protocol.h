/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026.                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_DEBUG_DC3_GDB_RSP_PROTOCOL_H_
#define XENIA_DEBUG_DC3_GDB_RSP_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef __linux__
#include <sys/socket.h>
#endif

namespace xe {
namespace debug {
namespace dc3_gdb_rsp {

enum class PacketReadKind { kPacket, kInterrupt, kBadChecksum, kEof };

struct PacketReadResult {
  PacketReadKind kind = PacketReadKind::kEof;
  std::string payload;
};

inline uint8_t HexNibble(char c) {
  if (c >= '0' && c <= '9') return uint8_t(c - '0');
  if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return uint8_t(c - 'A' + 10);
  return 0xFF;
}

inline void AppendHexByte(std::string& out, uint8_t v) {
  static const char kHex[] = "0123456789abcdef";
  out.push_back(kHex[(v >> 4) & 0xF]);
  out.push_back(kHex[v & 0xF]);
}

inline void AppendBe32Hex(std::string& out, uint32_t v) {
  AppendHexByte(out, uint8_t((v >> 24) & 0xFF));
  AppendHexByte(out, uint8_t((v >> 16) & 0xFF));
  AppendHexByte(out, uint8_t((v >> 8) & 0xFF));
  AppendHexByte(out, uint8_t(v & 0xFF));
}

inline bool ParseHexU32(const std::string& s, uint32_t& value) {
  if (s.empty()) return false;
  uint64_t acc = 0;
  for (char c : s) {
    uint8_t n = HexNibble(c);
    if (n == 0xFF) return false;
    acc = (acc << 4) | n;
    if (acc > 0xFFFFFFFFull) return false;
  }
  value = uint32_t(acc);
  return true;
}

inline bool ParseHexSize(const std::string& s, size_t& value) {
  if (s.empty()) return false;
  uint64_t acc = 0;
  for (char c : s) {
    uint8_t n = HexNibble(c);
    if (n == 0xFF) return false;
    acc = (acc << 4) | n;
    if (acc > (1ull << 31)) return false;
  }
  value = size_t(acc);
  return true;
}

#ifdef __linux__
inline PacketReadResult ReadPacketFromSocket(int fd) {
  for (;;) {
    uint8_t b = 0;
    ssize_t got = recv(fd, &b, 1, 0);
    if (got <= 0) return {PacketReadKind::kEof, {}};
    if (b == 0x03) return {PacketReadKind::kInterrupt, {}};
    if (b == '$') break;
    if (b == '+' || b == '-') continue;
  }

  std::string payload;
  uint8_t checksum = 0;
  while (true) {
    uint8_t b = 0;
    ssize_t got = recv(fd, &b, 1, 0);
    if (got <= 0) return {PacketReadKind::kEof, {}};
    if (b == '#') break;
    payload.push_back(static_cast<char>(b));
    checksum = static_cast<uint8_t>(checksum + b);
  }
  uint8_t csum_bytes[2];
  if (recv(fd, csum_bytes, 2, MSG_WAITALL) != 2) {
    return {PacketReadKind::kEof, {}};
  }
  uint8_t hi = HexNibble(static_cast<char>(csum_bytes[0]));
  uint8_t lo = HexNibble(static_cast<char>(csum_bytes[1]));
  if (hi == 0xFF || lo == 0xFF || uint8_t((hi << 4) | lo) != checksum) {
    return {PacketReadKind::kBadChecksum, {}};
  }
  return {PacketReadKind::kPacket, std::move(payload)};
}

inline bool SendPacketToSocket(int fd, const std::string& payload) {
  uint8_t checksum = 0;
  for (char c : payload) {
    checksum = static_cast<uint8_t>(checksum + uint8_t(c));
  }
  std::string frame;
  frame.reserve(payload.size() + 4);
  frame.push_back('$');
  frame += payload;
  frame.push_back('#');
  AppendHexByte(frame, checksum);
  return send(fd, frame.data(), frame.size(), 0) ==
         static_cast<ssize_t>(frame.size());
}
#endif  // __linux__

}  // namespace dc3_gdb_rsp
}  // namespace debug
}  // namespace xe

#endif  // XENIA_DEBUG_DC3_GDB_RSP_PROTOCOL_H_
