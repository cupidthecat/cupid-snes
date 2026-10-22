// A cached graphics program interrupts a console loop running from WRAM.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace snes::tests::gsu {

enum class Probe {
  None,
  Register,
  Cache,
  CacheWrite,
  UnusedStart,
  UnusedEnd,
  BankMirror,
  RomBank,
  BeforeWindow,
  RegisterMirror,
  VersionMirror,
  WindowEnd,
  AfterWindow
};

inline std::vector<uint8_t> IrqProgram(unsigned delay, bool pal, Probe probe = Probe::None) {
  std::vector<uint8_t> main{0x64, 0x42};
  const auto store = [&](uint16_t address, uint8_t value) {
    main.insert(main.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  for (uint16_t address : {0x1200, 0x1201, 0x1202, 0x1203, 0x1210})
    store(address, 0xa5);
  store(0x2100, 0x80);
  store(0x3034, 0);
  store(0x3039, 1);
  store(0x303a, 0x18);
  store(0x3037, 0);
  for (unsigned offset = 0; offset < 16; ++offset)
    store(uint16_t(0x3100 + offset), offset == 1 ? 0 : 1);
  store(0x301e, 0);
  main.insert(main.end(), delay, 0xea);
  store(0x301f, 0);
  constexpr std::array<std::array<uint8_t, 5>, 13> probes{{{},
                                                           {0xad, 0, 0x30},
                                                           {0xad, 0xff, 0x32},
                                                           {0xa9, 1, 0x8d, 0xff, 0x32},
                                                           {0xad, 0, 0x33},
                                                           {0xad, 0xff, 0x33},
                                                           {0xaf, 0, 0x30, 0x80},
                                                           {0xaf, 0, 0x30, 0x40},
                                                           {0xad, 0xff, 0x2f},
                                                           {0xad, 0, 0x34},
                                                           {0xad, 0x3b, 0x34},
                                                           {0xad, 0xff, 0x34},
                                                           {0xad, 0, 0x35}}};
  if (probe != Probe::None) {
    const auto index = unsigned(probe);
    const unsigned size = probe == Probe::CacheWrite                              ? 5
                          : probe == Probe::BankMirror || probe == Probe::RomBank ? 4
                                                                                  : 3;
    main.insert(main.end(), probes[index].begin(), probes[index].begin() + size);
    main.insert(main.end(), {0x8d, 0x10, 0x12});
  }
  main.insert(main.end(), {0x58, 0xa5, 0x42, 0xf0, 0xfc, 0xdb});

  // Copy the console loop to WRAM before granting cartridge ROM to the GSU.
  const std::array<uint8_t, 23> loader{0x78,
                                       0x18,
                                       0xfb,
                                       0xe2,
                                       0x30,
                                       0xa2,
                                       0,
                                       0xbd,
                                       0,
                                       0x90,
                                       0x9f,
                                       0,
                                       0x10,
                                       0x7e,
                                       0xe8,
                                       0xe0,
                                       uint8_t(main.size()),
                                       0xd0,
                                       0xf4,
                                       0x5c,
                                       0,
                                       0x10,
                                       0x7e};
  const std::array<uint8_t, 35> handler{0xad, 0x37, 0x21, 0xad, 0x3c, 0x21, 0x8d, 0,    0x12,
                                        0xad, 0x3c, 0x21, 0x8d, 1,    0x12, 0xad, 0x3d, 0x21,
                                        0x8d, 2,    0x12, 0xad, 0x3d, 0x21, 0x8d, 3,    0x12,
                                        0xad, 0x31, 0x30, 0xa9, 1,    0x85, 0x42, 0x40};
  std::vector<uint8_t> image(32768, 0xea);
  std::copy(loader.begin(), loader.end(), image.begin());
  std::copy(main.begin(), main.end(), image.begin() + 0x1000);
  std::copy(handler.begin(), handler.end(), image.begin() + 0x180);
  std::fill_n(image.begin() + 0x7fc0, 21, ' ');
  const std::array<uint8_t, 11> header{0x20, 0x13, 5, 6, uint8_t(pal ? 2 : 1), 0, 0,
                                       255,  255,  0, 0};
  std::copy(header.begin(), header.end(), image.begin() + 0x7fd5);
  image[0x7ffc] = 0;
  image[0x7ffd] = 0x80;
  for (unsigned vector : {0x7fee, 0x7ffe}) {
    image[vector] = 0x80;
    image[vector + 1] = 0x81;
  }
  return image;
}

} // namespace snes::tests::gsu
