#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace broadcast_test {

inline std::vector<uint8_t> ErasePollingRom(bool pal, bool delayedStart = false,
                                            unsigned slot = 0) {
  std::vector<uint8_t> code{0x78};
  const auto store = [&](uint32_t address, uint8_t value) {
    code.insert(code.end(), {0xa9, value, 0x8f, uint8_t(address), uint8_t(address >> 8),
                             uint8_t(address >> 16)});
  };
  for (unsigned reg : {12, 13, 14})
    store((reg << 16) | 0x5000, 0x80);
  // Use a sixteen-bit index to count actual reads of the busy flag.
  code.insert(code.end(), {0x18, 0xfb, 0xc2, 0x10});
  if (delayedStart)
    code.insert(code.end(), {0xa2, 0, 0x20, 0xca, 0xd0, 0xfd});
  code.insert(code.end(), {0xa2, 0, 0});
  const uint32_t window = slot == 2 ? 0xe00000 : 0xc00000;
  store(window, 0x20);
  store(window | 0x1234, 0xd0);
  code.insert(code.end(),
              {0xe8, 0xaf, 0, 0, uint8_t(window >> 16), 0x29, 0x80, 0xf0, 0xf7, 0x8e, 0, 2});
  store(0x120, 0xa5);
  code.push_back(0xdb);

  std::vector<uint8_t> rom(0x100000, 0xea);
  std::fill_n(rom.begin(), 0x3000, 0);
  std::copy(code.begin(), code.end(), rom.begin());
  std::fill_n(rom.begin() + 0x7fb0, 80, 0);
  std::copy_n("ZBSJ", 4, rom.begin() + 0x7fb2);
  std::copy_n("BROADCAST ERASE TEST  ", 21, rom.begin() + 0x7fc0);
  rom[0x7fd5] = 0x20;
  rom[0x7fd7] = 10;
  rom[0x7fd8] = 5;
  rom[0x7fd9] = pal ? 2 : 0;
  rom[0x7fda] = 0x33;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffc] = 0;
  rom[0x7ffd] = 0x80;
  if (slot)
    std::copy_n("ZGNJ", 4, rom.begin() + 0x7fb2);
  if (slot == 2) {
    std::copy_n(rom.begin(), 0x8000, rom.begin() + 0x8000);
    std::fill_n(rom.begin() + 0x7fb0, 80, 0);
    rom[0xffd5] = 0x21;
  }
  return rom;
}

inline std::vector<uint8_t> PageReadRom(unsigned slot = 0) {
  auto rom = ErasePollingRom(false, false, slot);
  std::vector<uint8_t> code{0x78};
  const auto store = [&](uint32_t address, uint8_t value) {
    code.insert(code.end(), {0xa9, value, 0x8f, uint8_t(address), uint8_t(address >> 8),
                             uint8_t(address >> 16)});
  };
  for (unsigned reg : {12, 14})
    store((reg << 16) | 0x5000, 0x80);
  const uint32_t window = slot == 2 ? 0xe00000 : 0xc00000;
  store(window, 0x75);
  for (unsigned page = 0; page < 2; ++page) {
    if (page)
      store(window, 0x72);
    code.insert(code.end(), {0xa2, 0, 0xbf, 0, 0, uint8_t(window >> 16), 0x9f, 0, uint8_t(3 + page),
                             0x7e, 0xe8, 0xd0, 0xf5});
  }
  store(0x120, 0xa5);
  code.push_back(0xdb);
  const size_t offset = slot == 2 ? 0x8000 : 0;
  std::fill_n(rom.begin() + offset, 0x3000, 0);
  std::copy(code.begin(), code.end(), rom.begin() + offset);
  const size_t header = slot == 2 ? 0xffc0 : 0x7fc0;
  std::fill_n(rom.begin() + header, 21, ' ');
  std::copy_n("FLASH PAGE STARTUP", 18, rom.begin() + header);
  return rom;
}

} // namespace broadcast_test
