#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sound_upload_test {
inline std::vector<uint8_t> PortProgram(unsigned setting, unsigned delay, bool pal) {
  std::vector<uint8_t> sound{0x8f, uint8_t(0x0a | setting << 6),
                             0xf0, // Select TEST's internal divider.
                             0x8f, 0x55,
                             0xf4, // Announce that upload completed.
                             0xe4, 0xf4,
                             0x68, 0x11,
                             0xd0, 0xfa,  // Wait for the console command.
                             0xc4, 0xf5}; // Acknowledge the command.
  sound.insert(sound.end(), 8, 0);        // Eight NOPs precede the sampled read.
  sound.insert(sound.end(), {0xe4, 0xf4, 0xc4, 0xf6, 0xff});
  std::vector<uint8_t> code;
  auto emit = [&](std::initializer_list<uint8_t> bytes) {
    code.insert(code.end(), bytes.begin(), bytes.end());
  };
  auto store = [&](uint16_t address, uint8_t value) {
    emit({0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  auto wait = [&](uint16_t address, uint8_t value) {
    emit({0xad, uint8_t(address), uint8_t(address >> 8), 0xc9, value, 0xd0, 0xf9});
  };
  wait(0x2140, 0xaa);
  wait(0x2141, 0xbb);
  store(0x2141, 1);
  store(0x2142, 0);
  store(0x2143, 2);
  store(0x2140, 0xcc);
  wait(0x2140, 0xcc);
  emit({0xa2, 0});
  const auto loop = code.size();
  emit({0xbd, 0, 0x84, 0x8d, 0x41, 0x21, 0x8a, 0x8d, 0x40, 0x21});
  emit({0xcd, 0x40, 0x21, 0xd0, 0xfb, 0xe8, 0xe0, uint8_t(sound.size()), 0xd0});
  emit({uint8_t(loop - code.size() - 1)});
  store(0x2141, 0);
  store(0x2142, 0);
  store(0x2143, 2);
  store(0x2140, uint8_t(sound.size() + 1));
  wait(0x2140, 0x55);
  store(0x2140, 0x11);
  wait(0x2141, 0x11);
  code.insert(code.end(), delay, 0xea);
  store(0x2140, 0x77);
  emit({0xad, 0x42, 0x21, 0xf0, 0xfb});
  emit({0x8f, 0, 1, 0x7e, 0x8f, 0, 1, 0x70});
  emit({0xa9, 0xa5, 0x8f, 1, 1, 0x7e, 0x8f, 1, 1, 0x70, 0xdb});
  std::vector<uint8_t> rom(0x8000);
  std::copy(code.begin(), code.end(), rom.begin());
  std::copy(sound.begin(), sound.end(), rom.begin() + 0x400);
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0x7fc0] = 'T';
  rom[0x7fd5] = 0x20;
  rom[0x7fd6] = 2;
  rom[0x7fd7] = 5;
  rom[0x7fd8] = 3;
  rom[0x7fd9] = pal ? 2 : 1;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffd] = 0x80;
  return rom;
}
} // namespace sound_upload_test
