#include "snes/core/gameboy/Cartridge.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>

using snes::core::gameboy::Cartridge;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::vector<uint8_t> Image(uint8_t type, unsigned banks, uint8_t ram = 3) {
  std::vector<uint8_t> image(banks * 0x4000);
  for (unsigned bank = 0; bank < banks; ++bank) {
    std::fill_n(image.begin() + bank * 0x4000, 0x4000, uint8_t(bank));
    image[bank * 0x4000 + 1] = uint8_t(bank >> 8);
  }
  for (unsigned i = 0; i < 48; ++i)
    image[0x104 + i] = uint8_t(i * 7 + 1);
  image[0x147] = type;
  image[0x149] = ram;
  return image;
}
void Mbc1Modes() {
  Cartridge cartridge;
  Check(cartridge.Load(Image(3, 128)), "Load the generated MBC1 image");
  Check(cartridge.Read(0x4000) == 1 && !cartridge.Read(0xa000),
        "MBC1 starts on bank one with RAM disabled");
  cartridge.Write(0x2000, 0);
  cartridge.Write(0x4000, 2);
  Check(cartridge.Read(0x4000) == 65 && cartridge.Read(0) == 0,
        "MBC1 combines high ROM bits and remaps the prohibited low bank");
  cartridge.Write(0x6000, 1);
  Check(cartridge.Read(0) == 64, "MBC1 RAM mode also banks the lower ROM window");
  cartridge.Write(0x0000, 0x1a);
  cartridge.Write(0xa123, 0x42);
  cartridge.Write(0x4000, 1);
  cartridge.Write(0xa123, 0x31);
  cartridge.Write(0x4000, 2);
  Check(cartridge.Read(0xa123) == 0x42, "MBC1 RAM banks retain separate contents");
  cartridge.Reset();
  Check(cartridge.Read(0) == 0 && cartridge.Read(0x4000) == 1 && !cartridge.Read(0xa123),
        "Reset restores banking and disables RAM");
  cartridge.Write(0x0000, 0x0a);
  cartridge.Write(0x6000, 1);
  cartridge.Write(0x4000, 2);
  Check(cartridge.Read(0xa123) == 0x42, "Reset retains battery RAM");
}

void MulticartAndSmallRam() {
  auto image = Image(3, 64);
  std::copy_n(image.begin() + 0x104, 48, image.begin() + 0x40104);
  Cartridge cartridge;
  Check(cartridge.Load(image), "Load the generated multicart image");
  cartridge.Write(0x2000, 0x10);
  cartridge.Write(0x4000, 2);
  cartridge.Write(0x6000, 1);
  Check(cartridge.Read(0x4000) == 32 && cartridge.Read(0) == 32,
        "Multicart wiring retains its low-bank-zero case and moves high ROM bits");
  cartridge.Write(0x2000, 0);
  Check(cartridge.Read(0x4000) == 33, "An all-zero low bank selects one on a multicart");

  Check(cartridge.Load(Image(6, 16, 0)), "Load the generated MBC2 image");
  cartridge.Write(0x0100, 3);
  Check(cartridge.Read(0x4000) == 3 && !cartridge.Read(0xa000),
        "MBC2 address bit eight distinguishes ROM bank writes from RAM enable");
  cartridge.Write(0x0000, 0x0a);
  cartridge.Write(0xa011, 0xab);
  Check(cartridge.Read(0xb211) == 0xfb && cartridge.Ram().size() == 512,
        "MBC2 mirrors its 512 cells and drives the high nibble high");
  Check(cartridge.SaveBattery()[0x11] == 0xab,
        "Battery data retains the stored MBC2 byte representation");
}

void Mbc3AndMbc5Banks() {
  Cartridge cartridge;
  Check(cartridge.Load(Image(0x13, 128, 3)), "Load the generated MBC3 image");
  cartridge.Write(0x2000, 0x80);
  Check(cartridge.Read(0x4000) == 1, "MBC3 masks the eighth ROM bank bit and remaps zero");
  cartridge.Write(0, 10);
  cartridge.Write(0x4000, 7);
  cartridge.Write(0xa000, 0x71);
  cartridge.Write(0x4000, 3);
  Check(cartridge.Read(0xa000) == 0x71, "MBC3 without a clock masks RAM selection to two bits");
  Check(cartridge.Load(Image(0x13, 256, 4)), "Load the extended MBC3 image");
  cartridge.Write(0x2000, 0x80);
  Check(cartridge.Read(0x4000) == 128, "Extended MBC3 retains the eighth ROM bank bit");
  cartridge.Write(0, 10);
  cartridge.Write(0x4000, 7);
  cartridge.Write(0xa000, 0x72);
  cartridge.Write(0x4000, 3);
  Check(cartridge.Read(0xa000) == 0xff, "Extended MBC3 exposes additional RAM banks");

  Check(cartridge.Load(Image(0x1e, 512, 4)), "Load the generated MBC5 image");
  cartridge.Write(0x2000, 0);
  cartridge.Write(0x3000, 1);
  Check(cartridge.Read(0x4000) == 0 && cartridge.Read(0x4001) == 1,
        "MBC5 selects ROM bank 256 using its separate ninth bit");
  cartridge.Write(0x3000, 0);
  Check(cartridge.Read(0x4000) == 0 && cartridge.Read(0x4001) == 0,
        "MBC5 permits ROM bank zero in the upper window");
  cartridge.Write(0, 0x1a);
  Check(!cartridge.Read(0xa000), "MBC5 RAM enable requires the complete byte to equal ten");
  cartridge.Write(0, 10);
  cartridge.Write(0x4000, 0x0b);
  cartridge.Write(0xa000, 0x58);
  cartridge.Write(0x4000, 3);
  Check(cartridge.Read(0xa000) == 0x58 && !cartridge.Rumble(),
        "Rumble selects a motor without adding a RAM bank address bit");
}

void LockingMulticart() {
  Cartridge cartridge;
  Check(cartridge.Load(Image(0x0d, 128, 3)), "Load the generated locking multicart");
  Check(cartridge.Read(0) == 0 && cartridge.Read(0x4000) == 1,
        "A locking multicart starts in the menu banks after image rotation");
  cartridge.Write(0, 0x0a);
  cartridge.Write(0x2000, 0x41);
  cartridge.Write(0x4000, 0);
  cartridge.Write(0x6000, 0);
  cartridge.Write(0, 0x4a);
  Check(cartridge.Read(0) == 66 && cartridge.Read(0x4000) == 67,
        "Locking selects the programmed game and retains the rotated physical bank order");
  cartridge.Write(0x2000, 2);
  Check(cartridge.Read(0) == 66 && cartridge.Read(0x4000) == 68,
        "Locked game banks retain the menu-selected high ROM address bits");
  cartridge.Write(0xa000, 0x77);
  Check(cartridge.Ram()[3 * 8192] == 0x77, "The RAM mask forces unselected RAM address bits high");

  cartridge.Reset();
  cartridge.Write(0, 0x3a);
  cartridge.Write(0x2000, 0x41);
  cartridge.Write(0x4000, 1);
  cartridge.Write(0x6000, 0x40);
  cartridge.Write(0, 0x7a);
  Check(cartridge.Read(0) == 34 && cartridge.Read(0x4000) == 35,
        "Multiplexing exchanges the middle ROM bank bits with the RAM bank bits");
  cartridge.Write(0x6000, 1);
  Check(cartridge.Read(0) == 2 && cartridge.Read(0x4000) == 35,
        "RAM mode releases the lower ROM window while retaining the selected upper bank");
}

void RtcAndBattery() {
  Cartridge noRam;
  Check(noRam.Load(Image(0x0f, 2, 0)), "Load a clock cartridge with no RAM");
  noRam.Write(0, 10);
  noRam.Write(0x4000, 4);
  Check(!noRam.Read(0xa000), "A clock cartridge without RAM leaves RAM reads undriven");

  uint64_t now = 1789389296;
  Cartridge cartridge;
  cartridge.SetTimeSource([&] { return now; });
  Check(cartridge.Load(Image(0x10, 2, 3)), "Load the generated RTC image");
  cartridge.Write(0, 10);
  const auto write = [&](unsigned index, uint8_t value) {
    cartridge.Write(0x4000, uint8_t(index + 8));
    cartridge.Write(0xa000, value);
  };
  const auto read = [&](unsigned index) {
    cartridge.Write(0x4000, uint8_t(index + 8));
    return cartridge.Read(0xa000);
  };
  write(0, 59);
  write(1, 59);
  write(2, 23);
  write(3, 255);
  write(4, 1);
  cartridge.Write(0x6000, 0x55);
  now += 2;
  cartridge.SynchronizeRtc(now);
  Check(read(0) == 59, "Clock reads retain latched values until a latch write");
  cartridge.Write(0x6000, 0x55);
  Check(read(0) == 1 && read(1) == 0 && read(2) == 0 && read(3) == 0 && read(4) == 0x80,
        "A latch write exposes elapsed time and the 512-day overflow flag");
  write(0, 0xff);
  cartridge.Write(0x6000, 0);
  Check(read(0) == 0x3f, "Clock reads mask unused bits in the latched registers");
  auto battery = cartridge.SaveBattery();
  Check(battery.size() == 0x8000 + 48 && battery[0x8000] == 0xff && battery[0x8000 + 20] == 0x3f,
        "Battery saves include RAM, raw clock bytes, and the masked latched clock");
  write(0, 0);
  write(4, 0x40);
  now += 10;
  cartridge.SynchronizeRtc(now);
  cartridge.Write(0x6000, 1);
  Check(read(0) == 0, "The clock halt flag freezes timekeeping");
  write(4, 0);
  cartridge.SynchronizeRtc(now);
  cartridge.Write(0x6000, 1);
  Check(read(0) == 10, "Resuming host time accounts for the elapsed interval");
  cartridge.LoadBattery(std::span<const uint8_t>(battery).first(0x8000));
  cartridge.Write(0x6000, 0);
  Check(read(4).value() & 0x80, "A missing clock trailer marks the clock for initialization");
  std::vector<uint8_t> legacy(0x8000 + 13);
  legacy[0x8000] = 17;
  legacy[0x8001] = 23;
  legacy[0x8002] = 12;
  for (unsigned i = 0; i < 8; ++i)
    legacy[0x8005 + i] = uint8_t(now >> (i * 8));
  cartridge.LoadBattery(legacy);
  Check(read(0) == 17 && read(1) == 23 && read(2) == 12,
        "Legacy clock saves use five packed register bytes followed by a 64-bit timestamp");
}

void MotionAndEeprom() {
  Cartridge cartridge;
  Check(cartridge.Load(Image(0x22, 32)), "Load the generated motion cartridge");
  Check(cartridge.Read(0x4000) == 0 && cartridge.Read(0xa020) == 0xff,
        "Motion cartridges start on ROM bank zero with their peripheral window disabled");
  cartridge.Write(0, 10);
  cartridge.Write(0x4000, 0x40);
  Check(cartridge.Read(0xa020) == 0 && cartridge.Read(0xa030) == 0x80 &&
            cartridge.Read(0xb020) == 0xff,
        "The peripheral window contains reset sensor latches and ends before B000");
  cartridge.SetMotion(0.5, -1.25);
  cartridge.Write(0xa000, 0x55);
  cartridge.Write(0xa010, 0xaa);
  Check(cartridge.Read(0xa020) == 8 && cartridge.Read(0xa030) == 0x82 &&
            cartridge.Read(0xa040) == 0x44 && cartridge.Read(0xa050) == 0x81,
        "Motion latches expose the scaled sensor words through mirrored byte ports");
  const auto bit = [&](bool value) {
    cartridge.Write(0xa080, uint8_t(0x80 | (value ? 2 : 0)));
    cartridge.Write(0xa080, uint8_t(0xc0 | (value ? 2 : 0)));
    return cartridge.Read(0xa080).value() & 1;
  };
  const auto command = [&](unsigned value) {
    for (int i = 10; i >= 0; --i)
      bit(value & (1u << i));
  };
  const auto data = [&](unsigned value) {
    for (int i = 15; i >= 0; --i)
      bit(value & (1u << i));
  };
  const auto read = [&] {
    unsigned value = 0;
    for (unsigned i = 0; i < 16; ++i)
      value = (value << 1) | bit(false);
    return value;
  };
  command(0x4c0);
  command(0x500 | 12);
  data(0xa55a);
  command(0x600 | 12);
  Check(read() == 0xa55a && cartridge.Ram()[24] == 0x5a && cartridge.Ram()[25] == 0xa5,
        "EEPROM writes and reads shift words most significant bit first with little-endian saves");
  command(0x400);
  command(0x500 | 12);
  data(0x0101);
  command(0x600 | 12);
  Check(read() == 0xa55b,
        "A disabled write retains the old word before incoming data bits combine");
  command(0x4c0);
  command(0x440);
  data(0x1234);
  Check(cartridge.Ram()[0] == 0x34 && cartridge.Ram()[1] == 0x12 && cartridge.Ram()[254] == 0 &&
            cartridge.Ram()[255] == 0,
        "Write-all clears the array and shifts data into the first 127 words");
  command(0x480);
  Check(std::all_of(cartridge.Ram().begin(), cartridge.Ram().end(),
                    [](uint8_t value) { return value == 0xff; }) &&
            cartridge.SaveBattery().size() == 256,
        "Erase-all restores every EEPROM byte and battery saves contain all 128 words");
}

void ClockPollingPhase() {
  unsigned polls = 0;
  Cartridge cartridge;
  cartridge.SetTimeSource([&] {
    ++polls;
    return uint64_t(1789389296);
  });
  Check(cartridge.Load(Image(0x0f, 2, 0)), "Load a cartridge for oscillator timing checks");
  polls = 0;
  cartridge.SetClockRate(4295454);
  cartridge.AdvanceRtc(134232);
  Check(polls == 0, "An odd half-clock polling interval retains its fractional clock");
  cartridge.AdvanceRtc(1);
  Check(polls == 1, "RTC host polling begins after the complete interval");
  cartridge.AdvanceRtc(134231);
  Check(polls == 1, "The retained half clock carries into the following interval");
  cartridge.AdvanceRtc(1);
  Check(polls == 2, "Two RTC polling intervals retain the exact oscillator ratio");
}
} // namespace

int main() {
  try {
    Mbc1Modes();
    MulticartAndSmallRam();
    Mbc3AndMbc5Banks();
    LockingMulticart();
    RtcAndBattery();
    MotionAndEeprom();
    ClockPollingPhase();
    std::puts("Handheld cartridge banking, RAM, RTC and battery checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
