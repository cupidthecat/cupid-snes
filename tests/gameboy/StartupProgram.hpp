// Copy untouched startup memory into cartridge RAM using ordinary instructions.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace snes::tests::gameboy {

inline std::vector<uint8_t> StartupProgram() {
  std::vector<uint8_t> code{0xf3, 0x31, 0xfe, 0xff, 0x3e, 0x0a, 0xea, 0, 0, 0x11, 0, 0xa0};
  const auto copy = [&](uint16_t first, uint16_t count) {
    code.insert(code.end(), {0x21, uint8_t(first), uint8_t(first >> 8), 0x01, uint8_t(count),
                             uint8_t(count >> 8), 0x2a, 0x12, 0x13, 0x0b, 0x78, 0xb1, 0x20, 0xf8});
  };
  copy(0xc000, 8192);
  code.insert(code.end(), {0x3e, 1, 0xea, 0, 0x40, 0x11, 0, 0xa0});
  copy(0xff80, 127);
  copy(0xfe00, 160);
  copy(0xff30, 16);
  code.insert(code.end(), {0xfa, 0x2f, 0xa1, 0x3c, 0xea, 0x2f, 0xa1, 0x76});

  std::vector<uint8_t> image(32768);
  image[0x147] = 0x1b;
  image[0x149] = 3;
  image[0x100] = 0xc3;
  image[0x102] = 2;
  std::copy(code.begin(), code.end(), image.begin() + 0x200);
  return image;
}

} // namespace snes::tests::gameboy
