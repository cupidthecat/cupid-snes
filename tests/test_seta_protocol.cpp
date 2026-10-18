#include "ConsoleConfig.hpp"
#include "snes/core/SetaProtocol.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace snes::core;
namespace {
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

void BoardProtocol() {
  std::vector<uint8_t> ram(4096, 0x55);
  St011 chip(ram);
  Check(chip.Read(1) == 0xff && chip.Read(0x101) == 0x55,
        "ST011 status does not replace ordinary RAM");
  chip.Write(0, 1);
  for (unsigned i = 0; i < 127; ++i)
    chip.Write(0, uint8_t(i));
  Check(chip.Board()[80] == 0, "Board packet remains incomplete through byte 127");
  chip.Write(0, 127);
  for (unsigned row = 0; row < 9; ++row)
    for (unsigned col = 0; col < 9; ++col)
      Check(chip.Board()[row * 9 + col] == row * 10 + col,
            "Board rows discard each wire-format padding byte");
  chip.Write(0x12c, 0x92);
  chip.Write(0x12d, 0x93);
  chip.Write(0x12e, 0x94);
  chip.Write(0, 4);
  Check(chip.Read(0x12c) == 0 && chip.Read(0x12d) == 0x93 && chip.Read(0x12e) == 0,
        "Observed command 4 clears only its two result bytes");
  chip.Write(0, 2);
  for (uint8_t byte : {4, 5, 6, 14})
    chip.Write(0, byte);
  Check(chip.Read(0x12d) == 0x93,
        "Command 2 consumes four parameters without interpreting them as commands");
  chip.Write(0, 14);
  Check(chip.Read(0x12d) == 0, "A completed packet accepts the next command");
  chip.Write(0x1002, 0x87);
  Check(chip.Read(0x1002) == 0x87 && ram[2] == 0x55,
        "Extra command work RAM does not alias saved RAM");
  chip.Write(0, 1);
  chip.Write(0, 99);
  chip.Reset();
  chip.Write(0, 5);
  Check(chip.Board()[0] == 0 && chip.Read(0x1002) == 0 && ram[2] == 0x55,
        "Reset cancels partial packets and preserves battery storage");
}

void StartupProtocol() {
  std::vector<uint8_t> ram(4096, 0x67);
  St018 chip(ram);
  for (uint8_t middle : {uint8_t(1), uint8_t(255)}) {
    chip.Write(0x3804, 0);
    chip.Write(0x3804, middle);
    Check(chip.PendingOutput() == 0, "ST018 waits for all three command bytes");
    chip.Write(0x3804, 0);
    Check(chip.PendingOutput() == 2, "Recognized startup command produces two response bytes");
    Check(chip.Read(0x3804) == 0x81 && chip.Read(0x3804) == 0x81 && chip.PendingOutput() == 0,
          "Initial handshake response is drained in order");
    for (unsigned pass = 0; pass < 2; ++pass) {
      chip.Write(0x3802, uint8_t(pass));
      Check(chip.PendingOutput() == 3 && chip.Read(0x3800) == 0,
            "Each startup parameter produces three replies and ready status");
      for (unsigned byte = 0; byte < 3; ++byte)
        Check(chip.Read(0x3804) == 0x81, "Startup response byte");
    }
    Check(chip.PendingOutput() == 0 && chip.Read(0x3804) == 0x81,
          "Empty output port returns its observed idle value");
  }
  Check(std::all_of(ram.begin(), ram.end(), [](uint8_t byte) { return byte == 0x67; }),
        "Startup I/O cannot overwrite battery RAM through address wrapping");
  chip.Write(0x3804, 0xff);
  chip.Reset();
  chip.Write(0x3804, 0);
  chip.Write(0x3804, 1);
  chip.Write(0x3804, 0);
  Check(chip.PendingOutput() == 2, "Reset discards an incomplete startup command");
}

std::vector<uint8_t> Image(uint8_t type) {
  std::vector<uint8_t> rom(0x80000, 0xea);
  std::fill_n(rom.begin() + 0x7fc0, 64, 0);
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  const std::string_view title = type == 0xf6 ? "2DAN MORITA SHOUGI" : "T";
  std::copy(title.begin(), title.end(), rom.begin() + 0x7fc0);
  rom[0x7fbf] = type == 0xf6 ? 1 : 2;
  rom[0x7fd5] = 0x30;
  rom[0x7fd6] = type;
  rom[0x7fd7] = 9;
  rom[0x7fd8] = type == 0xf6 ? 0 : 3;
  rom[0x7fd9] = 1;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffd] = 0x80;
  return rom;
}

void CartridgeMaps() {
  auto firmware = std::make_shared<CartridgeFirmware>();
  std::vector<uint8_t> dsp(CartridgeFirmware::Size(FirmwareKind::St011));
  const uint32_t instructions[]{0xc00000 | (0x400 << 6) | 7, 0xc00000 | (0x6a << 6) | 6};
  for (unsigned i = 0; i < std::size(instructions); ++i)
    for (unsigned byte = 0; byte < 3; ++byte)
      dsp[i * 3 + byte] = uint8_t(instructions[i] >> (byte * 8));
  std::vector<uint8_t> arm(CartridgeFirmware::Size(FirmwareKind::St018));
  const uint32_t program[]{0xe3a00101, 0xe3a0102a, 0xe5801000, 0xe5801010, 0xeafffffe};
  for (unsigned i = 0; i < std::size(program); ++i)
    for (unsigned byte = 0; byte < 4; ++byte)
      arm[i * 4 + byte] = uint8_t(program[i] >> (byte * 8));
  Check(firmware->Set(FirmwareKind::St011, dsp) && firmware->Set(FirmwareKind::St018, arm),
        "Generated programs supply both cartridge processors");
  auto config = snes::tests::DeterministicConsoleConfig();
  config.cartridgeFirmware = firmware;
  auto machine = std::make_unique<Emulator>(config);
  auto image = Image(0xf6);
  image[0] = 0xdb;
  Check(machine->LoadCartridge(image), "ST011 loads its clocked processor interface");
  auto &bus = machine->GetBus();
  Check(machine->LoadedCartridge()->Header().chip == EnhancementChip::St011 &&
            machine->LoadedCartridge()->Header().mapping == MappingType::St011,
        "ST011 keeps a distinct board type");
  bus.Write(0x68012c, 0x21);
  Check(bus.Read(0xe8012c) == 0x21 && bus.Read(0x68112c) == 0x21,
        "ST011 processor RAM shares its physical byte across CPU mirrors");
  bus.Write(0x680002, 0x64);
  bus.Write(0x601002, 0x31);
  Check(bus.Read(0x680002) == 0x64, "ST011 host data writes do not alias processor RAM");
  machine->StepFrame();
  Check(bus.Read(0x600001) == 0x84 && bus.Read(0xe00000) == 0x6a,
        "Firmware controls the mirrored ST011 status and data ports");
  Check(machine->LoadedCartridge()->SramData().size() == 4096,
        "ST011 persists its physical 4 KiB processor RAM");
  image = Image(0xf5);
  image[0] = 0xdb;
  Check(machine->LoadCartridge(image), "ST018 loads its clocked ARM interface");
  bus.Write(0x700802, 0x76);
  Check(bus.Read(0x003804) == 0, "The ST018 processor starts before its boot delay");
  machine->StepFrame();
  Check(bus.Read(0x803804) == 0x85 && bus.Read(0xbf3800) == 0x2a,
        "ST018 firmware status and data ports mirror through system banks");
  bus.Write(0x003802, 0);
  Check(bus.Read(0x700802) == 0x76 && bus.Read(0x008001) == 0xea,
        "ST018 port writes preserve saved data and LoROM mapping");
}
} // namespace
int main() {
  try {
    BoardProtocol();
    StartupProtocol();
    CartridgeMaps();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("SETA command fixtures and clocked cartridge mapping checks passed");
}
