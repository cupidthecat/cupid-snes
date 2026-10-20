// Two guest processors exchange a flag while the console counts polling iterations.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace snes::tests::sa1 {

inline std::vector<uint8_t> MailboxProgram(uint8_t count, unsigned delay) {
  std::vector<uint8_t> code{0x78};
  const auto store = [&](uint16_t address, uint8_t value) {
    code.insert(code.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  store(0x2100, 0x80);
  store(0x2229, 0xff);
  store(0x3026, 0);
  store(0x2203, 0);
  store(0x2204, 0x81);
  code.insert(code.end(), {0xa2, 0});
  store(0x2200, 0);
  code.insert(code.end(), delay, 0xea);
  code.insert(code.end(), {0xe8, 0xad, 0x26, 0x30, 0xf0, 0xfa, 0x8e, 0,    0x10, 0x8d, 1,
                           0x10, 0xad, 0x37, 0x21, 0xad, 0x3c, 0x21, 0x8d, 2,    0x10, 0xdb});
  const std::array<uint8_t, 16> processor{0xa9, 0xff, 0x8d, 0x2a, 0x22, 0xa2, count, 0xca,
                                          0xd0, 0xfd, 0xa9, 1,    0x8d, 0x26, 0x30,  0xdb};
  std::vector<uint8_t> image(32768, 0xea);
  std::copy(code.begin(), code.end(), image.begin());
  std::copy(processor.begin(), processor.end(), image.begin() + 0x100);
  std::fill_n(image.begin() + 0x7fc0, 21, ' ');
  constexpr std::array<uint8_t, 11> header{0x23, 0x34, 5, 0, 1, 0, 0, 255, 255, 0, 0};
  std::copy(header.begin(), header.end(), image.begin() + 0x7fd5);
  image[0x7ffc] = 0;
  image[0x7ffd] = 0x80;
  return image;
}

} // namespace snes::tests::sa1
