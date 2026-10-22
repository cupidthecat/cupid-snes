#include "ConsoleConfig.hpp"
#include "cartridge/St010CommandFixture.hpp"
#include "snes/core/St010.hpp"
#include "st010_cases.hpp"

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
using Ram = std::array<uint8_t, 4096>;
void Word(Ram &ram, unsigned offset, uint16_t value) {
  ram[offset] = uint8_t(value);
  ram[offset + 1] = uint8_t(value >> 8);
}
uint16_t Word(const Ram &ram, unsigned offset) {
  return uint16_t(ram[offset] | (uint16_t(ram[offset + 1]) << 8));
}
uint32_t Long(const Ram &ram, unsigned offset) {
  return Word(ram, offset) | (uint32_t(Word(ram, offset + 2)) << 16);
}
void CpuPortGuest() {
  constexpr std::array<uint8_t, 22> expected{0,    0x55, 0x12, 0x60, 0xe0, 0x73, 0x73, 0x93,
                                             0x70, 0xf0, 0x90, 0x90, 6,    5,    0x38, 1,
                                             6,    7,    0x7e, 0x55, 0x11, 0x7e};
  for (bool pal : {false, true}) {
    auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    Check(machine->LoadCartridge(st010_test::PortGuest(pal)), "The command-port guest loads");
    Check(!machine->LoadedCartridge()->HasSignalProcessor(), "The guest selects command execution");
    for (bool warm : {false, true}) {
      if (warm)
        Check(machine->Reset(), "The command-port guest resets");
      machine->StepFrame();
      const auto *wram = machine->GetBus().WramData();
      Check(wram[0x120] == 0xa5, "The command-port guest completes");
      Check(std::equal(expected.begin(), expected.end(), wram + 0x200),
            "CPU command-port readbacks, mirrors, open bus and ROM windows");
    }
  }
}
void Run(St010 &device, Ram &ram, uint8_t command) {
  device.Write(0x680020, command, ram);
  device.Write(0x680021, 0x80, ram);
  Check(device.Read(0x680020, ram) == command && device.Read(0x680021, ram) == 0,
        "Completion retains the command byte and clears the execution bit");
}

void RegistersAndMath() {
  St010 device;
  Ram ram{};
  Word(ram, 0, 3);
  Word(ram, 2, 7);
  device.Write(0x680020, 6, ram);
  Check(device.Read(0x680020, ram) == 6 && Long(ram, 0x10) == 0,
        "The command byte reads directly from shared RAM before execution");
  device.Write(0x680021, 0x85, ram);
  Check(Long(ram, 0x10) == 42 && device.Read(0x680021, ram) == 5 && ram[0x21] == 5,
        "A command needs no enable write and preserves the lower execution bits");
  Check(!St010::Selects(0x600000) && !St010::Selects(0xe00000),
        "The command model leaves the processor port windows disconnected");
  Run(device, ram, 6);
  Check(Long(ram, 0x10) == 42, "Signed multiply produces twice the product");
  Word(ram, 0, 0x8000);
  Word(ram, 2, 0x8000);
  Run(device, ram, 6);
  Check(Long(ram, 0x10) == 0x80000000, "Multiply wraps the positive 32-bit overflow");
  Word(ram, 0, 0xfffd);
  Word(ram, 2, 4);
  Word(ram, 4, 5);
  Run(device, ram, 3);
  Check(Long(ram, 0x10) == uint32_t(-30) && Long(ram, 0x14) == 40, "Signed coordinate scaling");
  Word(ram, 0, 3);
  Word(ram, 2, 4);
  Run(device, ram, 4);
  Check(Word(ram, 0x10) == 5, "Vector distance");
  Word(ram, 0, 100);
  Word(ram, 2, 200);
  Word(ram, 4, 0x4000);
  Run(device, ram, 8);
  Check(Word(ram, 0x10) == 199 && Word(ram, 0x12) == uint16_t(-99),
        "Quarter-turn rotation keeps integer rounding");
  Word(ram, 0, 0);
  Word(ram, 2, 0xffff);
  Run(device, ram, 1);
  Check(Word(ram, 4) == 0x8000 && Word(ram, 6) == 0 && Word(ram, 0x10) == 0,
        "Compass retains RAM outside its result slots and adjusts the axis quadrant");
  Word(ram, 0, 0);
  Run(device, ram, 7);
  Check(Word(ram, 0xf0) == 895 && Word(ram, 0x510) == 895 && Word(ram, 0x3b0) == 0,
        "Raster output duplicates A/D and leaves zero B/C terms zero");
  Check(Word(ram, 0x24e) == 42 && Word(ram, 0x66e) == 42, "Raster includes final row");
  const auto before = ram;
  Run(device, ram, 0xff);
  auto expected = before;
  expected[0x20] = 0xff;
  Check(ram == expected, "Unknown commands acknowledge without modifying results");
}

void DriverSorting() {
  St010 device;
  Ram ram{};
  Word(ram, 0x24, 4);
  for (unsigned i = 0; i < 4; ++i) {
    Word(ram, 0x40 + 2 * i, i == 2 ? 20 : 10);
    Word(ram, 0x80 + 2 * i, uint16_t(100 + i));
  }
  Run(device, ram, 2);
  Check(Word(ram, 0x40) == 20 && Word(ram, 0x80) == 102 && Word(ram, 0x82) == 100 &&
            Word(ram, 0x84) == 101 && Word(ram, 0x86) == 103,
        "Sort is descending and stable across ties");
  Word(ram, 0x24, 0xffff);
  std::fill(ram.begin() + 0xc0, ram.end(), 0xa5);
  const auto before = ram;
  Run(device, ram, 2);
  Check(ram == before, "A negative driver count leaves the complete workspace unchanged");
}

void CommandVectors() {
  constexpr std::array<uint64_t, 8> expected{
      0xdd888df8670c9438, 0x40df59a4891abdec, 0xe5288c38de8764e1, 0x0003a0ef9de7ae97,
      0x8e1ff4b228f3ccaf, 0x49ff504db6b4ab22, 0xdc8dcd87dc5d07c1, 0x5ab8b119fd98d684};
  constexpr std::array<unsigned, 8> counts{2025, 2048, 2048, 2047, 2048, 2048, 2048, 2048};
  for (unsigned command = 1; command <= 8; ++command) {
    uint64_t digest = 14695981039346656037ull;
    unsigned count = 0;
    for (unsigned index = 0; index < st010_test::Cases; ++index) {
      auto ram = st010_test::Input(command, index);
      // Keep table coordinates and squared sums within their signed ranges.
      if (!st010_test::BoundedVector(command, ram))
        continue;
      St010 device;
      Run(device, ram, uint8_t(command));
      digest = st010_test::Digest(digest, ram);
      ++count;
    }
    Check(count == counts[command - 1], "Command vector input coverage");
    if (digest != expected[command - 1]) {
      std::fprintf(stderr, "Command %u digest %016llx\n", command,
                   static_cast<unsigned long long>(digest));
      throw std::runtime_error("Command output vector mismatch");
    }
  }
}

std::vector<uint8_t> Rom(uint8_t size = 10) {
  std::vector<uint8_t> rom(0x100000);
  for (unsigned bank = 0; bank < 32; ++bank)
    std::fill_n(rom.begin() + bank * 0x8000, 0x8000, uint8_t(bank));
  std::fill_n(rom.begin() + 0x7fc0, 64, 0);
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0] = 0xdb;
  rom[0x7fbf] = 1;
  if (size == 9)
    std::copy_n("2DAN MORITA SHOUGI", 18, rom.begin() + 0x7fc0);
  else
    rom[0x7fc0] = 'T';
  rom[0x7fd5] = 0x30;
  rom[0x7fd6] = 0xf6;
  rom[0x7fd7] = size;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffd] = 0x80;
  return rom;
}

void CartridgeMapping() {
  auto image = Rom();
  auto cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
  Check(cart && cart->Header().mapping == MappingType::St010 && cart->SramData().size() == 4096,
        "ST010 detection forces the board layout and RAM capacity");
  CartridgeDatabase database;
  DatabaseOverride override;
  override.forceSramSize = 0;
  database.AddOverride(cart->RomCrc32(), override);
  const auto overridden = Cartridge::FromRomImage(image, nullptr, &database, nullptr);
  Check(overridden && overridden->SramData().size() == 4096,
        "Header overrides cannot remove required device RAM");
  auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Check(emu->LoadCartridge(image), "ST010 is accepted by emulator");
  auto &bus = emu->GetBus();
  for (unsigned bank = 0; bank < 256; ++bank) {
    if (bank == 0x7e || bank == 0x7f)
      continue;
    Check(bus.Read((bank << 16) | 0x8100) == (bank & 31),
          "Shared work RAM does not overlay the upper ROM banks");
  }
  for (unsigned bank : {0x40, 0x5f, 0x60, 0x67, 0x70, 0x7d, 0xc0, 0xe0, 0xe7, 0xf0, 0xff}) {
    const unsigned address = (bank << 16) | 0x4567;
    Check(cart->Read(address, 0x5a) == 0x5a, "Lower unused cartridge windows stay open");
    bus.Write(0x7e0000, 0x5a);
    Check(bus.Read(address) == 0x5a, "Bus leaves lower unused windows unmapped");
  }
  bus.Write(0x680123, 0x69);
  for (unsigned mirror : {0u, 0x80u})
    for (unsigned bank = 0x68; bank <= 0x6f; ++bank)
      for (unsigned page = 0; page < 8; ++page)
        Check(bus.Read(((bank | mirror) << 16) | (page << 12) | 0x123) == 0x69,
              "RAM aliases every 4 KiB in both sets of device banks");
  bus.Write(0x6f7000, 3);
  bus.Write(0x680002, 7);
  bus.Write(0x6fa020, 6); // Outside the device window: ROM write, no command.
  bus.Write(0x6f7020, 6);
  bus.Write(0x687021, 0x80);
  Check(bus.Read(0xe80010) == 42 && bus.Read(0x6f7020) == 6,
        "Mirrored command registers execute through the bus");
  const auto saved = emu->LoadedCartridge()->SramData();
  cart->LoadSram(saved);
  Check(cart->Read(0x680123) == 0x69 && cart->Read(0xef7010) == 42,
        "Save restoration shares device RAM");
  Check(cart->Read(0x680020) == 6, "Save loading restores the shared command byte");
  auto sibling = Rom(9);
  std::string error;
  Check(!emu->LoadCartridge(std::span<const uint8_t>(image).first(4096), &error),
        "Truncated cartridge is rejected");
  Check(bus.Read(0x680123) == 0x69, "Rejected cartridge preserves the loaded machine");
  Check(!emu->LoadCartridge(sibling, &error) && error.find("st011.rom") != std::string::npos,
        "The ST011 sibling requires its own firmware instead of using ST010 commands");
  sibling.resize(sibling.size() + CartridgeFirmware::Size(FirmwareKind::St011), 0);
  Check(emu->LoadCartridge(sibling) &&
            emu->LoadedCartridge()->Header().chip == EnhancementChip::St011 &&
            emu->LoadedCartridge()->HasSignalProcessor() && bus.Read(0x600001) == 0,
        "Sibling firmware selects the ST011 processor interface");
  Check(emu->LoadCartridge(image), "Reload ST010");
  bus.Write(0x680020, 6);
  bus.Write(0x680021, 0x80);
  Check(bus.Read(0x680020) == 6 && bus.Read(0x680010) == 0,
        "Reload clears RAM and accepts a command without an enable latch");
}

void CpuCommandProgram() {
  auto rom = Rom();
  const std::array<uint8_t, 41> program{
      0xa9, 12,   0x8f, 0,    0,    0x68, 0xa9, 13,   0x8f, 2,    0,    0x68, 0xa9, 6,
      0x8f, 0x20, 0,    0x68, 0xa9, 0x80, 0x8f, 0x21, 0,    0x68, 0xaf, 0x10, 0,    0x68,
      0x8f, 0,    0,    0x7e, 0xaf, 0x11, 0,    0x68, 0x8f, 1,    0,    0x7e, 0xdb};
  std::copy(program.begin(), program.end(), rom.begin());
  auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Check(emu->LoadCartridge(rom), "CPU fixture loads");
  emu->StepFrame();
  Check(emu->GetBus().Read(0x7e0000) == 0x38 && emu->GetBus().Read(0x7e0001) == 1,
        "65816 program executes a cartridge math command and reads the shared result");
}
void CpuCommandGuest() {
  constexpr std::array<uint64_t, 16> expected{
      0xbcbf9dd0dbcfc5b0, 0xc13ebb3220439036, 0x43d5f39f20ff994e, 0xc13b86c3ded0ac6f,
      0x50ebba997330de65, 0x5d6bf8e49f254fbc, 0xc66aa8c7fc96ba30, 0x81757206e60b6c91,
      0xf66113df5a966e65, 0x81182a99d89a8cca, 0x7cd32c2e8a21544b, 0xf097439a41414a27,
      0x3500a49f4ad14a9e, 0x90484571f3a70170, 0x10b07d8d0bda7d49, 0x2314441c94713913};
  for (bool pal : {false, true}) {
    auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    Check(machine->LoadCartridge(st010_test::CommandGuest(pal)), "The command-result guest loads");
    for (bool warm : {false, true}) {
      if (warm)
        Check(machine->Reset(), "The command-result guest resets");
      for (unsigned field = 0; field < 80 && machine->GetBus().WramData()[0x120] != 0xa5; ++field)
        machine->StepFrame();
      const auto *wram = machine->GetBus().WramData();
      Check(wram[0x120] == 0xa5, "The command-result guest completes");
      for (unsigned scene = 0; scene < expected.size(); ++scene) {
        Ram output;
        std::copy_n(wram + 0x10000 + scene * 4096, output.size(), output.begin());
        Check(st010_test::Digest(14695981039346656037ull, output) == expected[scene],
              "CPU command output retains every shared RAM byte");
      }
    }
  }
}
} // namespace

int main() {
  try {
    CpuPortGuest();
    CpuCommandGuest();
    RegistersAndMath();
    DriverSorting();
    CommandVectors();
    CartridgeMapping();
    CpuCommandProgram();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("ST010 command, mapping, and shared RAM checks passed");
  return 0;
}
