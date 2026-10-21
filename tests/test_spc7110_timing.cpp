#include "ConsoleConfig.hpp"
#include "snes/core/Cartridge.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace snes::core;
namespace {
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::vector<uint8_t> Image() {
  std::vector<uint8_t> rom(0x300000, 0);
  rom[0x8000] = 0x78;
  rom[0xffc0] = 'T';
  rom[0xffd5] = 0x3a;
  rom[0xffd6] = 0xf5;
  rom[0xffd7] = 12;
  rom[0xffd8] = 3;
  rom[0xffdc] = rom[0xffdd] = 0xff;
  rom[0xfffd] = 0x80;
  return rom;
}
void DecompressionReady() {
  auto cart = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
  Check(cart && cart->Header().chip == EnhancementChip::Spc7110, "Generated SPC7110 image loads");
  cart->Write(0x4806, 0);
  Check(cart->Read(0x480c) == 0, "Starting decompression leaves readiness clear");
  cart->AdvanceHardware(20);
  Check(cart->Read(0x480c) == 0, "The decoder remains pending at its final wait clock");
  cart->AdvanceHardware(1);
  Check(cart->Read(0x480c) == 0x80 && cart->Read(0x80480c) == 0x80,
        "Completed decompression retains readiness across status reads and CPU mirrors");
}
void FreshSaveRam() {
  auto cart = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
  Check(cart && cart->SramData().size() == 8192, "The generated board allocates its battery RAM");
  for (uint8_t byte : cart->SramData())
    Check(byte == 0xff, "A fresh SPC7110 battery RAM allocation contains erased bytes");
}
void HeaderSpeedFlags() {
  for (uint8_t mode : {0x2a, 0x3a}) {
    for (uint8_t type : {0xf5, 0xf9}) {
      auto image = Image();
      image[0xffd5] = mode;
      image[0xffd6] = type;
      auto cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
      Check(cart && cart->Header().chip ==
                        (type == 0xf9 ? EnhancementChip::Spc7110Rtc : EnhancementChip::Spc7110),
            "Slow and fast ROM flags retain SPC7110 and RTC detection");
    }
  }
}
void ReservedMode() {
  auto image = Image();
  image[0x100000] = 3;
  auto cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
  cart->Write(0x4804, 0);
  cart->Write(0x4806, 0);
  cart->AdvanceHardware(100);
  Check(cart->Read(0x480c) == 0 && cart->Read(0x4800) == 0,
        "The reserved mode leaves readiness clear and provides no decoded output");
  Check(cart->Read(0x4809) == 0xff && cart->Read(0x480a) == 0xff,
        "An unready stream still decrements its byte counter");
}

void ReverseDmaParameters() {
  constexpr std::array<uint64_t, 3> hashes{0x90b696d9c4ca6390ULL, 0x9c1e060529813ccbULL,
                                           0x18d48bd29a48b05cULL};
  auto base = Image();
  for (size_t i = 0x100000; i < base.size(); ++i)
    base[i] = uint8_t((i * 37) ^ (i >> 4) ^ 0xa5);
  std::vector<uint8_t> code{0x78};
  const auto write = [&](uint16_t address, uint8_t value) {
    code.insert(code.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  const auto read = [&](uint16_t address, uint16_t target) {
    code.insert(code.end(), {0xad, uint8_t(address), uint8_t(address >> 8), 0x8f, uint8_t(target),
                             uint8_t(target >> 8), 0x7e});
  };
  write(0x2100, 0x80);
  for (const auto [address, value] : {std::pair{0x4801, 0},
                                      {0x4802, 1},
                                      {0x4803, 0},
                                      {0x4804, 0},
                                      {0x4834, 1},
                                      {0x1200, 0},
                                      {0x1201, 0},
                                      {0x1202, 1},
                                      {0x2181, 0},
                                      {0x2182, 0x12},
                                      {0x2183, 0},
                                      {0x4300, 0x90},
                                      {0x4301, 0x80},
                                      {0x4302, 6},
                                      {0x4303, 0x48},
                                      {0x4304, 0},
                                      {0x4305, 3},
                                      {0x4306, 0},
                                      {0x420b, 1}})
    write(uint16_t(address), uint8_t(value));
  read(0x480c, 0x1000);
  for (unsigned i = 0; i < 128; ++i)
    read(0x4800, uint16_t(0x1100 + i));
  read(0x480c, 0x1001);
  read(0x4804, 0x1003);
  write(0x1002, 0xa5);
  code.push_back(0xdb);
  std::copy(code.begin(), code.end(), base.begin() + 0x8000);
  for (bool pal : {false, true})
    for (unsigned initial = 0; initial < 3; ++initial)
      for (unsigned final = 0; final < 3; ++final) {
        auto image = base;
        image[0xffd9] = pal ? 2 : 1;
        const uint8_t table[]{uint8_t(initial), 0, 0x12, 0x34, uint8_t(final), 0, 0x34, 0x56};
        std::copy(std::begin(table), std::end(table), image.begin() + 0x100100);
        auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
        Check(machine->LoadCartridge(image), "The reverse-DMA decoder cartridge loads");
        for (bool warm : {false, true}) {
          if (warm)
            Check(machine->Reset(), "The reverse-DMA decoder cartridge resets");
          machine->StepFrame();
          const auto *ram = machine->GetBus().WramData();
          Check(ram[0x1002] == 0xa5 && ram[0x1003] == 1 && ram[0x1202] == 1,
                "Reverse DMA consumes the prepared byte and changes the table index");
          Check(ram[0x1000] == 0x80 && ram[0x1001] == 0x80,
                "The decoder completes its pending transfer before and after the output reads");
          uint64_t hash = 14695981039346656037ULL;
          for (unsigned i = 0; i < 128; ++i)
            hash = (hash ^ ram[0x1100 + i]) * 1099511628211ULL;
          Check(hash == hashes[final],
                "Pending decompression uses the mode and address changed during startup");
        }
      }
}
} // namespace
int main() {
  try {
    DecompressionReady();
    FreshSaveRam();
    HeaderSpeedFlags();
    ReservedMode();
    ReverseDmaParameters();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("SPC7110 timing tests passed");
}
