#pragma once

#include "MsuFixture.hpp"

#include <array>

namespace msu_test {

inline std::array<uint8_t, 256> AdapterBoot() {
  std::array<uint8_t, 256> boot{};
  boot[0] = 0xc3;
  boot[1] = 0xfc;
  boot[0xfc] = 0x3e;
  boot[0xfd] = 1;
  boot[0xfe] = 0xe0;
  boot[0xff] = 0x50;
  return boot;
}

inline std::vector<uint8_t> AdapterGame() {
  std::vector<uint8_t> game(32768);
  std::vector<uint8_t> sound;
  for (const auto [reg, value] : std::initializer_list<std::pair<uint8_t, uint8_t>>{{0x26, 0x80},
                                                                                    {0x24, 0x77},
                                                                                    {0x25, 0xff},
                                                                                    {0x11, 0x40},
                                                                                    {0x12, 0xf3},
                                                                                    {0x13, 0x87},
                                                                                    {0x14, 0x83}})
    sound.insert(sound.end(), {0x3e, value, 0xe0, reg});
  sound.insert(sound.end(), {0x18, 0xfe});
  std::copy(sound.begin(), sound.end(), game.begin() + 0x100);
  return game;
}

inline std::vector<uint8_t> ClockedAdapterRom(bool pal, bool readFence = false) {
  Program p;
  p.Store(0x6003, 0x81);
  const auto sound = p.Sound(true);
  p.Track(1);
  p.Store(0x2006, 255);
  p.Store(0x2007, 3);
  for (unsigned divider = 0; divider < 4; ++divider) {
    if (readFence)
      p.Emit({0xa2, 64, 0xad, 0, 0x20, 0xca, 0xd0, 0xfa});
    p.Store(0x6003, uint8_t(0x80 | divider));
    for (unsigned wait = 0; wait < 2; ++wait)
      p.Emit({0xad, 0x12, 0x42, 0x29, 0x80, 0xd0, 0xf9, 0xad, 0x12, 0x42, 0x29, 0x80, 0xf0, 0xf9});
    p.Track(uint16_t(divider % 2 + 1));
    p.Store(0x2007, 3);
  }
  p.Store(0x01ff, 0xa5);
  p.Emit({0xdb});
  if (p.code.size() >= 0x400)
    throw std::runtime_error("Adapter program exceeds its ROM region");
  auto bios = Rom(0, pal);
  bios.resize(0x80000);
  std::fill_n(bios.begin(), 0x400, 0);
  std::copy(p.code.begin(), p.code.end(), bios.begin());
  std::copy(sound.begin(), sound.end(), bios.begin() + 0x400);
  bios[0x7fd6] = 0xe3;
  bios[0x7fd7] = 9;
  return bios;
}

} // namespace msu_test
