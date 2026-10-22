// The console records raster counters when a second processor raises its interrupt.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace snes::tests::sa1 {

inline std::vector<uint8_t> DeferredIrqProgram(uint8_t count, unsigned delay, bool pal) {
  std::vector<uint8_t> code{0x78};
  const auto store = [&](uint16_t address, uint8_t value) {
    code.insert(code.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  store(0x2100, 0x80);
  for (unsigned byte = 0; byte < 16; ++byte)
    store(uint16_t(0x1000 + byte), 0);
  store(0x2203, 0);
  store(0x2204, 0x81);
  store(0x2201, 0x80);
  store(0x2200, 0);
  code.push_back(0x58);
  code.insert(code.end(), delay, 0xea);
  code.insert(code.end(), {0x80, 0xfe});
  constexpr std::array<uint16_t, 4> counters{0x213c, 0x213c, 0x213d, 0x213d};
  std::vector<uint8_t> interrupt{0xad, 0x37, 0x21};
  for (unsigned byte = 0; byte < counters.size(); ++byte)
    interrupt.insert(interrupt.end(),
                     {0xad, uint8_t(counters[byte]), 0x21, 0x8d, uint8_t(byte), 0x10});
  interrupt.insert(interrupt.end(),
                   {0xa9, 0x80, 0x8d, 0x02, 0x22, 0xa9, 0xa5, 0x8d, 0x08, 0x10, 0xdb});
  const std::array<uint8_t, 11> processor{0xa2, count, 0xca, 0xd0, 0xfd, 0xa9,
                                          0x80, 0x8d,  0x09, 0x22, 0xdb};
  std::vector<uint8_t> image(32768, 0xea);
  std::copy(code.begin(), code.end(), image.begin());
  std::copy(processor.begin(), processor.end(), image.begin() + 0x100);
  std::copy(interrupt.begin(), interrupt.end(), image.begin() + 0x1000);
  std::fill_n(image.begin() + 0x7fc0, 21, ' ');
  constexpr std::array<uint8_t, 11> header{0x23, 0x34, 5, 5, 1, 0, 0, 255, 255, 0, 0};
  std::copy(header.begin(), header.end(), image.begin() + 0x7fd5);
  image[0x7fd9] = pal ? 2 : 1;
  image[0x7ffc] = 0;
  image[0x7ffd] = 0x80;
  image[0x7ffa] = image[0x7ffe] = 0;
  image[0x7ffb] = image[0x7fff] = 0x90;
  return image;
}

inline std::vector<uint8_t> DeferredDmaProgram(uint8_t count, unsigned delay, bool pal) {
  auto image = DeferredIrqProgram(count, delay, pal);
  std::fill_n(image.begin(), 0x200, 0xea);
  std::vector<uint8_t> code{0x78};
  const auto store = [&](uint16_t address, uint8_t value) {
    code.insert(code.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  store(0x2100, 0x80);
  store(0x2229, 0xff);
  store(0x3000, 0);
  store(0x2181, 0);
  store(0x2182, 0x10);
  store(0x2183, 0);
  store(0x4300, 0);
  store(0x4301, 0x80);
  store(0x4302, 0);
  store(0x4303, 0x30);
  store(0x4304, 0);
  store(0x4305, 1);
  store(0x4306, 0);
  store(0x2203, 0);
  store(0x2204, 0x81);
  store(0x2200, 0);
  code.insert(code.end(), delay, 0xea);
  store(0x420b, 1);
  code.push_back(0xdb);
  const std::array<uint8_t, 16> processor{0xa9, 0xff, 0x8d, 0x2a, 0x22, 0xa2, count, 0xca,
                                          0xd0, 0xfd, 0xa9, 0x5a, 0x8d, 0,    0x30,  0xdb};
  std::copy(code.begin(), code.end(), image.begin());
  std::copy(processor.begin(), processor.end(), image.begin() + 0x100);
  return image;
}

} // namespace snes::tests::sa1
