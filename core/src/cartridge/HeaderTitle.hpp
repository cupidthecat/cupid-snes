#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace snes::core::detail {

inline std::string DecodeHeaderTitle(std::span<const uint8_t> bytes) {
  constexpr std::array<uint16_t, 63> kana{
      0x3002, 0x300c, 0x300d, 0x3001, 0x30fb, 0x30f2, 0x30a1, 0x30a3, 0x30a5, 0x30a7, 0x30a9,
      0x30e3, 0x30e5, 0x30e7, 0x30c3, 0x30fc, 0x30a2, 0x30a4, 0x30a6, 0x30a8, 0x30aa, 0x30ab,
      0x30ad, 0x30af, 0x30b1, 0x30b3, 0x30b5, 0x30b7, 0x30b9, 0x30bb, 0x30bd, 0x30bf, 0x30c1,
      0x30c4, 0x30c6, 0x30c8, 0x30ca, 0x30cb, 0x30cc, 0x30cd, 0x30ce, 0x30cf, 0x30d2, 0x30d5,
      0x30d8, 0x30db, 0x30de, 0x30df, 0x30e0, 0x30e1, 0x30e2, 0x30e4, 0x30e6, 0x30e8, 0x30e9,
      0x30ea, 0x30eb, 0x30ec, 0x30ed, 0x30ef, 0x30f3, 0xff9e, 0xff9f};
  std::string result;
  const auto append = [&](uint16_t point) {
    if (point < 0x80)
      result.push_back(char(point));
    else {
      result.push_back(char(0xe0 | (point >> 12)));
      result.push_back(char(0x80 | ((point >> 6) & 63)));
      result.push_back(char(0x80 | (point & 63)));
    }
  };
  for (size_t index = 0; index < bytes.size(); ++index) {
    const uint8_t byte = bytes[index];
    if (byte == 0 || byte == 0xff)
      continue;
    uint16_t point = byte >= 0xa1 && byte <= 0xdf   ? kana[byte - 0xa1]
                     : byte >= 0x20 && byte <= 0x7e ? byte
                                                    : '?';
    const uint8_t mark = index + 1 < bytes.size() ? bytes[index + 1] : 0;
    if (mark == 0xde &&
        (byte == 0xb3 || (byte >= 0xb6 && byte <= 0xc4) || (byte >= 0xca && byte <= 0xce))) {
      point = byte == 0xb3 ? 0x30f4 : uint16_t(point + 1);
      ++index;
    } else if (mark == 0xdf && byte >= 0xca && byte <= 0xce) {
      point += 2;
      // The halfwidth semi-voiced mark also remains a separate header character.
    }
    append(point);
  }
  const auto first = result.find_first_not_of(' ');
  if (first == std::string::npos)
    return {};
  return result.substr(first, result.find_last_not_of(' ') - first + 1);
}

} // namespace snes::core::detail
