#pragma once

#include "snes/core/Cx4Processor.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace snes::tests {
struct Cx4Fixture {
  std::vector<uint8_t> rom = std::vector<uint8_t>(0x100000);
  std::vector<uint8_t> ram = std::vector<uint8_t>(0x2000);
  std::vector<uint8_t> firmware = std::vector<uint8_t>(3072);
  snes::core::Cx4Processor::RetainedStorage storage{};
  Cx4Fixture() {
    const std::array<uint16_t, 16> program{0x6261, 0x602f, 0x1c00, 0x6003, 0x8401, 0xe060,
                                           0x642a, 0xe02f, 0x1c00, 0xc401, 0xf001, 0x2401,
                                           0x6463, 0xe062, 0x9c02, 0xfc00};
    for (unsigned page = 0; page < 2; ++page)
      for (unsigned word = 0; word < 256; ++word)
        storage.program[page][word] = program[word % program.size()];
    for (unsigned offset = 0; offset < rom.size(); ++offset)
      rom[offset] = uint8_t(storage.program[0][(offset / 2) & 255] >> ((offset & 1) * 8));
    for (unsigned offset = 0; offset < ram.size(); ++offset)
      ram[offset] = uint8_t(offset * 31 + 7);
    for (unsigned offset = 0; offset < firmware.size(); ++offset)
      firmware[offset] = uint8_t(offset * 17 + 3);
  }
};
inline void Cx4Address(snes::core::Cx4Processor &chip, uint32_t port, uint32_t value,
                       unsigned bytes = 3) {
  for (unsigned byte = 0; byte < bytes; ++byte)
    chip.Write(port + byte, uint8_t(value >> (byte * 8)));
}
inline std::vector<uint8_t> Cx4TimingTrace(unsigned profile) {
  Cx4Fixture fixture;
  const bool hi = profile & 2;
  snes::core::Cx4Processor chip(fixture.rom, fixture.ram, fixture.firmware,
                                hi ? snes::core::Cx4Processor::Layout::HiRom
                                   : snes::core::Cx4Processor::Layout::LoRom);
  chip.InitializeStorage(fixture.storage);
  chip.SetPal(profile & 1);
  chip.SetDelayedSync(profile & 4);
  const uint32_t ram = hi ? 0x306000 : 0x700000;
  Cx4Address(chip, 0x7f83, ram);
  Cx4Address(chip, 0x7f49, 0x008000);
  chip.Write(0x7f4f, 0);
  std::vector<uint8_t> output;
  uint32_t random = 0x973a15u + profile;
  const auto next = [&] {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  const auto observe = [&] {
    output.push_back(chip.CpuIrqPending());
    for (uint32_t port : {0x7f53u, 0x7f50u, 0x7f48u, 0x7f4fu, 0x7f80u, 0x7f81u, 0x7f82u, 0x7f86u,
                          0x7f87u, 0x7f88u, 0x6000u, 0x6001u, 0x6011u})
      output.push_back(chip.Read(port, 0xa5));
    output.insert(output.end(), fixture.ram.begin(), fixture.ram.begin() + 32);
  };
  for (unsigned step = 0; step < 40000; ++step) {
    const auto choice = next();
    chip.Advance(choice & 31, choice & 32);
    if (step % 37 == 0)
      chip.Write(0x7f50, uint8_t(choice >> 8));
    if (step % 101 == 0)
      chip.Write(0x7f58 + (choice % 4), 0);
    if (step % 107 == 0)
      chip.Write(0x7f5d, 0);
    if (step % 131 == 0)
      chip.Write(0x7f81, uint8_t(choice >> 16));
    if (step % 331 == 0) {
      chip.Write(0x7f48, uint8_t(choice));
      chip.Write(0x7f4f, uint8_t(choice >> 8));
    }
    if (step % 419 == 0) {
      Cx4Address(chip, 0x7f40, 0x008000 + (choice & 127));
      Cx4Address(chip, 0x7f43, 7 + (choice & 63), 2);
      Cx4Address(chip, 0x7f45, ram + (choice & 127));
    }
    if (step % 997 == 0)
      chip.Write(0x7f51, uint8_t(choice & 1));
    if (step % 10000 == 9999) {
      chip.Reset();
      Cx4Address(chip, 0x7f49, 0x008000);
      Cx4Address(chip, 0x7f83, ram);
      chip.Write(0x7f4f, 0);
    }
    if (step % 11 == 0)
      observe();
  }
  chip.Advance(0, true);
  observe();
  return output;
}
inline std::vector<uint8_t> Cx4BoundaryRom(bool skip) {
  std::vector<uint8_t> rom(0x100000);
  const auto word = [&](unsigned index, uint16_t value) {
    rom[index * 2] = uint8_t(value);
    rom[index * 2 + 1] = uint8_t(value >> 8);
  };
  word(253, 0x7c01);
  word(254, skip ? 0x2400 : 0x6461);
  word(255, 0x642a);
  word(256, 0xe060);
  word(257, 0x7c00);
  word(258, 0x0800);
  return rom;
}
inline void Cx4BoundarySetup(snes::core::Cx4Processor &chip) {
  Cx4Address(chip, 0x7f49, 0xc00000);
  chip.Write(0x7f48, 0);
  chip.Advance(3000, true);
  chip.Write(0x7f4f, 253);
}
inline std::vector<uint8_t> Cx4BoundaryTrace(unsigned profile) {
  Cx4Fixture fixture;
  fixture.rom = Cx4BoundaryRom(profile & 8);
  snes::core::Cx4Processor chip(fixture.rom, fixture.ram, fixture.firmware,
                                profile & 2 ? snes::core::Cx4Processor::Layout::HiRom
                                            : snes::core::Cx4Processor::Layout::LoRom);
  chip.InitializeStorage(fixture.storage);
  chip.SetPal(profile & 1);
  chip.SetDelayedSync(profile & 4);
  Cx4BoundarySetup(chip);
  std::vector<uint8_t> output;
  for (unsigned step = 0; step < 3000; ++step) {
    chip.Advance(1, true);
    if ((profile & 16) && step == 500)
      chip.Write(0x7f48, 1);
    for (uint32_t port : {0x7f53u, 0x7f48u, 0x7f80u, 0x7f81u, 0x7f82u})
      output.push_back(chip.Read(port, 0xa5));
    output.push_back(chip.CpuIrqPending());
  }
  return output;
}
inline void Cx4ZeroWaitSetup(snes::core::Cx4Processor &chip) {
  for (unsigned index = 0; index < 512; ++index)
    chip.Write(0x6000 + index, 0);
  for (unsigned index = 0; auto word : {0x642a, 0xe060, 0xfc00}) {
    chip.Write(0x6000 + index++, uint8_t(word));
    chip.Write(0x6000 + index++, uint8_t(word >> 8));
  }
  Cx4Address(chip, 0x7f49, 0x006000);
  chip.Write(0x7f50, 0);
  chip.Write(0x7f48, 0);
  chip.Write(0x7f4f, 0);
}
inline std::vector<uint8_t> Cx4ZeroWaitTrace(unsigned profile) {
  Cx4Fixture fixture;
  snes::core::Cx4Processor chip(fixture.rom, fixture.ram, fixture.firmware,
                                profile & 2 ? snes::core::Cx4Processor::Layout::HiRom
                                            : snes::core::Cx4Processor::Layout::LoRom);
  chip.InitializeStorage(fixture.storage);
  chip.SetPal(profile & 1);
  chip.SetDelayedSync(profile & 4);
  Cx4ZeroWaitSetup(chip);
  std::vector<uint8_t> output;
  for (unsigned step = 0; step < 48; ++step) {
    chip.Advance(step % 7, true);
    output.push_back(chip.Read(0x7f53, 0xa5));
    output.push_back(chip.Read(0x7f80, 0xa5));
    output.push_back(chip.CpuIrqPending());
  }
  return output;
}
} // namespace snes::tests
