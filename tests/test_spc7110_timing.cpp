#include "snes/core/Cartridge.hpp"

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
} // namespace
int main() {
  try {
    DecompressionReady();
    FreshSaveRam();
    HeaderSpeedFlags();
    ReservedMode();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("SPC7110 timing tests passed");
}
