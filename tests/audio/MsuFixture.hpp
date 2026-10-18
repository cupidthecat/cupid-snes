#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace msu_test {

class Program {
public:
  std::vector<uint8_t> code;
  unsigned statuses = 0;
  void Emit(std::initializer_list<uint8_t> bytes) {
    code.insert(code.end(), bytes.begin(), bytes.end());
  }
  void Store(uint16_t address, uint8_t value) {
    Emit({0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  }
  void Wait(uint16_t address, uint8_t value) {
    Emit({0xad, uint8_t(address), uint8_t(address >> 8), 0xc9, value, 0xd0, 0xf9});
  }
  void Status() {
    Emit({0xad, 0, 0x20, 0x8f, uint8_t(statuses++), 1, 0x7e});
  }
  void Delay(unsigned count = 1) {
    while (count--)
      Emit({0xa2, 0, 0xe8, 0xd0, 0xfd});
  }
  void Track(uint16_t track) {
    Store(0x2004, uint8_t(track));
    Store(0x2005, uint8_t(track >> 8));
    Status();
  }
  void Mute(uint8_t flags) {
    Store(0x2140, flags);
    Wait(0x2141, flags);
  }
  std::vector<uint8_t> Sound(bool noise) {
    std::vector<uint8_t> sound{0x8f, 0x6c, 0xf2, 0x8f, 0x20, 0xf3, 0x8f, 0x55, 0xf4, 0xe4, 0xf4,
                               0x68, 0xcc, 0xf0, 0xfa, 0xc4, 0xf3, 0xc4, 0xf5, 0x2f, 0xf4};
    if (noise) {
      // Clear DSP registers, install a looping BRR block, then key on noise.
      std::vector<uint8_t> setup{0xcd, 0, 0xd8, 0xf2, 0x8f, 0, 0xf3, 0x3d, 0xc8, 0x80, 0xd0, 0xf6};
      auto ram = [&](uint16_t address, uint8_t value) {
        setup.insert(setup.end(), {0xe8, value, 0xc5, uint8_t(address), uint8_t(address >> 8)});
      };
      ram(0x400, 0);
      ram(0x401, 5);
      ram(0x402, 0);
      ram(0x403, 5);
      for (unsigned i = 0; i < 9; ++i)
        ram(uint16_t(0x500 + i), i == 0 ? 3 : 0);
      for (const auto [reg, value] :
           std::initializer_list<std::pair<uint8_t, uint8_t>>{{0x5d, 4},
                                                              {3, 0x10},
                                                              {0, 127},
                                                              {1, 127},
                                                              {0x0c, 127},
                                                              {0x1c, 127},
                                                              {7, 127},
                                                              {0x3d, 1},
                                                              {0x6c, 0x3f},
                                                              {0x4c, 1}})
        setup.insert(setup.end(), {0x8f, reg, 0xf2, 0x8f, value, 0xf3});
      setup.insert(setup.end(), {0x8f, 0x6c, 0xf2});
      setup.insert(setup.end(), sound.begin() + 6, sound.end());
      sound = std::move(setup);
    }
    // Wait for a console command instead of passing the IPL transfer counter to FLG.
    sound[sound.size() - 9] = uint8_t(sound.size() + 1);
    Wait(0x2140, 0xaa);
    Wait(0x2141, 0xbb);
    Store(0x2141, 1);
    Store(0x2142, 0);
    Store(0x2143, 2);
    Store(0x2140, 0xcc);
    Wait(0x2140, 0xcc);
    Emit({0xa2, 0});
    const auto loop = code.size();
    Emit({0xbd, 0, 0x84, 0x8d, 0x41, 0x21, 0x8a, 0x8d, 0x40, 0x21});
    Emit({0xcd, 0x40, 0x21, 0xd0, 0xfb, 0xe8, 0xe0, uint8_t(sound.size()), 0xd0});
    Emit({uint8_t(loop - code.size() - 1)});
    Store(0x2141, 0);
    Store(0x2142, 0);
    Store(0x2143, 2);
    Store(0x2140, uint8_t(sound.size() + 1));
    Wait(0x2140, 0x55);
    Mute(noise ? 0x3f : 0x20);
    return sound;
  }
};

inline std::vector<uint8_t> Rom(unsigned scene, bool pal) {
  Program p;
  p.Store(0x01ff, 0);
  p.Status();
  std::vector<uint8_t> sound;
  if (scene)
    sound = p.Sound(scene == 2);
  if (scene == 6) {
    p.Store(0x2006, 255);
    p.Store(0x2007, 3);
    p.Delay(2);
    p.Status();
  } else {
    p.Track(1);
    p.Store(0x2006, 255);
    p.Store(0x2007, scene == 3 ? 1 : 3);
    if (scene == 1 || scene == 2) {
      p.Delay();
      p.Store(0x2006, 73);
      p.Delay();
      p.Mute(0x40);
      p.Delay();
      p.Mute(0x20);
      p.Delay();
      p.Store(0x2007, 4);
      p.Status();
      p.Delay();
      p.Track(2);
      p.Store(0x2007, 3);
      p.Delay();
      p.Track(1);
      p.Store(0x2007, 3);
      p.Delay();
      p.Store(0x2007, 0);
      p.Status();
    } else if (scene == 3) {
      p.Delay(3);
      p.Status();
      p.Store(0x2007, 1);
      p.Delay();
      p.Status();
    } else if (scene == 4) {
      for (uint16_t track : {3, 4, 5, 6, 7, 8, 65535}) {
        p.Delay();
        p.Track(track);
        p.Store(0x2007, 3);
        p.Status();
      }
    } else if (scene == 7) {
      for (unsigned channel = 0; channel < 8; ++channel) {
        const uint16_t reg = uint16_t(0x4300 + channel * 16);
        unsigned offset = 0;
        for (uint8_t value : {8, 0x80, 1, 0x20, 0, 0, 0})
          p.Store(uint16_t(reg + offset++), value);
      }
      p.Store(0x420b, 255);
    } else if (scene == 5) {
      p.Delay();
      p.Store(0x2004, 2);
      p.Store(0x2007, 4);
      p.Status();
      p.Track(2);
      p.Store(0x2007, 3);
      p.Delay();
      p.Track(1);
      p.Store(0x2007, 3);
      p.Delay();
    }
  }
  p.Emit({0xa9, 0xa5, 0x8f, 0xff, 1, 0x7e, 0xdb});
  if (p.code.size() >= 0x400 || sound.size() >= 256)
    throw std::runtime_error("MSU test program exceeds its ROM region");
  std::vector<uint8_t> rom(0x8000, 0xea);
  std::copy(p.code.begin(), p.code.end(), rom.begin());
  std::copy(sound.begin(), sound.end(), rom.begin() + 0x400);
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  constexpr char title[] = "MSU CLOCK FIXTURE";
  std::copy_n(title, sizeof(title) - 1, rom.begin() + 0x7fc0);
  const std::initializer_list<uint8_t> header{0x20, 0,   5, 0, uint8_t(pal ? 2 : 1), 0, 0,
                                              255,  255, 0, 0};
  std::copy(header.begin(), header.end(), rom.begin() + 0x7fd5);
  rom[0x7ffc] = 0;
  rom[0x7ffd] = 0x80;
  return rom;
}

} // namespace msu_test
