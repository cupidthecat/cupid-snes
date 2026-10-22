#include "ConsoleConfig.hpp"
#include "snes/core/Spc7110.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
using namespace snes::core;
namespace {
void Check(bool result, const char *message) {
  if (!result)
    throw std::runtime_error(message);
}
void Set(Spc7110 &chip, unsigned address, uint32_t value, unsigned bytes = 2) {
  for (unsigned i = 0; i < bytes; ++i)
    chip.Write(address + i, uint8_t(value >> (i * 8)));
}
uint32_t Get(Spc7110 &chip, unsigned address, unsigned bytes = 2) {
  uint32_t value = 0;
  for (unsigned i = 0; i < bytes; ++i)
    value |= uint32_t(chip.Read(address + i)) << (i * 8);
  return value;
}
std::vector<uint8_t> Rom(bool rtc = false, size_t size = 0x300000) {
  std::vector<uint8_t> rom(size);
  for (size_t i = 0; i < size; ++i)
    rom[i] = uint8_t((i >> 20) * 31 + i);
  std::fill_n(rom.begin() + 0xffc0, 64, 0);
  rom[0x8000] = 0x78;
  rom[0x8001] = 0xdb;
  rom[0xffc0] = 'T';
  rom[0xffd5] = 0x3a;
  rom[0xffd6] = rtc ? 0xf9 : 0xf5;
  rom[0xffbf] = 0;
  rom[0xffd7] = size > 0x500000 ? 13 : 12;
  rom[0xffd8] = 3;
  rom[0xffdc] = rom[0xffdd] = 0xff;
  rom[0xfffd] = 0x80;
  return rom;
}
void Arithmetic() {
  const auto rom = Rom();
  Spc7110 chip(rom, false);
  for (bool sign : {false, true}) {
    chip.Write(0x482e, sign);
    for (uint16_t a : {0, 1, 0x7fff, 0x8000, 0xffff})
      for (uint16_t b : {0, 1, 0x7fff, 0x8000, 0xffff}) {
        Set(chip, 0x4820, a);
        Set(chip, 0x4824, b);
        Check(chip.Read(0x482f) == 0x81, "Multiplication reports busy and its operation bit");
        chip.Advance(30);
        Check(chip.Read(0x482f) == 0x81, "The product stays pending through its wait clock");
        chip.Advance(1);
        const uint32_t expected =
            sign ? uint32_t(int64_t(int16_t(a)) * int16_t(b)) : uint32_t(a) * b;
        Check(Get(chip, 0x4828, 4) == expected,
              "Signed and unsigned multiplication publish their result");
        Check(chip.Read(0x482f) == 1 && chip.Read(0x482f) == 1,
              "Completed math status is persistent");
      }
    for (uint32_t a : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
      for (uint16_t b : {0, 1, 2, 0x7fff, 0x8000, 0xffff}) {
        Set(chip, 0x4820, a, 4);
        Set(chip, 0x4826, b);
        chip.Advance(40);
        Check(chip.Read(0x482f) & 0x80, "Division retains busy during its wait");
        chip.Advance(1);
        const int64_t dividend = sign ? int64_t(int32_t(a)) : int64_t(a),
                      divisor = sign ? int64_t(int16_t(b)) : int64_t(b);
        Check(Get(chip, 0x4828, 4) == (divisor ? uint32_t(dividend / divisor) : 0),
              "Division handles overflow and zero divisors");
        Check(Get(chip, 0x482c) == (divisor ? uint16_t(dividend % divisor) : uint16_t(a)),
              "Division publishes the remainder");
      }
  }
  const auto operands = Get(chip, 0x4820, 4);
  chip.Write(0x482e, 0xff);
  Check(Get(chip, 0x4820, 4) == operands && chip.Read(0x482e) == 1,
        "Math mode masks its control without erasing operands");
  Check(chip.Read(0x4840, 0x56) == 0x56 && chip.SaveRtc().empty(),
        "A board without RTC preserves open bus");
}
void DataPorts() {
  const auto rom = Rom();
  Spc7110 chip(rom, false);
  chip.Write(0x4818, 0xff);
  Check(chip.Read(0x4818) == 0x7f && chip.Read(0x4810) == rom[0x100000],
        "Control writes prefetch immediately and mask bit seven");
  chip.Reset();
  Set(chip, 0x4811, 100, 3);
  Check(chip.Read(0x4810) == rom[0x100064] && Get(chip, 0x4811, 3) == 101,
        "Pointer-bank writes prefetch and reads advance the pointer");
  Set(chip, 0x4816, 0xfffe);
  chip.Write(0x4818, 5);
  chip.Read(0x4810);
  Check(Get(chip, 0x4811, 3) == 99, "Signed negative strides update the pointer");
  chip.Write(0x4818, 0x20);
  Set(chip, 0x4814, 3);
  Check(Get(chip, 0x4811, 3) == 102, "Low-byte adjustment mode applies on the low-byte write");
  chip.Write(0x4818, 0x60);
  Set(chip, 0x4814, 0xfffe);
  chip.Write(0x4818, 0x68);
  Check(chip.Read(0x481a) == 0 && Get(chip, 0x4811, 3) == 100,
        "The secondary port seeks and returns zero");
  chip.Write(0x4818, 0);
  chip.Write(0x4834, 1);
  Set(chip, 0x4811, 0x1fffff, 3);
  Check(chip.Read(0x4810) == rom.back() && chip.Read(0x4810) == rom[0x100000],
        "The selected data size wraps the physical ROM");
  chip.Write(0x4813, 0xff);
  Check(chip.Read(0x4813) == 0x7f, "Data pointers retain 23 address bits");
}
void Start(Spc7110 &chip) {
  chip.Write(0x4834, 1);
  chip.Write(0x4804, 0);
  chip.Write(0x4806, 0);
  chip.Advance(21);
}
void DecoderVectors() {
  constexpr uint8_t prefixes[3][32] = {
      {0x03, 0x03, 0x02, 0x60, 0x1e, 0x6c, 0x02, 0x00, 0x11, 0x0c, 0x02,
       0x10, 0x0e, 0x03, 0x1d, 0x0e, 0x01, 0x03, 0x12, 0x3f, 0x1e, 0x33,
       0x02, 0x01, 0x11, 0x01, 0x72, 0x0c, 0x6e, 0x00, 0x72, 0x1c},
      {0x00, 0x00, 0x0c, 0x18, 0x10, 0xcc, 0x97, 0x28, 0x00, 0xef, 0xa1,
       0xee, 0xf5, 0xe0, 0xfb, 0x6c, 0x77, 0x2e, 0x72, 0x05, 0x71, 0x88,
       0x51, 0xac, 0x77, 0x88, 0x73, 0x94, 0x69, 0x9e, 0x74, 0x9a},
      {0x3b, 0x39, 0xff, 0x89, 0xfa, 0x05, 0xaa, 0x05, 0x32, 0xc0, 0x0d,
       0xdd, 0xe1, 0xde, 0x45, 0xa9, 0x33, 0x20, 0x40, 0x3f, 0xa4, 0xbf,
       0xff, 0xfa, 0xff, 0x20, 0xcc, 0x29, 0x1e, 0x51, 0x5e, 0xf1}};
  constexpr uint64_t hashes[]{0x47da7bb6bb025fceULL, 0xb41a6b3287517f28ULL, 0xd7be3ec8ef36ab90ULL};

  for (unsigned mode = 0; mode < 3; ++mode) {
    auto rom = Rom();
    for (unsigned i = 0x100000; i < rom.size(); ++i)
      rom[i] = uint8_t((i * 37) ^ (i >> 4) ^ 0xa5);
    rom[0x100000] = uint8_t(mode);
    rom[0x100001] = 0;
    rom[0x100002] = 0x12;
    rom[0x100003] = 0x34;
    Spc7110 first(rom, false), second(rom, false);
    Start(first);
    Start(second);
    uint64_t hash = 14695981039346656037ull;
    for (unsigned i = 0; i < 4096; ++i) {
      const uint8_t value = first.Read(0x4800);
      if (i < 32)
        Check(value == prefixes[mode][i],
              "Each compressed mode matches its recorded output prefix");
      Check(value == second.Read(0x500000 + i),
            "Independent chips and the stream-bank alias agree");
      hash = (hash ^ value) * 1099511628211ull;
    }
    Check(hash == hashes[mode], "Complete streams preserve context evolution and tile refills");
  }
}
void Mapping() {
  auto rom = Rom();
  auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Check(emu->LoadCartridge(rom), "SPC7110 loads");
  auto &bus = emu->GetBus();
  Check(bus.Read(0x008000) == 0x78 && bus.Read(0xc08000) == 0x78,
        "Program ROM is visible through both bank groups");
  Check(bus.Read(0x108000) == rom[0x108000],
        "The next CPU program window routes through the data controller");
  bus.Write(0x4834, 3);
  for (unsigned page = 0; page < 8; ++page) {
    bus.Write(0x4831, uint8_t(page));
    Check(bus.Read(0xd01234) == rom[0x100000 + ((page & 1) << 20) + 0x1234],
          "All data selectors mirror physical storage");
  }
  bus.Write(0x006123, 0x43);
  Check(bus.Read(0x006123) == 0, "Disabled SRAM reads zero");
  bus.Write(0x804830, 0x80);
  bus.Write(0x016123, 0x43);
  Check(bus.Read(0xbf6123) == 0x43 && emu->LoadedCartridge()->SramData()[0x123] == 0x43,
        "Battery RAM follows physical board mirrors");
  bus.Write(0x4830, 0);
  Check(bus.Read(0x306123) == 0, "Disabling RAM gates reads as well as writes");
  bus.Write(0x4830, 0x80);
  Check(bus.Read(0x306123) == 0x43, "RAM contents remain while disabled");
  bus.Write(0x7e0000, 0x56);
  Check(bus.Read(0x408000) == 0x56, "Standard boards do not expose expansion ROM");
  Check(bus.Read(0x580000) == 0, "The second stream bank exposes the reserved zero register");
  auto programOnly = rom;
  programOnly.resize(0x100000);
  Check(emu->LoadCartridge(programOnly) && bus.Read(0x008000) == programOnly[0x8000],
        "A program-only image retains its program ROM mapping");
  Check(bus.Read(0xd00000) == 0, "An absent data image returns zero");
}
void QueuedMathAndDataSize() {
  const auto rom = Rom(false, 0x600000);
  Spc7110 chip(rom, false);
  Set(chip, 0x4820, 2);
  Set(chip, 0x4824, 3);
  chip.Advance(10);
  Set(chip, 0x4820, 7, 4);
  Set(chip, 0x4826, 2);
  chip.Advance(20);
  Check(Get(chip, 0x4828, 4) == 0, "Queued arithmetic does not publish at its wait boundary");
  chip.Advance(1);
  Check(Get(chip, 0x4828, 4) == 21 && chip.Read(0x482f) == 1,
        "Multiplication uses live operands and clears busy before queued division finishes");
  chip.Advance(39);
  Check(Get(chip, 0x4828, 4) == 21, "The product remains visible through the division wait");
  chip.Advance(1);
  Check(Get(chip, 0x4828, 4) == 3 && Get(chip, 0x482c) == 1,
        "Queued division completes after multiplication without losing its request");
  chip.Write(0x4831, 4);
  Check(chip.Read(0xd01234) == 0, "Smaller data configurations suppress address bit 22");
  chip.Write(0x4834, 3);
  Check(chip.Read(0xd01234) == rom[0x501234],
        "The largest data configuration exposes the fifth MiB");
  chip.Write(0x4834, 4);
  Check(chip.Read(0xd08000) == rom[0x8000],
        "The program extension bit mirrors physical program storage");
  auto expanded = Rom(true, 0x700000);
  Spc7110 extension(expanded, true);
  Check(extension.Read(0x408000) == expanded[0x608000],
        "The expansion board exposes its separate final MiB");
}
void StreamDma() {
  auto rom = Rom();
  rom[0x100000] = 1;
  rom[0x100001] = 0;
  rom[0x100002] = 0x10;
  rom[0x100003] = 0;
  auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Check(emu->LoadCartridge(rom), "The stream DMA image loads");
  auto &bus = emu->GetBus();
  bus.Write(0x4804, 0);
  bus.Write(0x4806, 0);
  emu->StepFrame();
  Spc7110 expected(rom, false);
  Start(expected);
  std::vector<uint8_t> actual;
  bus.Map(
      0, 0, 0x2110, 0x2110, [](uint32_t, uint8_t value) { return value; },
      [&](uint32_t, uint8_t value) { actual.push_back(value); });
  auto &dma = emu->GetDma();
  auto &channel = dma.Channel(0);
  channel.writeControl(0);
  channel.sourceBank = 0x50;
  channel.sourceAddress = 0xfff0;
  channel.targetAddress = 0x10;
  channel.transferSize = 100;
  dma.EnableDma(1);
  dma.RunDma();
  Check(actual.size() == 100, "DMA consumes the stream across a source-bank wrap");
  for (uint8_t value : actual)
    Check(value == expected.Read(0x4800), "DMA and CPU reads preserve the same decoded stream");
  Check(bus.Read(0x4809) == uint8_t(-100) && bus.Read(0x480a) == 0xff,
        "DMA stream reads wrap the decompression counter");
}
void Clock() {
  const auto rom = Rom(true);
  int64_t now = 1700000000;
  Spc7110 chip(rom, true, [&] { return now; });
  const auto select = [&](unsigned index) {
    chip.Write(0x4840, 0);
    chip.Write(0x4840, 1);
    chip.Write(0x4841, 3);
    chip.Advance(100);
    chip.Write(0x4841, uint8_t(index));
    chip.Advance(100);
  };
  const auto put = [&](uint8_t value) {
    chip.Write(0x4841, value);
    chip.Advance(100);
  };
  select(15);
  put(4);
  select(0);
  const std::array<uint8_t, 16> digits{9, 5, 9, 5, 3, 2, 8, 2, 2, 0, 4, 2, 3, 2, 1, 4};
  for (auto value : digits)
    put(value);
  chip.Advance(21'477'272);
  auto save = chip.SaveRtc();
  Check(save.size() == 24 && save[0] == 0 && save[4] == 0 && save[6] == 9 && (save[7] & 3) == 2 &&
            (save[12] & 7) == 4,
        "The RTC advances across midnight into leap day");
  Check(chip.Read(0x4842) == 0x80 && chip.Read(0x4842) == 0x80, "RTC readiness is persistent");
  select(15);
  put(6);
  const auto stopped = chip.SaveRtc();
  chip.Advance(21'477'272 * 60u);
  Check(chip.SaveRtc() == stopped, "A stopped clock retains time while emulation advances");
  select(15);
  put(4);
  chip.Advance(21'477'272);
  Check(chip.SaveRtc()[0] == 1, "The clock resumes from its stopped time");
  save = chip.SaveRtc();
  Spc7110 restored(rom, true, [&] { return now; });
  Check(restored.LoadRtc(save) && restored.SaveRtc() == save,
        "RTC saves round trip through the legacy 24-byte format");
  save[0] = 0xff;
  Check(!restored.LoadRtc(save), "Malformed RTC saves are rejected");
  auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Check(emu->LoadCartridge(rom) && emu->SaveRtc().size() == 24,
        "The RTC connects to frontend saves");
}
} // namespace
int main() {
  try {
    Arithmetic();
    DataPorts();
    DecoderVectors();
    Mapping();
    QueuedMathAndDataSize();
    StreamDma();
    Clock();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("SPC7110 decoder, registers, mapping, and RTC tests passed");
}
