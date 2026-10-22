// Exercise the writable status flag and the processor's separate IRQ output.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace snes::tests::gsu {

inline std::vector<uint8_t> IrqSignalProgram(unsigned scene, bool pal) {
  std::vector<uint8_t> code{0x78, 0x9c, 0, 0x12};
  const auto store = [&](uint16_t address, uint8_t value) {
    code.insert(code.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  store(0x2100, 0x80);
  if (scene == 0 || scene == 1 || scene == 7) {
    store(0x3431, 0x80);
    if (scene == 1)
      code.insert(code.end(), {0xad, 0x31, 0x34});
  } else {
    store(0x3034, 0);
    store(0x3039, 1);
    // The complete cached program needs no cartridge ROM grant.
    store(0x303a, 8);
    store(0x3037, scene == 5 || scene == 6 ? 0x80 : 0);
    if (scene == 5)
      store(0x3431, 0x80);
    for (unsigned offset = 0; offset < 16; ++offset)
      store(uint16_t(0x3100 + offset), offset == 1 ? 0 : 1);
    store(0x301e, 0);
    store(0x301f, 0);
    code.insert(code.end(), {0xad, 0x30, 0x30, 0x29, 0x20, 0xd0, 0xf9});
    if (scene == 3 || scene == 4 || scene == 8)
      store(0x3431, 0);
    if (scene == 4)
      code.insert(code.end(), {0xad, 0x31, 0x34});
  }
  // These cases distinguish a real interrupt from a software status write.
  if (scene == 7 || scene == 8)
    code.insert(code.end(), {0x58, 0xea, 0xea});
  code.push_back(0xdb);

  const std::array<uint8_t, 9> handler{0xa9, 1, 0x8d, 0, 0x12, 0xad, 0x31, 0x34, 0x40};
  std::vector<uint8_t> image(32768, 0xea);
  std::copy(code.begin(), code.end(), image.begin());
  std::copy(handler.begin(), handler.end(), image.begin() + 0x180);
  std::fill_n(image.begin() + 0x7fc0, 21, ' ');
  const std::array<uint8_t, 11> header{0x20, 0x13, 5, 6, uint8_t(pal ? 2 : 1), 0, 0,
                                       255,  255,  0, 0};
  std::copy(header.begin(), header.end(), image.begin() + 0x7fd5);
  image[0x7ffc] = 0;
  image[0x7ffd] = 0x80;
  image[0x7ffe] = 0x80;
  image[0x7fff] = 0x81;
  return image;
}

} // namespace snes::tests::gsu
