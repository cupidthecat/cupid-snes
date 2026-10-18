#include "snes/core/Emulator.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>

using namespace snes::core;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<uint8_t> Bios(bool secondModel, bool pal = false, uint8_t control = 0x81) {
  std::vector<uint8_t> bytes(0x80000);
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = uint8_t(i >> 15);
  std::fill_n(bytes.begin() + 0x7fb0, 80, 0);
  std::fill_n(bytes.begin() + 0x7fc0, 21, ' ');
  const std::string_view title = secondModel ? "Super GAMEBOY2" : "Super GAMEBOY";
  std::copy(title.begin(), title.end(), bytes.begin() + 0x7fc0);
  bytes[0x7fd5] = secondModel ? 0x30 : 0x20;
  bytes[0x7fd6] = secondModel ? 0 : 0xe3;
  if (secondModel) {
    bytes[0x7fda] = 0x33;
    std::copy_n("042J", 4, bytes.begin() + 0x7fb2);
  }
  bytes[0x7fd7] = 9;
  bytes[0x7fd9] = pal ? 2 : 0;
  bytes[0x7fdc] = bytes[0x7fdd] = 0xff;
  bytes[0x7ffd] = 0x80;
  const std::array<uint8_t, 7> program{0x78, 0xa9, control, 0x8d, 0x03, 0x60, 0xdb};
  std::copy(program.begin(), program.end(), bytes.begin());
  return bytes;
}

std::array<uint8_t, 256> Boot() {
  std::array<uint8_t, 256> bytes{};
  bytes[0] = 0xc3;
  bytes[1] = 0xfc;
  bytes[0xfc] = 0x3e;
  bytes[0xfd] = 1;
  bytes[0xfe] = 0xe0;
  bytes[0xff] = 0x50;
  return bytes;
}

std::vector<uint8_t> Game(bool sound = false) {
  std::vector<uint8_t> bytes(32768);
  bytes[0x147] = 3;
  bytes[0x149] = 2;
  std::vector<uint8_t> program{0x3e, 0x0a, 0xea, 0, 0};
  if (sound)
    for (const auto [address, value] : std::array<std::array<uint8_t, 2>, 7>{{{0x26, 0x80},
                                                                              {0x24, 0x77},
                                                                              {0x25, 0xff},
                                                                              {0x11, 0x40},
                                                                              {0x12, 0xf3},
                                                                              {0x13, 0x87},
                                                                              {0x14, 0x83}}})
      program.insert(program.end(), {0x3e, value, 0xe0, address});
  program.insert(program.end(), {0x21, 0, 0xa0, 0x34, 0x18, 0xfd});
  std::copy(program.begin(), program.end(), bytes.begin() + 0x100);
  return bytes;
}

void BoardMapping() {
  for (bool secondModel : {false, true}) {
    const auto bios = Bios(secondModel);
    auto cartridge = Cartridge::FromRomImage(bios, nullptr, nullptr, nullptr);
    Check(cartridge && cartridge->BoardConnected() &&
              cartridge->Header().chip == EnhancementChip::SuperGameBoy &&
              cartridge->SramData().empty(),
          "Both adapter BIOS headers select their cartridge device");
    auto bus = std::make_unique<MemoryBus>();
    bus->MapCartridge(*cartridge);
    for (unsigned bank = 0; bank < 256; ++bank) {
      if (bank == 0x7e || bank == 0x7f)
        continue;
      for (unsigned offset :
           {0x6000u, 0x600fu, 0x67ffu, 0x6800u, 0x7000u, 0x7800u, 0x7fffu, 0x8123u}) {
        const auto address = (bank << 16) | offset;
        uint8_t expected = 0xa5;
        if ((bank & 0x40) || offset >= 0x8000)
          expected = bios[((bank << 15) | (offset & 0x7fff)) & (bios.size() - 1)];
        else if (offset < 0x6800 || offset >= 0x7000)
          expected = offset == 0x600f ? 0x21 : offset == 0x7800 ? 0xff : 0;
        bus->SetOpenBus(0xa5);
        Check(bus->Read(address) == expected,
              "Adapter ports, ROM mirrors and open-bus gaps retain their bank selection");
      }
    }
  }
  auto bytes = Bios(false);
  bytes[0x7fd6] = 0;
  auto ordinary = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
  Check(ordinary && ordinary->Header().chip == EnhancementChip::None &&
            ordinary->Read(0x600f, 0xa5) == 0xa5,
        "An adapter title alone does not replace ordinary cartridge wiring");
  bytes[0x7fd6] = 0xe3;
  std::fill_n(bytes.begin() + 0x7fc0, 21, ' ');
  auto generic = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
  Check(generic && generic->Header().chip == EnhancementChip::SuperGameBoy &&
            generic->Read(0x600f) == 0x21,
        "The cartridge type selects adapter wiring without a recognized title");
}

void ClockAndBattery() {
  for (bool secondModel : {false, true})
    for (bool pal : {false, true})
      for (uint8_t divider = 0; divider < 4; ++divider) {
        auto whole = Cartridge::FromSuperGameBoy(Bios(secondModel), Game(), Boot());
        auto split = Cartridge::FromSuperGameBoy(Bios(secondModel), Game(), Boot());
        Check(whole && split, "Generated slot and boot images load for both adapter models");
        const std::vector<uint8_t> saved(8192, 0);
        Check(whole->LoadSlotBattery(0, saved) && split->LoadSlotBattery(0, saved),
              "The inserted cartridge accepts its own battery data");
        whole->SetPal(pal);
        split->SetPal(pal);
        whole->AdvanceHardware(10000);
        Check(whole->SaveSlotBattery(0) == saved, "A held handheld leaves battery RAM unchanged");
        whole->Write(0x006003, uint8_t(0x80 | divider));
        split->Write(0x806003, uint8_t(0x80 | divider));
        std::vector<std::array<int16_t, 2>> a, b;
        constexpr std::array<unsigned, 4> divisors{4, 5, 7, 9};
        const auto master = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
        const auto rate = (secondModel ? 20'971'520 : master) / divisors[divider] / 128;
        const auto sink = [rate](auto &samples) {
          return [&samples, rate](int16_t left, int16_t right, uint32_t sampleRate) {
            Check(sampleRate == rate, "Audio samples retain the selected physical oscillator rate");
            samples.push_back({left, right});
          };
        };
        whole->SetGameBoyAudioOutput(sink(a));
        split->SetGameBoyAudioOutput(sink(b));
        whole->AdvanceHardware(12345);
        unsigned elapsed = 0;
        for (unsigned i = 0; elapsed < 12345; ++i) {
          const auto clocks = std::min(12345 - elapsed, (i * 17) % 257 + 1);
          split->AdvanceHardware(clocks);
          elapsed += clocks;
        }
        auto battery = whole->SaveSlotBattery(0);
        Check(battery.size() == 8192 && battery[0] != 0 && battery == split->SaveSlotBattery(0) &&
                  !a.empty() && a == b,
              "Master-clock batching preserves handheld execution, battery bytes and sound");
        whole->Write(0x6003, divider);
        whole->AdvanceHardware(10000);
        Check(whole->SaveSlotBattery(0) == battery,
              "Clearing run control holds processor execution");
        whole->Write(0x6003, uint8_t(0x80 | divider));
        Check(whole->SaveSlotBattery(0) == battery,
              "A control-port reset retains the inserted cartridge battery");
        whole->Reset();
        Check(whole->SaveSlotBattery(0) == battery && whole->Read(0x6000) == 0,
              "Console reset retains the battery and resets the adapter link");
        Check(!whole->LoadSlotBattery(1, saved) && whole->SaveSlotBattery(1).empty(),
              "An absent slot cannot overwrite the inserted cartridge battery");
      }
}

void ConsoleExecutionAndFailedLoads() {
  for (bool secondModel : {false, true}) {
    auto emulator = std::make_unique<Emulator>();
    const auto bios = Bios(secondModel);
    const auto game = Game();
    const auto boot = Boot();
    Check(emulator->LoadSuperGameBoy(bios, game, boot),
          "The console accepts a complete adapter set");
    Check(emulator->LoadSlotBattery(0, std::vector<uint8_t>(8192, 0)),
          "A slot save loads before boot");
    emulator->StepFrame();
    const auto battery = emulator->SaveSlotBattery(0);
    Check(battery.size() == 8192 && battery[0] != 0 && emulator->GetBus().Read(0x80600f) == 0x21,
          "SNES control-port writes start the handheld on the console clock bus");
    emulator->GetBus().Write(0x7e1234, 0x5a);
    const auto frame = emulator->CurrentFrame();
    std::string error;
    Check(!emulator->LoadSuperGameBoy(bios, game, std::span<const uint8_t>(boot).first(255),
                                      &error) &&
              !error.empty(),
          "An incomplete handheld boot image is rejected");
    auto ordinary = bios;
    std::fill_n(ordinary.begin() + 0x7fc0, 21, ' ');
    ordinary[0x7fd6] = ordinary[0x7fda] = 0;
    Check(!emulator->LoadSuperGameBoy(ordinary, game, boot, &error) && !error.empty(),
          "An ordinary cartridge cannot replace the adapter BIOS");
    Check(emulator->CurrentFrame() == frame && emulator->GetBus().Read(0x7e1234) == 0x5a &&
              emulator->SaveSlotBattery(0) == battery,
          "Failed adapter loads preserve the running console, RAM and slot battery");
  }
}

void ConsoleClockBoundaries() {
  constexpr std::array<std::array<uint8_t, 3>, 16> expected{{{114, 252, 135},
                                                             {243, 149, 55},
                                                             {25, 104, 183},
                                                             {131, 250, 112},
                                                             {114, 195, 20},
                                                             {243, 205, 168},
                                                             {25, 254, 227},
                                                             {131, 54, 232},
                                                             {39, 90, 140},
                                                             {183, 19, 111},
                                                             {238, 11, 40},
                                                             {98, 177, 1},
                                                             {67, 84, 100},
                                                             {206, 117, 27},
                                                             {254, 191, 127},
                                                             {111, 4, 154}}};
  unsigned fixture = 0;
  for (bool secondModel : {false, true})
    for (bool pal : {false, true})
      for (uint8_t divider = 0; divider < 4; ++divider, ++fixture) {
        auto emulator = std::make_unique<Emulator>();
        Check(emulator->LoadSuperGameBoy(Bios(secondModel, pal, uint8_t(0x80 | divider)),
                                         Game(true), Boot()),
              "A generated console program selects the adapter model, region and oscillator");
        std::vector<uint8_t> battery(8192, 0);
        Check(emulator->LoadSlotBattery(0, battery), "The timing fixture starts with a known save");
        for (unsigned frame = 0; frame < 3; ++frame) {
          emulator->StepFrame();
          battery[0] = expected[fixture][frame];
          Check(emulator->SaveSlotBattery(0) == battery,
                "Console frame pauses retain the handheld's pending CPU bus interval");
        }
      }
}
} // namespace

int main() {
  try {
    BoardMapping();
    ClockAndBattery();
    ConsoleExecutionAndFailedLoads();
    ConsoleClockBoundaries();
    std::puts("Super Game Boy cartridge mapping, execution, clocks and battery checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
