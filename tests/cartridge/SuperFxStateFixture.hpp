#pragma once
#include "snes/core/SuperFx.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace snes::tests {
struct SuperFxStateFixture {
  std::vector<uint8_t> rom = std::vector<uint8_t>(0x10000);
  std::vector<uint8_t> ram = std::vector<uint8_t>(0x20000);
  snes::core::SuperFx chip{rom, ram};
  unsigned profile, scenario;
  SuperFxStateFixture(unsigned settings, unsigned program, size_t romSize = 0x10000,
                      size_t ramSize = 0x20000)
      : rom(romSize), ram(ramSize), chip(rom, ram), profile(settings), scenario(program) {
    for (unsigned n = 0; n < rom.size(); ++n)
      rom[n] = uint8_t(n * 43 + (n >> 8) * 7 + 11);
    for (unsigned n = 0; n < ram.size(); ++n)
      ram[n] = uint8_t(n * 31 + (n >> 8) * 3 + 7);
    chip.SetPal(profile & 1);
    chip.WriteCpu(0x3039, uint8_t((profile >> 1) & 1));
    chip.WriteCpu(0x303a, uint8_t(0x18 | (profile & 3)));
    Reg(0, 0x1234);
    Reg(1, 0x1000);
    Reg(2, 9);
    Reg(6, 7);
    Reg(14, 0x2345);
    std::vector<uint8_t> code;
    switch (scenario) {
    case 0:
      return;
    case 1:
      code = {0xf0, 0x34, 0x12, 0, 1};
      break;
    case 2:
      code = {2, 0xf0, 0x34, 0x12, 0, 1};
      break;
    case 3:
      code = {1, 5, 0xfe, 1};
      break;
    case 4:
      code = {0x3d, 0x31, 0x31, 0x90, 0, 1};
      break;
    case 5:
      code = {0x3d, 0x31, 0x41, 0, 1};
      break;
    case 6:
      code = {0x3d, 0x31, 0x3e, 0xf0, 0, 0x10, 0x3f, 0xf2, 0, 0x10, 0, 1};
      break;
    case 7:
      code = {0x80, 0x3d, 0x9f, 0, 1};
      break;
    case 8:
      code = {0x3f, 0xdf, 0xef, 0x3d, 0xef, 0x3e, 0xef, 0x3f, 0xef, 0xdf, 0, 1};
      break;
    case 9:
      code = {0x3d, 0x31, 0x3e, 0xdf, 0, 1};
      break;
    case 10:
      Reg(1, 7);
      code = {0x4e, 0x4c, 0xf1, 8, 0, 0x4c, 0xf1, 16, 0, 0x4c, 0x3d, 0x4c, 0, 1};
      break;
    case 11:
      Reg(1, 0);
      code = {0x4e, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c,
              0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x3d, 0x4c, 0,    1};
      break;
    case 12:
      Reg(1, 7);
      code = {0x4e, 0x4c, 0xf1, 8, 0, 0x4c, 0x3d, 0x4c, 0, 1};
      break;
    case 13:
      chip.WriteCpu(0x3034, 0x70);
      chip.WriteCpu(0x303a, 8);
      code = {2, 0xf0, 0x73, 0x15, 0, 1};
      break;
    case 14:
      chip.WriteCpu(0x303a, 0);
      code = {0xf0, 0x34, 0x12, 0, 1};
      break;
    case 15:
      chip.WriteCpu(0x303a, 0x10);
      code = {0x41, 0, 1};
      break;
    case 16:
      code = {2, 0xf0, 0x34, 0x12};
      break;
    case 17:
      code = {0x3d, 0x31, 1, 0, 1};
      break;
    }
    const bool cached = (scenario >= 3 && scenario <= 12) || scenario == 15 || scenario == 17;
    const uint16_t start = cached ? 0 : scenario == 16 ? 0xfffc : 0x8000;
    if (cached) {
      for (unsigned n = 0; n < 32; ++n)
        chip.WriteCpu(0x3100 + n, n < code.size() ? code[n] : 1);
    } else {
      std::copy(code.begin(), code.end(), rom.begin() + (start & 0x7fff));
      std::copy(code.begin(), code.end(), ram.begin() + start);
      if (scenario == 16) {
        rom[0] = ram[0] = 0;
        rom[1] = ram[1] = 1;
      }
    }
    Reg(15, start);
  }
  void Reg(unsigned index, uint16_t value) {
    chip.WriteCpu(0x3000 + index * 2, uint8_t(value));
    chip.WriteCpu(0x3001 + index * 2, uint8_t(value >> 8));
  }
  void Instruction(unsigned opcode, unsigned mode) {
    const std::array<uint8_t, 8> code{
        mode ? uint8_t(0x3c + mode) : uint8_t(1), uint8_t(opcode), 0x40, 0x10, 1, 0, 1, 0};
    for (unsigned n = 0; n < 32; ++n)
      chip.WriteCpu(0x3100 + n, n < code.size() ? code[n] : 1);
    Reg(15, 0);
  }
  uint16_t Reg(unsigned index) {
    return uint16_t(chip.ReadCpu(0x3000 + index * 2) |
                    (uint16_t(chip.ReadCpu(0x3001 + index * 2)) << 8));
  }
  void Advance(unsigned clocks) {
    if (profile & 4)
      chip.AdvanceMasterClocks(clocks);
    else
      chip.Advance(clocks);
  }
  void Drive(unsigned cut) {
    if (scenario == 17 && cut >= 4) {
      chip.WriteCpu(0x303a, 0);
      if (cut % 6 == 4)
        Reg(14, uint16_t(0x3000 + cut));
    }
  }
  void Edit(unsigned cut, unsigned tail) {
    Reg(0, uint16_t(0xa500 + cut * 13 + tail));
    Reg(14, uint16_t(0x4100 + cut + tail));
    if (tail == 2) {
      chip.WriteCpu(0x3031, uint8_t(cut & 3));
      chip.WriteCpu(0x3039, uint8_t(cut & 1));
      chip.WriteCpu(0x3037, uint8_t(cut & 0xa0));
      chip.WriteCpu(0x3038, uint8_t(cut));
      chip.WriteCpu(0x303a, 0);
    }
    if (tail == 3)
      chip.WriteCpu(0x303a, uint8_t(0x18 | (profile & 3)));
    if (tail == 4) {
      Reg(1, uint16_t(cut));
      Reg(2, uint16_t(cut * 3));
      Reg(15, uint16_t(cut & 15));
    }
    chip.WriteCpu(0x701000 + (cut & 255), uint8_t(cut + tail));
  }
  void SerializeState(snes::core::detail::StateArchive &archive) {
    chip.SerializeState(archive);
    archive.Fixed(std::span<uint8_t>(ram));
  }
};
} // namespace snes::tests
