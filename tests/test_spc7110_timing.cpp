#include "ConsoleConfig.hpp"
#include "cartridge/Spc7110DmaFixture.hpp"
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

void InterleavedDmaAndHdma() {
  constexpr std::array<uint64_t, 48> expected{
      0xef29b4c383e56d78ULL, 0x657d2c99b2f7820bULL, 0x99e6c0a583caa8e3ULL, 0xd1c563928d8e3cf7ULL,
      0x9a351069f1832a5eULL, 0x37e7ba8ad7615ca1ULL, 0xfd57c16b44fc603dULL, 0x4227ee4f6b57d501ULL,
      0xf1e7b4b4d5691172ULL, 0xbc87ec7a0d8bb513ULL, 0xb9017e20a8cd2e73ULL, 0xbbc6ce3fe6b7511fULL,
      0xfda8e63f5e39fabbULL, 0x2340d23da7252aacULL, 0x6f7ef6091d971cddULL, 0x4d3225c2c5595a3fULL,
      0x89e4d87d000395f4ULL, 0xd46be306df99be5bULL, 0xd9ac2d579823cf26ULL, 0xe41ae87a41af79c4ULL,
      0x35521fdafe24c311ULL, 0xa75270bc45728052ULL, 0x45d2e5c60e7cdb5bULL, 0x6cb77f17cbd57a25ULL,
      0xd223cfeeab6298f1ULL, 0x9b0e6052ad9f3cdaULL, 0xc6dd8735a5d96c2bULL, 0x9647a9f0933caf85ULL,
      0xa5056c827708d9c2ULL, 0x570e6f0c39972d31ULL, 0x35c33b91fbc44b94ULL, 0x2623602a2f2df532ULL,
      0xa35e172f5c8c9f87ULL, 0x7fe82a7b69e741d8ULL, 0x91068e7127f392f9ULL, 0xa487e11b47c4c85bULL,
      0xa6d312d03408f141ULL, 0xa6d312d03408f141ULL, 0x4f4dbea0427bbbd7ULL, 0x4f4dbea0427bbbd7ULL,
      0x6a52a4b40cc80a19ULL, 0x6a52a4b40cc80a19ULL, 0x42376c600ae35503ULL, 0x42376c600ae35503ULL,
      0xeb32c0a76f3581a1ULL, 0xeb32c0a76f3581a1ULL, 0x76ab9ac7b1aea7e7ULL, 0x76ab9ac7b1aea7e7ULL};
  for (unsigned index = 0; index < expected.size(); ++index) {
    const auto transfer = static_cast<spc7110_test::Transfer>(index / 12);
    const unsigned mode = (index / 4) % 3;
    const unsigned alignment = (index / 2) % 2 ? 18 : 0;
    const bool pal = index & 1;
    const auto program = spc7110_test::DmaTimeProgram(transfer, mode, alignment, pal);
    Emulator::Config config;
    config.entropyMode = EntropyMode::None;
    config.entropySeed = 0;
    auto machine = std::make_unique<Emulator>(config);
    Check(machine->LoadCartridge(program.image), "The interleaved cartridge DMA guest loads");
    Check(machine->LoadedCartridge()->Header().chip == EnhancementChip::Spc7110,
          "The guest selects the clocked cartridge processor");
    for (bool warm : {false, true}) {
      if (warm)
        Check(machine->Reset(), "The interleaved cartridge DMA guest resets");
      for (unsigned field = 0; field < 8; ++field)
        machine->StepFrame();
      const auto *ram = machine->GetBus().WramData();
      Check(ram[0x120] == 0xa5, "The interleaved cartridge DMA guest completes");
      const auto *control = ram + 0x10000 + program.controlOffset;
      Check(control[0] == 0x62 && control[1] == 0xe0 && control[2] == 0,
            "HDMA consumes every descriptor and the indirect byte after termination");
      if (transfer != spc7110_test::Transfer::Hdma) {
        const uint16_t cursor = transfer == spc7110_test::Transfer::Reverse ? 0x4830 : 0x4800;
        Check(control[3] == uint8_t(cursor) && control[4] == uint8_t(cursor >> 8) &&
                  control[5] == 0 && control[6] == 0 && control[7] == 0,
              "DMA retains the selected address direction and consumes its entire byte count");
      }
      uint64_t digest = 0xcbf29ce484222325;
      for (unsigned offset = 0; offset < 0x20000; ++offset)
        digest = (digest ^ ram[offset]) * 0x100000001b3;
      if (digest != expected[index]) {
        std::fprintf(stderr, "DMA guest %u (%s) work RAM: %016llx\n", index, warm ? "warm" : "cold",
                     static_cast<unsigned long long>(digest));
        throw std::runtime_error("Interleaved cartridge operations retain complete guest work RAM");
      }
    }
  }
}

void ArithmeticDmaEdges() {
  constexpr std::array<uint64_t, 48> expected{
      0x95ca40db136b0f2aULL, 0x95ca40db136b0f2aULL, 0xa79e1a8605f86c4cULL, 0xa79e1a8605f86c4cULL,
      0x337bc9faf9b11b69ULL, 0x337bc9faf9b11b69ULL, 0xc65d0c6639e09ed3ULL, 0xc65d0c6639e09ed3ULL,
      0x8bd616df674c6ad4ULL, 0x8bd616df674c6ad4ULL, 0x98250145ae83f832ULL, 0x98250145ae83f832ULL,
      0xe088e647ffa3ced7ULL, 0xe088e647ffa3ced7ULL, 0x54ac73041917230dULL, 0x54ac73041917230dULL,
      0x2bc70b7efccfe2c5ULL, 0x2bc70b7efccfe2c5ULL, 0xa1be19e6b5d1386fULL, 0xa1be19e6b5d1386fULL,
      0xb041301ca5fd786aULL, 0xb041301ca5fd786aULL, 0xa66cd6a9cf0d8becULL, 0xa66cd6a9cf0d8becULL,
      0xa2a051509530c74cULL, 0xa2a051509530c74cULL, 0x8b0e734caea76ab2ULL, 0x8b0e734caea76ab2ULL,
      0xb410e862022b5cbfULL, 0xb410e862022b5cbfULL, 0xfa139fff09fa12cdULL, 0xfa139fff09fa12cdULL,
      0xa2a051509530c74cULL, 0xa2a051509530c74cULL, 0x8b0e734caea76ab2ULL, 0x8b0e734caea76ab2ULL,
      0xb410e862022b5cbfULL, 0xb410e862022b5cbfULL, 0xfa139fff09fa12cdULL, 0xfa139fff09fa12cdULL,
      0xf349c1556bf2e29cULL, 0xf349c1556bf2e29cULL, 0x199311c2f83569d6ULL, 0x199311c2f83569d6ULL,
      0xc707248fcc9216cfULL, 0xc707248fcc9216cfULL, 0x4be0ad9c39de5979ULL, 0x4be0ad9c39de5979ULL};
  for (unsigned index = 0; index < expected.size(); ++index) {
    const bool divide = index >= 24;
    const unsigned mode = (index % 24) / 8;
    const unsigned alignment = (index / 2) % 4;
    const bool pal = index & 1;
    const auto program = spc7110_test::ArithmeticDmaProgram(divide, mode, alignment, pal);
    Emulator::Config config;
    config.entropyMode = EntropyMode::None;
    config.entropySeed = 0;
    auto machine = std::make_unique<Emulator>(config);
    Check(machine->LoadCartridge(program.image), "The chained arithmetic DMA guest loads");
    for (bool warm : {false, true}) {
      if (warm)
        Check(machine->Reset(), "The chained arithmetic DMA guest resets");
      for (unsigned field = 0; field < 8; ++field)
        machine->StepFrame();
      const auto *ram = machine->GetBus().WramData();
      Check(ram[0x120] == 0xa5, "The chained arithmetic DMA guest completes");
      for (unsigned sample = 0; sample < 16; ++sample) {
        const uint8_t status = uint8_t((sample < (divide ? 4u : 3u) ? 0x80 : 0) | (divide ? 0 : 1));
        Check(ram[0x1201 + sample] == status,
              "Chained DMA observes arithmetic completion at the expected byte");
      }
      const auto *control = ram + 0x10000 + program.controlOffset;
      const std::array<uint8_t, 8> completed{
          uint8_t(divide ? 0x27 : 0x25), 0x48, 0, 0, 0x2f, 0x48, 0, 0};
      Check(std::equal(completed.begin(), completed.end(), control),
            "Both chained DMA channels finish their fixed-address transfers");
      uint64_t digest = 0xcbf29ce484222325;
      for (unsigned offset = 0; offset < 0x20000; ++offset)
        digest = (digest ^ ram[offset]) * 0x100000001b3;
      Check(digest == expected[index], "Chained arithmetic DMA retains complete guest work RAM");
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
    InterleavedDmaAndHdma();
    ArithmeticDmaEdges();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("SPC7110 timing tests passed");
  return 0;
}
