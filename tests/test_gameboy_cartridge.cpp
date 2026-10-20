#include "snes/core/gameboy/Cartridge.hpp"

#include <algorithm>
#include <array>
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

void InfraredBankingAndBattery() {
  Cartridge cartridge;
  Check(cartridge.Load(Image(0xff, 128, 4)), "Load the generated infrared cartridge");
  Check(cartridge.Read(0x4000) == 1 && cartridge.Read(0xa000) == 0xff,
        "Infrared cartridges start on ROM bank one with RAM accessible");
  cartridge.Write(0, 0);
  Check(cartridge.Read(0x4000) == 0,
        "The first bank-register write commits the reset zero-valued ROM selector");
  cartridge.Write(0x2000, 0);
  Check(cartridge.Read(0x4000) == 0, "The upper ROM window can select bank zero");
  cartridge.Write(0x3000, 0xff);
  Check(cartridge.Read(0x4000) == 63 && cartridge.Read(0) == 0,
        "Six ROM bank bits select the upper window without banking the lower window");
  for (unsigned bank = 0; bank < 8; ++bank) {
    cartridge.Write(0x4000, uint8_t(bank));
    cartridge.Write(0xa000, uint8_t(0x30 + bank));
  }
  cartridge.Write(0x4000, 0xf8);
  Check(cartridge.Read(0xa000) == 0x30, "The RAM selector uses three bank bits");
  cartridge.Write(0x6000, 1);
  cartridge.Write(0, 0);
  Check(cartridge.Read(0xa000) == 0x30 && cartridge.Read(0x4000) == 63,
        "The unused mode register and ordinary RAM gate values leave banks connected");
  cartridge.Write(0, 0x1e);
  cartridge.Write(0xa000, 0x55);
  Check(cartridge.Read(0xbfff) == 0xc0 && cartridge.InfraredOutput(),
        "The infrared window mirrors status and sends the low written bit");
  cartridge.Write(0, 0);
  Check(cartridge.Read(0xa000) == 0x30 && cartridge.InfraredOutput(),
        "Infrared writes preserve RAM and leaving sensor mode retains the transmitter latch");
  const auto battery = cartridge.SaveBattery();
  Check(battery.size() == 0x20000 && battery[0x10000] == 0xff,
        "Battery storage retains the declared RAM size beyond the connected bank lines");
  Cartridge restored;
  Check(restored.Load(Image(0xff, 128, 4)), "Reload the generated infrared cartridge");
  restored.LoadBattery(battery);
  for (unsigned bank = 0; bank < 8; ++bank) {
    restored.Write(0x4000, uint8_t(bank));
    Check(restored.Read(0xa000) == 0x30 + bank,
          "Battery reload preserves every connected RAM bank");
  }
  cartridge.Reset();
  Check(!cartridge.InfraredOutput() && cartridge.Read(0x4000) == 1 &&
            cartridge.Read(0xa000) == 0x30,
        "Reset clears infrared and bank latches while preserving battery RAM");
}

void InfraredTiming() {
  Cartridge cartridge;
  Check(cartridge.Load(Image(0xff, 2, 0)), "Infrared operation does not require cartridge RAM");
  cartridge.Write(0, 14);
  cartridge.SetInfraredInput(true);
  cartridge.Advance(10069);
  Check(cartridge.Read(0xa000) == 0xc0, "A cold sensor waits through its warmup interval");
  cartridge.Advance(1);
  Check(cartridge.Read(0xa000) == 0xc1, "The cold sensor detects light at the threshold clock");
  cartridge.SetInfraredInput(false);
  cartridge.Advance(1);
  Check(cartridge.Read(0xa000) == 0xc0, "Removing light clears detection on the next timing batch");
  cartridge.Reset();
  cartridge.Write(0, 0xfe);
  cartridge.Advance(9950);
  cartridge.Write(0xbfff, 3);
  cartridge.Advance(119);
  Check(cartridge.Read(0xa000) == 0xc0,
        "A warmed sensor retains the shorter light detection delay");
  cartridge.Advance(1);
  Check(cartridge.Read(0xa000) == 0xc1 && cartridge.InfraredOutput(),
        "The sensor detects its own transmitter after the warm detection interval");
  cartridge.Advance(50000);
  Check(cartridge.Read(0xa000) == 0xc1,
        "Continuous light holds the sensor above its detection threshold");
  cartridge.Write(0xa000, 2);
  cartridge.Advance(1);
  Check(!cartridge.InfraredOutput() && cartridge.Read(0xa000) == 0xc0,
        "Clearing the output bit removes reflected light on the next timing batch");
  cartridge.SetInfraredInput(true);
  cartridge.Reset();
  cartridge.Write(0, 14);
  cartridge.Advance(10070);
  Check(cartridge.Read(0xa000) == 0xc0 && !cartridge.InfraredOutput(),
        "Reset clears incoming light, outgoing light, and the sensor charge");
}

void Huc3Command(Cartridge &cartridge, uint8_t value) {
  cartridge.Write(0, 0xb);
  cartridge.Write(0xa000, value);
}
void Huc3Cursor(Cartridge &cartridge, uint8_t index) {
  Huc3Command(cartridge, uint8_t(0x40 | (index & 15)));
  Huc3Command(cartridge, uint8_t(0x50 | (index >> 4)));
}
uint8_t Huc3Read(Cartridge &cartridge) {
  Huc3Command(cartridge, 0x10);
  cartridge.Write(0, 0xc);
  return cartridge.Read(0xa000).value();
}
std::vector<uint8_t> Huc3Payload(uint64_t timestamp, std::array<uint16_t, 4> words,
                                 uint8_t alarm = 1) {
  std::vector<uint8_t> bytes(17);
  for (unsigned i = 0; i < 8; ++i)
    bytes[i] = uint8_t(timestamp >> (i * 8));
  for (unsigned i = 0; i < words.size(); ++i) {
    bytes[8 + i * 2] = uint8_t(words[i]);
    bytes[9 + i * 2] = uint8_t(words[i] >> 8);
  }
  bytes[16] = alarm;
  return bytes;
}

void Huc3PortsAndBanks() {
  Cartridge cartridge;
  Check(cartridge.Load(Image(0xfe, 128, 3)),
        "Load the generated cartridge with clock and infrared");
  Check(cartridge.Read(0x4000) == 1 && cartridge.Read(0xa000) == 0xff,
        "The reset bank is mapped before the zero-valued selectors are committed");
  cartridge.Write(0x6000, 0xff);
  Check(cartridge.Read(0x4000) == 0,
        "An unused register write still commits the reset ROM selector");
  cartridge.Write(0x2000, 0xff);
  Check(cartridge.Read(0x4000) == 127, "Seven ROM bank bits select the upper ROM window");
  cartridge.Write(0, 0x1a);
  for (unsigned bank = 0; bank < 8; ++bank) {
    cartridge.Write(0x4000, uint8_t(bank));
    cartridge.Write(0xa000, uint8_t(0x40 + bank));
  }
  for (unsigned bank = 8; bank < 16; ++bank) {
    cartridge.Write(0x4000, uint8_t(bank));
    const auto before = cartridge.Read(0xa000);
    cartridge.Write(0xa000, 0x90);
    Check(before == 0x44 + (bank & 3) && cartridge.Read(0xa000) == before,
          "Upper RAM bank selections remain readable while ignoring writes");
  }
  cartridge.Write(0x4000, 0);
  cartridge.Write(0, 0);
  cartridge.Write(0xa000, 0x80);
  Check(cartridge.Read(0xa000) == 0x44, "Mode zero exposes read-only cartridge RAM");
  for (uint8_t mode : {uint8_t(1), uint8_t(0xb), uint8_t(0xd), uint8_t(0xf)}) {
    cartridge.Write(0, mode);
    Check(cartridge.Read(0xbfff) == 1, "Status and unsupported read modes return the idle value");
  }
  cartridge.Write(0, 0xe);
  cartridge.SetInfraredInput(true);
  cartridge.Advance(10070);
  Check(cartridge.Read(0xa000) == 1,
        "Clock cartridges expose the filtered infrared bit without a status prefix");
}

void Huc3CommandsAndSaves() {
  constexpr uint64_t now = 1789389296;
  Cartridge cartridge;
  cartridge.SetTimeSource([] { return now; });
  Check(cartridge.Load(Image(0xfe, 2, 0)), "Clock command ports operate without cartridge RAM");
  Huc3Cursor(cartridge, 0);
  for (uint8_t nibble : {12, 11, 10, 1, 2, 3, 4})
    Huc3Command(cartridge, uint8_t(0x30 | nibble));
  Huc3Cursor(cartridge, 0);
  Check(Huc3Read(cartridge) == 12 && Huc3Read(cartridge) == 11 && Huc3Read(cartridge) == 10,
        "Read and write commands stream minute nibbles through an incrementing cursor");
  Huc3Command(cartridge, 0x62);
  cartridge.Write(0, 0xc);
  Check(cartridge.Read(0xa000) == 1, "Access flag two overrides the read latch with ready status");
  Huc3Command(cartridge, 0x60);
  Huc3Cursor(cartridge, 0x58);
  Huc3Command(cartridge, 0x10);
  cartridge.Write(0, 0xc);
  Check(cartridge.Read(0xa000) == 10, "Unsupported clock reads preserve the previous data latch");
  Huc3Cursor(cartridge, 0xff);
  Check(Huc3Read(cartridge) == 10 && Huc3Read(cartridge) == 12,
        "The byte-sized command cursor wraps from an unsupported register to minute zero");
  Huc3Cursor(cartridge, 0);
  Huc3Command(cartridge, 0x2e);
  Check(Huc3Read(cartridge) == 14, "Command two writes a nibble without advancing the cursor");
  Huc3Cursor(cartridge, 0x58);
  for (uint8_t nibble : {9, 8, 7, 13, 12, 11, 10, 1})
    Huc3Command(cartridge, uint8_t(0x30 | nibble));
  Check(cartridge.SaveBattery() == Huc3Payload(now, {0xabe, 0x4321, 0x789, 0xabcd}),
        "Clock saves contain a timestamp, four little-endian words, and the alarm enable byte");
  cartridge.LoadBattery(Huc3Payload(now, {0xfabc, 123, 789, 234}, 7));
  Huc3Cursor(cartridge, 0);
  Huc3Command(cartridge, 0x2e);
  Check(cartridge.SaveBattery() == Huc3Payload(now, {0xfabe, 123, 789, 234}),
        "Nibble writes retain unaddressed minute bits and saves normalize the alarm flag");
  Huc3Cursor(cartridge, 0x5f);
  Huc3Command(cartridge, 0x62);
  cartridge.Reset();
  Check(cartridge.SaveBattery() == Huc3Payload(now, {0, 0, 0, 0}, 0),
        "Processor reset clears clock words, command latches, and alarm state");
  cartridge.Write(0, 0xc);
  Check(cartridge.Read(0xa000) == 0, "Reset clears the read latch and access flags");
  Huc3Command(cartridge, 0x2f);
  Check(cartridge.SaveBattery() == Huc3Payload(now, {15, 0, 0, 0}, 0),
        "Reset returns the nibble cursor to the minute register");
}

void Huc3ClockBoundaries() {
  constexpr uint64_t now = 1789389296;
  Cartridge cartridge;
  cartridge.SetTimeSource([] { return now; });
  Check(cartridge.Load(Image(0xfe, 2, 0)),
        "Load the generated clock image for elapsed-time checks");
  cartridge.SetClockRate(4295454);
  cartridge.LoadBattery(Huc3Payload(now - 86401, {123, 456, 789, 234}));
  Check(cartridge.SaveBattery() == Huc3Payload(now - 86401, {123, 456, 789, 234}),
        "Loading past clock data waits for the next host polling interval");
  cartridge.AdvanceRtc(134232);
  Check(cartridge.SaveBattery() == Huc3Payload(now - 86401, {123, 456, 789, 234}),
        "Clock polling retains the fractional interval of the first adapter model");
  cartridge.AdvanceRtc(1);
  Check(cartridge.SaveBattery() == Huc3Payload(now - 1, {123, 457, 789, 234}),
        "The first poll advances whole-minute boundaries while retaining the timestamp remainder");
  cartridge.Reset();
  cartridge.LoadBattery(Huc3Payload(0, {123, 456, 789, 234}));
  cartridge.AdvanceRtc(134233);
  Check(cartridge.SaveBattery() == Huc3Payload(1789389240, {877, 21166, 789, 234}),
        "An epoch-zero clock advances across long elapsed periods at its first poll");
  cartridge.LoadBattery(Huc3Payload(now, {1440, 0xffff, 789, 234}));
  cartridge.SynchronizeRtc(now + 60);
  Check(cartridge.SaveBattery() == Huc3Payload(now + 60, {1441, 0xffff, 789, 234}),
        "A programmed minute value beyond the day boundary retains its full word");
  cartridge.SynchronizeRtc(now + 65536 * 60);
  const auto wrapped = Huc3Payload(now + 65536 * 60, {0, 0, 789, 234});
  Check(cartridge.SaveBattery() == wrapped,
        "Minute and day words wrap through their counter widths");
  cartridge.SynchronizeRtc(now);
  Check(cartridge.SaveBattery() == wrapped,
        "A backward host clock preserves the stored clock state");
  cartridge.LoadBattery(Huc3Payload(now + 3600, {123, 456, 789, 234}));
  Check(cartridge.SaveBattery() == Huc3Payload(now, {0xfff, 0xffff, 789, 234}, 0),
        "A future saved timestamp invalidates the clock while retaining alarm time words");
  cartridge.Reset();
  cartridge.LoadBattery(std::array<uint8_t, 16>{});
  Check(cartridge.SaveBattery() == Huc3Payload(now, {0xfff, 0xffff, 0, 0}, 0),
        "A truncated clock payload restores the unknown-time markers");
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

void ClockResetPhase() {
  for (uint32_t rate : {4194304u, 4295454u})
    for (uint8_t type : {uint8_t(0x0f), uint8_t(0xfe), uint8_t(0xbc)}) {
      uint64_t now = 1789389296;
      unsigned polls = 0;
      Cartridge cartridge;
      cartridge.SetTimeSource([&] {
        ++polls;
        return now;
      });
      auto image = Image(type, 2, 0);
      if (type == 0xbc) {
        image[0x149] = 0xc1;
        image[0x14a] = 0x65;
        image[0x152] = 0;
        image[0x153] = 8;
      }
      Check(cartridge.Load(image), "Load a battery clock for reset timing checks");
      cartridge.SetClockRate(rate);
      cartridge.AdvanceRtc(1000);
      cartridge.Reset();
      now += 60;
      polls = 0;
      const unsigned remaining = (rate / 16 + 1) / 2 - 1000;
      cartridge.AdvanceRtc(remaining - 1);
      Check(polls == 0, "Reset retains the incomplete battery clock polling interval");
      cartridge.AdvanceRtc(1);
      Check(polls == 1, "Reset preserves the next battery clock polling edge");
    }
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
    InfraredBankingAndBattery();
    InfraredTiming();
    Huc3PortsAndBanks();
    Huc3CommandsAndSaves();
    Huc3ClockBoundaries();
    ClockPollingPhase();
    ClockResetPhase();
    std::puts("Handheld cartridge banking, RAM, RTC and battery checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
