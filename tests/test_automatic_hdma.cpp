#include "ConsoleConfig.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace snes::core;

namespace {
enum class Probe { FieldEarly, FieldVblank, FieldOff, Repeated, Wrapped, Reverse, ReverseVblank };

std::vector<uint8_t> Program(Probe probe, bool pal) {
  std::vector<uint8_t> code;
  auto emit = [&](std::initializer_list<uint8_t> bytes) {
    code.insert(code.end(), bytes.begin(), bytes.end());
  };
  auto store = [&](uint16_t address, uint8_t data) {
    emit({0xa9, data, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  auto storeLong = [&](uint32_t address, uint8_t data) {
    emit({0xa9, data, 0x8f, uint8_t(address), uint8_t(address >> 8), uint8_t(address >> 16)});
  };
  auto waitVblank = [&](uint8_t branch) { emit({0xad, 0x12, 0x42, branch, 0xfb}); };
  auto waitHblank = [&](uint8_t branch) { emit({0x2c, 0x12, 0x42, branch, 0xfb}); };
  const bool reverse = probe == Probe::Reverse || probe == Probe::ReverseVblank;
  const bool vblank = probe == Probe::FieldVblank || probe == Probe::ReverseVblank;
  const bool field = probe <= Probe::FieldOff;

  // Initialize every descriptor byte before enabling automatic transfers. The
  // CPU stores are part of the timing probe, independent of power-on WRAM data.
  emit({0xa2, 0, 0x74, 0, 0xe8, 0xe0, 0x80, 0xd0, 0xf9});
  store(0x4300, reverse ? 0x80 : 0);
  store(0x4304, reverse ? 0x7e : 0x80);
  if (probe == Probe::Repeated) {
    store(0x430a, 0x82);
    store(0x4308, 0);
    store(0x4309, 0);
  }
  if (reverse) {
    store(0x4301, 0x37); // Reverse transfers read SLHV and latch the beam.
    store(0x4302, 0);
    store(0x4303, 0x20);
    storeLong(0x7e2000, 0x81);
    storeLong(0x7e2001, 0);
    storeLong(0x7e2002, 0);
  }
  if (vblank)
    waitVblank(0x10);
  store(0x420c, probe == Probe::FieldOff ? 0 : 1);
  if (field || vblank) {
    if (!vblank)
      waitVblank(0x10);
    waitVblank(0x30);
  }
  if (!field) {
    emit({0xa9, 0x40});
    waitHblank(0xf0);
    const unsigned lines = probe == Probe::Wrapped ? 128 : probe == Probe::Repeated ? 2 : 1;
    for (unsigned line = 1; line < lines; ++line) {
      waitHblank(0xd0);
      waitHblank(0xf0);
    }
    if (reverse && !vblank) {
      waitHblank(0xd0);
      waitHblank(0xf0);
    }
    emit({0xea, 0xea});
  }
  store(0x420c, 0);
  if (!reverse)
    emit({0xad, 0x37, 0x21});

  constexpr std::array<uint16_t, 12> registers{0x213c, 0x213c, 0x213d, 0x213d, 0x4308, 0x4309,
                                               0x430a, 0x4305, 0x4306, 0x4300, 0x4301, 0x4304};
  for (unsigned index = 0; index < registers.size(); ++index) {
    emit({0xad, uint8_t(registers[index]), uint8_t(registers[index] >> 8)});
    if (index == 1 || index == 3)
      emit({0x29, 1});
    emit({0x8f, uint8_t(index), 1, 0x7e, 0x8f, uint8_t(index), 1, 0x70});
  }
  for (uint8_t index = 12; index < 14; ++index)
    emit({0xaf, 0x7e, 0, uint8_t(index == 12 ? 0x80 : 0x7e), 0x8f, index, 1, 0x7e, 0x8f, index, 1,
          0x70});
  storeLong(0x7e010e, 0);
  emit({0x8f, 14, 1, 0x70});
  storeLong(0x7e010f, 0xa5);
  emit({0x8f, 15, 1, 0x70, 0xdb});

  std::vector<uint8_t> rom(0x8000);
  std::copy(code.begin(), code.end(), rom.begin());
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

void AutomaticScheduling() {
  struct Case {
    Probe probe;
    uint8_t hNtsc, hPal, hHigh, v, a2Low, a2High, counter;
  };
  constexpr std::array cases{
      Case{Probe::FieldEarly, 0x24, 0x21, 0, 0, 0, 0, 0},
      Case{Probe::FieldVblank, 0x26, 0x27, 0, 0, 0, 0, 0},
      Case{Probe::FieldOff, 0x20, 0x1f, 0, 0, 0xff, 0xff, 0xff},
      Case{Probe::Repeated, 0x3e, 0x3e, 1, 9, 2, 0, 0},
      Case{Probe::Wrapped, 0x32, 0x32, 1, 0x87, 0x7e, 0, 0},
      Case{Probe::Reverse, 0x1c, 0x1c, 1, 9, 0, 0, 0xfd},
      Case{Probe::ReverseVblank, 0x1b, 0x1b, 1, 0, 3, 0x20, 0},
  };
  for (bool pal : {false, true}) {
    for (const auto &test : cases) {
      for (bool clearRam : {false, true}) {
        auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
        std::string error;
        if (!machine->LoadCartridge(Program(test.probe, pal), &error))
          throw std::runtime_error(error);
        if (clearRam)
          std::fill_n(machine->GetBus().WramData(), MemoryBus::WramSize, 0);
        for (unsigned frame = 0; frame < 3; ++frame)
          machine->StepFrame();
        const bool reverse = test.probe == Probe::Reverse || test.probe == Probe::ReverseVblank;
        const std::array<uint8_t, 16> expected{pal ? test.hPal : test.hNtsc,
                                               test.hHigh,
                                               test.v,
                                               0,
                                               test.a2Low,
                                               test.a2High,
                                               test.counter,
                                               0xff,
                                               0xff,
                                               uint8_t(reverse ? 0x80 : 0),
                                               uint8_t(reverse ? 0x37 : 0xff),
                                               uint8_t(reverse ? 0x7e : 0x80),
                                               0,
                                               0,
                                               0,
                                               0xa5};
        if (!std::equal(expected.begin(), expected.end(), machine->GetBus().WramData() + 0x100))
          throw std::runtime_error(
              "Automatic HDMA preserves the initialized table, beam, and channel trace");
      }
    }
  }
}
} // namespace

int main() {
  try {
    AutomaticScheduling();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("Automatic HDMA setup, late enable, repeated lines, wrapping, and beam latches passed");
}
