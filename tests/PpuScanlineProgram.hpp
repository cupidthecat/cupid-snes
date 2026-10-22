#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

inline std::vector<uint8_t> PpuGraphicsProgram(unsigned mode, bool objects, bool dynamic,
                                               bool extended, unsigned mosaic = 0,
                                               unsigned screenMask = 0xff) {
  std::vector<uint8_t> code;
  code.reserve(4096);
  code.push_back(0x78);
  const auto port = [&](uint16_t address, unsigned value) {
    code.insert(code.end(), {0xa9, uint8_t(value), 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  const auto dma = [&](uint16_t address, uint8_t bank, uint16_t size, uint8_t bus,
                       uint8_t control) {
    port(0x4300, control);
    port(0x4301, bus);
    port(0x4302, address);
    port(0x4303, address >> 8);
    port(0x4304, bank);
    port(0x4305, size);
    port(0x4306, size >> 8);
    port(0x420b, 1);
  };
  port(0x2100, 0x80);
  port(0x2115, 0x80);
  port(0x2116, 0);
  port(0x2117, 0);
  dma(0x8000, 1, 32768, 0x18, 1);
  dma(0x8000, 2, 32768, 0x18, 1);
  port(0x2121, 0);
  dma(0x9000, 0, 512, 0x22, 0);
  port(0x2102, 0);
  port(0x2103, 0);
  dma(0x9200, 0, 544, 4, 0);
  port(0x2101, 2);
  port(0x2105, mode);
  for (unsigned layer = 0; layer < 4; ++layer)
    port(uint16_t(0x2107 + layer), 0x60 + layer * 4);
  port(0x210b, 0);
  port(0x210c, 0);
  for (uint16_t address = 0x210d; address < 0x2115; ++address) {
    port(address, address & 1 ? 5 : 7);
    port(address, 0);
  }
  if (mode == 7) {
    for (uint16_t address = 0x211b; address <= 0x2120; ++address) {
      const unsigned value = address == 0x211b || address == 0x211e ? 256 : 0;
      port(address, value);
      port(address, value >> 8);
    }
  }
  port(0x2133, extended ? 0x40 : 0);
  port(0x212c, screenMask != 0xff ? screenMask : objects ? 0x10 : extended ? 3 : 1);
  port(0x212d, 0);
  if (mosaic)
    port(0x2106, mosaic);
  port(0x2100, 15);
  if (dynamic) {
    const auto loop = code.size();
    for (unsigned scroll : {0u, 7u}) {
      port(0x210d, scroll);
      port(0x210d, 0);
      code.insert(code.end(), {0xa2, uint8_t(scroll ? 9 : 11), 0xca, 0xd0, 0xfd});
    }
    const auto offset = int(loop) - int(code.size()) - 2;
    code.insert(code.end(), {0x80, uint8_t(offset)});
  } else {
    code.insert(code.end(), {0x80, 0xfe});
  }

  std::vector<uint8_t> rom(131072);
  std::copy(code.begin(), code.end(), rom.begin());
  for (unsigned index = 0; index < 256; ++index) {
    const auto color = (index * 3 & 31) | ((index * 5 & 31) << 5) | ((index * 7 & 31) << 10);
    rom[0x1000 + index * 2] = uint8_t(color);
    rom[0x1001 + index * 2] = uint8_t(color >> 8);
  }
  for (unsigned index = 0; index < 128; ++index) {
    rom[0x1200 + index * 4] = 44;
    rom[0x1201 + index * 4] = 240;
  }
  std::fill_n(rom.begin() + 0x1400, 32, 0x55);
  for (unsigned index = 0; index < 8; ++index) {
    rom[0x1200 + index * 4] = uint8_t(index * 25);
    rom[0x1201 + index * 4] = uint8_t(8 + index * 21);
    rom[0x1202 + index * 4] = uint8_t(index * 7);
    rom[0x1203 + index * 4] = uint8_t((index % 8) << 1 | (index % 4) << 4 | (index & 1 ? 0x40 : 0) |
                                      (index & 2 ? 0x80 : 0));
    rom[0x1400 + index / 4] &= uint8_t(~(3u << (index % 4 * 2)));
  }
  for (unsigned index = 0; index < 65536; ++index)
    rom[32768 + index] = uint8_t((index * 13) ^ (index / 8 * 7) ^ 0x5a);
  if (mode == 7) {
    for (unsigned index = 0; index < 32768; ++index) {
      rom[32768 + index * 2] = uint8_t(index * 7 + index / 128 * 13);
      rom[32769 + index * 2] = uint8_t(index * 3 + index / 8 * 11);
    }
  } else {
    for (unsigned layer = 0; layer < 4; ++layer)
      for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 32; ++x) {
          const unsigned entry = ((x * 3 + y * 13) & 255) | (((x + y + layer) & 7) << 10) |
                                 ((x >> 2 & 1) << 13) | ((x & 1) << 14) | ((y & 1) << 15);
          const auto offset = 32768 + (0x6000 + layer * 1024 + y * 32 + x) * 2;
          rom[offset] = uint8_t(entry);
          rom[offset + 1] = uint8_t(entry >> 8);
        }
  }
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  const uint8_t header[]{0x20, 0, 7, 0, 1, 0, 0, 255, 255, 0, 0};
  std::copy(std::begin(header), std::end(header), rom.begin() + 0x7fd5);
  rom[0x7ffc] = 0;
  rom[0x7ffd] = 0x80;
  return rom;
}
