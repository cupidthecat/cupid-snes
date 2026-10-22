#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

enum class PpuOffsetKind { None, Horizontal, Vertical };

inline std::vector<uint8_t> PpuOffsetTileProgram(unsigned mode, unsigned layer, bool large,
                                                 PpuOffsetKind offset, bool pal) {
  std::vector<uint8_t> code{0x78};
  const auto port = [&](unsigned address, unsigned value) {
    code.insert(code.end(), {0xa9, uint8_t(value), 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  const auto word = [&](unsigned address, unsigned value) {
    port(0x2116, address);
    port(0x2117, address >> 8);
    port(0x2118, value);
    port(0x2119, value >> 8);
  };
  port(0x2100, 0x80);
  port(0x2115, 0x80);
  const unsigned stride = mode == 2 ? 16 : layer == 0 ? 32 : 8;
  for (unsigned tile : {0u, 1u, 16u, 17u})
    for (unsigned row = 0; row < 8; ++row)
      word(tile * stride + row, bool(tile & 1) != bool(tile & 16) ? 0xff00 : 0x00ff);
  const unsigned enabled = 0x2000u << layer;
  if (offset == PpuOffsetKind::Horizontal)
    word(0x2400, enabled | 8);
  if (offset == PpuOffsetKind::Vertical)
    word(mode == 2 ? 0x2420 : 0x2400, enabled | (mode == 4 ? 0x8000 : 0) | 8);
  port(0x2107, 0x20);
  port(0x2108, 0x20);
  port(0x2109, 0x24);
  port(0x210b, 0);
  for (unsigned address : {0x210du, 0x210eu, 0x210fu, 0x2110u, 0x2111u, 0x2112u}) {
    port(address, 0);
    port(address, 0);
  }
  port(0x2121, 1);
  for (unsigned value : {0x1fu, 0u, 0xe0u, 3u})
    port(0x2122, value);
  port(0x2105, mode | (large ? 0x10u << layer : 0));
  port(0x212c, 1u << layer);
  port(0x2100, 15);
  code.insert(code.end(), {0x80, 0xfe});

  std::vector<uint8_t> rom(32768);
  std::copy(code.begin(), code.end(), rom.begin());
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  const uint8_t header[]{0x20, 0, 5, 0, uint8_t(pal ? 2 : 1), 0, 0, 255, 255, 0, 0};
  std::copy(std::begin(header), std::end(header), rom.begin() + 0x7fd5);
  rom[0x7ffc] = 0;
  rom[0x7ffd] = 0x80;
  return rom;
}
