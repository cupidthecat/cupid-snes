#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace sa1_test {

struct RomDmaFixture {
  std::vector<uint8_t> rom;
  std::vector<uint8_t> pack;
};

inline RomDmaFixture RomPortDma(uint32_t source, uint16_t count, uint8_t mapping, bool pal,
                                bool switchVectors = false, bool bwram = false) {
  const auto store = [](std::vector<uint8_t> &code, uint32_t address, uint8_t value) {
    code.insert(code.end(), {0xa9, value, 0x8f, uint8_t(address), uint8_t(address >> 8),
                             uint8_t(address >> 16)});
  };
  std::vector<uint8_t> worker{0x78, 0x18, 0xfb, 0xc2, 0x10};
  if (switchVectors)
    for (const auto [address, value] : {std::pair<uint32_t, uint8_t>{0x220c, 0x56},
                                        {0x220d, 0x78},
                                        {0x220e, 0x9a},
                                        {0x220f, 0xbc},
                                        {0x2209, 0x50}})
      store(worker, address, value);
  for (const auto [address, value] : {std::pair<uint32_t, uint8_t>{0x222a, 0xff},
                                      {0x2230, uint8_t(bwram ? 0x84 : 0x80)},
                                      {0x2232, uint8_t(source)},
                                      {0x2233, uint8_t(source >> 8)},
                                      {0x2234, uint8_t(source >> 16)},
                                      {0x2238, uint8_t(count)},
                                      {0x2239, uint8_t(count >> 8)},
                                      {0x2235, 0}})
    store(worker, address, value);
  if (bwram) {
    store(worker, 0x2236, 4);
    store(worker, 0x2237, 0);
  } else {
    store(worker, 0x2237, 0);
    store(worker, 0x2236, 4);
  }
  store(worker, 0x3020, 0x5a);
  worker.push_back(0xdb);

  std::vector<uint8_t> main{0x78, 0x18, 0xfb, 0xc2, 0x10};
  for (const auto [address, value] : {std::pair<uint32_t, uint8_t>{0x120, 0},
                                      {0x2229, 0xff},
                                      {0x2220, mapping},
                                      {0x2203, 0},
                                      {0x2204, 0x90},
                                      {0x2200, 0}})
    store(main, address, value);
  // Wait for the cartridge program to finish DMA before reading its complete result.
  main.insert(main.end(), {0xaf,
                           0x20,
                           0x30,
                           0,
                           0xc9,
                           0x5a,
                           0xd0,
                           0xf8,
                           0xa2,
                           0,
                           0,
                           0xbf,
                           0,
                           uint8_t(bwram ? 0x04 : 0x34),
                           uint8_t(bwram ? 0x40 : 0),
                           0x9f,
                           0,
                           2,
                           0x7e,
                           0xe8,
                           0xe0,
                           uint8_t(count),
                           uint8_t(count >> 8),
                           0xd0,
                           0xf2});
  store(main, 0x120, 0xa5);
  main.push_back(0xdb);

  RomDmaFixture fixture{std::vector<uint8_t>(0x400000), std::vector<uint8_t>(0x100000, 0x83)};
  for (unsigned bank = 0; bank < 64; ++bank)
    std::fill_n(fixture.rom.begin() + bank * 0x10000, 0x10000, uint8_t(bank * 29 + 0x31));
  for (auto *image : {&fixture.rom, &fixture.pack}) {
    std::copy(worker.begin(), worker.end(), image->begin() + 0x1000);
    std::copy(main.begin(), main.end(), image->begin() + 0x2000);
  }
  auto &rom = fixture.rom;
  std::fill_n(rom.begin() + 0x7fb0, 80, 0);
  std::copy_n("ZGNJ", 4, rom.begin() + 0x7fb2);
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  constexpr std::string_view title = "SA1 ROM DMA PORT";
  std::copy(title.begin(), title.end(), rom.begin() + 0x7fc0);
  rom[0x7fd5] = 0x23;
  rom[0x7fd6] = 0x35;
  rom[0x7fd7] = 12;
  rom[0x7fd8] = 5;
  rom[0x7fd9] = pal ? 2 : 0;
  rom[0x7fda] = 0x33;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffc] = 0;
  rom[0x7ffd] = 0xa0;
  return fixture;
}

} // namespace sa1_test
