#include "snes/core/gameboy/Cartridge.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

using snes::core::gameboy::Cartridge;
namespace {
constexpr uint64_t Now = 1789389296;
using Clock = std::array<uint8_t, 4>;
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::vector<uint8_t> Image(unsigned banks = 2, uint8_t ram = 0, uint8_t features = 15) {
  std::vector<uint8_t> rom(banks * 0x4000);
  for (unsigned bank = 0; bank < banks; ++bank) {
    std::fill_n(rom.begin() + bank * 0x4000, 0x4000, uint8_t(bank));
    rom[bank * 0x4000 + 1] = uint8_t(bank >> 8);
  }
  rom[0x147] = 0xbc;
  rom[0x149] = 0xc1;
  rom[0x14a] = 0x65;
  rom[0x152] = ram;
  rom[0x153] = features;
  return rom;
}
void Load(Cartridge &cartridge, unsigned banks = 2, uint8_t ram = 0, uint8_t features = 15) {
  cartridge.SetTimeSource([] { return Now; });
  Check(cartridge.Load(Image(banks, ram, features)), "Load the generated TPP1 cartridge");
}
void Program(Cartridge &cartridge, Clock bytes) {
  cartridge.Write(3, 5);
  for (unsigned i = 0; i < bytes.size(); ++i)
    cartridge.Write(uint16_t(0xa000 + i), bytes[i]);
  cartridge.Write(3, 0x11);
}
Clock ReadLatch(Cartridge &cartridge) {
  cartridge.Write(3, 5);
  Clock bytes{};
  for (unsigned i = 0; i < bytes.size(); ++i)
    bytes[i] = cartridge.Read(uint16_t(0xa000 + i)).value();
  return bytes;
}
uint8_t Flags(Cartridge &cartridge) {
  cartridge.Write(3, 0);
  return cartridge.Read(0xa003).value();
}
std::vector<uint8_t> Battery(uint64_t timestamp, Clock bytes, uint8_t flags = 15) {
  std::vector<uint8_t> payload = {'T', 'P', 'P', '1', 1, 0, flags, 0};
  for (unsigned i = 0; i < 8; ++i)
    payload.push_back(uint8_t(timestamp >> (i * 8)));
  payload.insert(payload.end(), bytes.begin(), bytes.end());
  return payload;
}

void HeaderAndBanking() {
  for (unsigned code : {0u, 1u, 3u, 9u, 10u, 255u}) {
    Cartridge cartridge;
    Load(cartridge, 2, uint8_t(code));
    const size_t size = code >= 1 && code <= 9 ? size_t(8192) << (code - 1) : 0;
    Check(cartridge.Ram().size() == size, "The extended header selects complete RAM banks");
  }
  for (unsigned address : {0x147u, 0x149u, 0x14au}) {
    auto rom = Image();
    rom[address] ^= 1;
    Cartridge cartridge;
    Check(cartridge.Load(rom) && !cartridge.Read(0xa000),
          "All three header signature bytes are required for TPP1 control ports");
  }
  Cartridge cartridge;
  Load(cartridge, 512, 9);
  Check(cartridge.Read(0x4000) == 1 && cartridge.Read(0xa000) == 0,
        "The reset ROM mapping retains bank one while register readback starts at zero");
  cartridge.Write(0x7fff, 0xff);
  Check(cartridge.Read(0x4000) == 0, "The first mapper write commits the pending bank-zero latch");
  cartridge.Write(0x7000, 0x81);
  cartridge.Write(0x1001, 1);
  Check(cartridge.Read(0x4000) == 0x81 && cartridge.Read(0x4001) == 1 &&
            cartridge.Read(0xa001) == 1,
        "Mirrored mapper addresses preserve both bytes of the ROM selector");
  cartridge.Write(0x6002, 255);
  cartridge.Write(0x4003, 3);
  cartridge.Write(0xa123, 0x51);
  cartridge.Write(2, 0);
  Check(cartridge.Read(0xa123) == 0xff, "RAM exposes eight bank-selection bits");
  cartridge.Write(2, 255);
  cartridge.Write(3, 2);
  cartridge.Write(0xa123, 0x99);
  Check(cartridge.Read(0xa123) == 0x51, "Mode two leaves RAM readable but ignores writes");
  cartridge.Write(3, 4);
  Check(cartridge.Read(0xa123) == 0x51, "Unsupported commands retain the current access mode");
  Check(cartridge.SaveBattery()[255 * 8192 + 0x123] == 0x51,
        "Battery files retain the selected RAM bank contents");
  cartridge.Reset();
  cartridge.Write(2, 255);
  cartridge.Write(3, 2);
  Check(cartridge.Read(0xa123) == 0x51, "Processor reset retains cartridge RAM");
  for (unsigned strength = 0; strength < 4; ++strength) {
    cartridge.Write(3, uint8_t(0x20 + strength));
    Check(cartridge.RumbleStrength() == strength && cartridge.Rumble() == (strength != 0),
          "All four rumble levels remain available to the cartridge host");
  }
  Cartridge volatileCartridge;
  Load(volatileCartridge, 2, 1, 0);
  volatileCartridge.Write(3, 0x23);
  Check(volatileCartridge.SaveBattery().empty() && volatileCartridge.RumbleStrength() == 3,
        "The battery capability gates saves independently of control commands");
  volatileCartridge.Write(3, 3);
  volatileCartridge.Write(0xa123, 0x6b);
  volatileCartridge.Reset();
  volatileCartridge.Write(3, 2);
  Check(volatileCartridge.Read(0xa123) == 0x6b && volatileCartridge.SaveBattery().empty(),
        "TPP1 reset retains RAM independently of the save-file capability");
}

void ClockAndBattery() {
  Cartridge cartridge;
  Load(cartridge);
  Program(cartridge, {255, 0xd7, 59, 59});
  cartridge.Write(3, 0x19);
  cartridge.SynchronizeRtc(Now + 1);
  cartridge.Write(3, 0x10);
  Check(ReadLatch(cartridge) == Clock{0, 0, 0, 0} && Flags(cartridge) == 12,
        "Second, minute, hour, weekday and week rollover set the sticky overflow flag");
  cartridge.Write(3, 0x14);
  cartridge.Write(3, 0x18);
  cartridge.SynchronizeRtc(Now + 100);
  cartridge.Write(3, 0x10);
  Check(ReadLatch(cartridge) == Clock{0, 0, 0, 0} && Flags(cartridge) == 0,
        "Stopped clock synchronization preserves the clock and timestamp");
  cartridge.Write(3, 0x19);
  cartridge.SynchronizeRtc(Now + 100);
  cartridge.Write(3, 0x10);
  Check(ReadLatch(cartridge) == Clock{0, 0, 1, 39},
        "Restarting the clock accounts for elapsed time since its last running update");
  Check(cartridge.SaveBattery() == Battery(Now, {0, 0, 1, 39}, 4),
        "The clock trailer uses live host time and explicit byte order");
  Program(cartridge, {17, 0x77, 59, 58});
  cartridge.Write(3, 0x22);
  const auto payload = Battery(Now, {201, 0xe7, 62, 255}, 15);
  cartridge.LoadBattery(payload);
  Check(ReadLatch(cartridge) == Clock{17, 0x77, 59, 58} && Flags(cartridge) == 6,
        "Battery load retains the held clock, running flag, and rumble level");
  cartridge.Reset();
  Check(ReadLatch(cartridge) == Clock{17, 0x77, 59, 58} && Flags(cartridge) == 4,
        "Reset retains clock latches and flags while clearing rumble");
  cartridge.Write(3, 0x10);
  Check(ReadLatch(cartridge) == Clock{201, 0xe7, 62, 255},
        "A latch command exposes the independently restored live clock");
  cartridge.LoadBattery(std::array<uint8_t, 19>{});
  Check(cartridge.SaveBattery() == Battery(Now, {201, 0xe7, 62, 255}, 4),
        "An incomplete trailer preserves the four visible clock bytes");
  cartridge.LoadBattery(Battery(Now + 3600, {7, 0x38, 9, 10}));
  cartridge.SynchronizeRtc(Now + 1);
  cartridge.Write(3, 0x10);
  Check(ReadLatch(cartridge) == Clock{7, 0x38, 9, 11},
        "A future timestamp restarts the clock epoch without discarding loaded fields");
}

void ClockAccessAndDivider() {
  for (bool legacy : {false, true}) {
    Cartridge cartridge;
    Load(cartridge);
    unsigned polls = 0;
    cartridge.SetTimeSource([&] {
      ++polls;
      return Now;
    });
    cartridge.SetClockRate(4295454);
    cartridge.AdvanceRtc(100000);
    if (legacy) {
      cartridge.Write(1, 8);
      cartridge.Write(2, 0);
      cartridge.Write(3, 3);
      cartridge.Write(0xa000, 17);
    } else {
      cartridge.Write(3, 5);
      cartridge.Write(0xa003, 17);
    }
    cartridge.AdvanceRtc(legacy ? 134232 : 34232);
    Check(polls == 0, "Only a direct seconds write resets the RTC polling phase");
    cartridge.AdvanceRtc(1);
    Check(polls == 1, "The first model retains its fractional host polling interval");
  }
  Cartridge cartridge;
  Load(cartridge);
  Program(cartridge, {0xf3, 0xff, 0x83, 0xe2});
  cartridge.Write(1, 8);
  cartridge.Write(2, 0);
  cartridge.Write(3, 3);
  Check(cartridge.Read(0xa000) == 0x22,
        "ROM bit eleven selects clock access even when the cartridge has no RAM");
  Check(ReadLatch(cartridge) == Clock{0xf3, 0x1f, 3, 0x22},
        "Legacy reads mask all held clock fields, including packed weekday bits");
  cartridge.Write(2, 5);
  cartridge.Write(3, 2);
  Check(!cartridge.Read(0xa000), "Unconnected legacy clock selectors leave the bus undriven");
}

void ClockRolloverCorpus() {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned weeks : {0u, 255u})
    for (unsigned weekday : {0u, 6u, 7u})
      for (unsigned hour : {0u, 23u, 24u, 31u})
        for (unsigned minute : {0u, 59u, 60u, 255u})
          for (unsigned second : {0u, 59u, 60u, 255u})
            for (unsigned delta :
                 {0u, 1u, 59u, 60u, 3599u, 3600u, 86399u, 86400u, 86401u, 604800u, 154828800u}) {
              Cartridge cartridge;
              Load(cartridge);
              Program(cartridge, {uint8_t(weeks), uint8_t((weekday << 5) | hour), uint8_t(minute),
                                  uint8_t(second)});
              cartridge.Write(3, 0x19);
              cartridge.SynchronizeRtc(Now + delta);
              cartridge.Write(3, 0x10);
              for (auto value : ReadLatch(cartridge))
                hash = (hash ^ value) * 1099511628211ull;
              hash = (hash ^ Flags(cartridge)) * 1099511628211ull;
            }
  Check(hash == 0xf664ba0fa3cccb15ull,
        "All 4224 valid and invalid clock rollover fixtures retain their output bytes");
}
} // namespace

int main() {
  try {
    HeaderAndBanking();
    ClockAndBattery();
    ClockAccessAndDivider();
    ClockRolloverCorpus();
    std::puts("Handheld TPP1 banking, clock, rumble and battery checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
