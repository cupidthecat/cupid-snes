#include "snes/core/Emulator.hpp"
#include "snes/core/NecDsp.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace snes::core;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

uint32_t Load(uint16_t value, unsigned destination) {
  return 0xc00000 | (uint32_t(value) << 6) | destination;
}

std::vector<uint8_t> Firmware(FirmwareKind kind, uint8_t tag = 0x55) {
  std::vector<uint8_t> bytes(CartridgeFirmware::Size(kind));
  const uint32_t words[]{Load(0x400, 7), Load(tag, 6),  0x800000 | (0xbe << 13) | (2 << 2),
                         9 << 4 | 3,     Load(0xa5, 6), 0x800000 | (0x100 << 13) | (5 << 2)};
  for (size_t i = 0; i < std::size(words); ++i)
    for (unsigned byte = 0; byte < 3; ++byte)
      bytes[i * 3 + byte] = static_cast<uint8_t>(words[i] >> (byte * 8));
  return bytes;
}

void ClockAndHandshake() {
  auto chip = std::make_unique<NecDsp>(FirmwareKind::Dsp1, NecDsp::Board::LoRom);
  Check(chip->LoadFirmware(Firmware(FirmwareKind::Dsp1)), "The bridge accepts a complete program");
  Check(chip->Read(0x30c000) == 0 && chip->Cycles() == 0, "Port reads do not execute firmware");
  chip->Advance(1);
  Check(chip->Cycles() == 1 && chip->Read(0x30c000) == 4,
        "The first elapsed master clock starts the processor");
  chip->Advance(1);
  Check(chip->Cycles() == 1, "Fractional chip clocks persist between host operations");
  chip->Advance(1);
  Check(chip->Cycles() == 2 && chip->Read(0x30c000) == 0x84,
        "Firmware raises ready after its data-register instruction");
  chip->Write(0x30c000, 0);
  Check(chip->Read(0x30c000) == 0x84, "Status writes do not acknowledge a data request");
  Check(chip->Read(0x308000) == 0x55 && chip->Read(0x30c000) == 4,
        "Reading the data port acknowledges the byte request");
  chip->Advance(9);
  Check(chip->Read(0x30c000) == 0x84 && chip->Read(0x308000) == 0xa5,
        "The firmware wait loop resumes only after acknowledgement");
}

void ClockDomains() {
  for (bool pal : {false, true}) {
    for (const auto kind : {FirmwareKind::Dsp1, FirmwareKind::St010, FirmwareKind::St011}) {
      const bool seta = kind != FirmwareKind::Dsp1;
      auto chip = std::make_unique<NecDsp>(kind, seta ? NecDsp::Board::Seta : NecDsp::Board::LoRom);
      Check(chip->LoadFirmware(std::vector<uint8_t>(CartridgeFirmware::Size(kind))),
            "Clock fixture loads zero-operation firmware");
      chip->SetPal(pal);
      const uint64_t frequency = !seta                         ? 7'600'000
                                 : kind == FirmwareKind::St010 ? 11'000'000
                                                               : 15'000'000;
      const uint64_t master = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
      uint64_t elapsed = 0;
      for (unsigned repeat = 0; repeat < 3; ++repeat) {
        for (uint32_t clocks : {0u, 1u, 2u, 7u, 31u, 357368u}) {
          elapsed += clocks;
          chip->Advance(clocks);
          Check(chip->Cycles() == (elapsed * frequency + master - 1) / master,
                "Every board tracks its oscillator through both console clock domains");
        }
      }
      chip->Reset();
      Check(chip->Cycles() == 0, "Reset restarts the fractional clock origin");
      chip->Advance(1);
      Check(chip->Cycles() == 1, "The first post-reset master clock starts a fresh instruction");
    }
  }
}

void BoardAddressLines() {
  using Board = NecDsp::Board;
  for (const auto board :
       {Board::LoRom, Board::LoRomSmallRam, Board::LoRomLargeRam, Board::HiRom, Board::Seta}) {
    auto chip = std::make_unique<NecDsp>(
        board == Board::Seta ? FirmwareKind::St010 : FirmwareKind::Dsp1, board);
    auto bus = std::make_unique<MemoryBus>();
    bus->Reset();
    bus->MapWram();
    const auto slot = bus->RegisterHandler([](uint32_t, uint8_t) { return uint8_t{0x3c}; },
                                           [](uint32_t, uint8_t) {});
    chip->MapCpu(*bus, slot);
    for (unsigned bank = 0; bank < 256; ++bank) {
      for (unsigned offset : {0u, 1u, 0x1000u, 0x3fffu, 0x4000u, 0x6000u, 0x7fffu, 0x8000u, 0xbfffu,
                              0xc000u, 0xffffu}) {
        const uint32_t address = (bank << 16) | offset;
        if (bank == 0x7e || bank == 0x7f)
          continue;
        if (!(bank & 0x40) && offset < 0x2000) {
          Check(bus->ReadCpu(address) == bus->WramData()[offset],
                "Cartridge maps preserve the low WRAM mirrors");
          continue;
        }
        const bool mapped =
            chip->SelectsDevice(address) || chip->RomAddress(address) || chip->SramAddress(address);
        bus->SetOpenBus(0x97);
        Check(bus->ReadCpu(address) == (mapped ? 0x3c : 0x97),
              "The bus map and physical board address lines select the same windows");
      }
    }
    bus->Write(0x7e1234, 0xa7);
    Check(bus->WramData()[0x1234] == 0xa7, "Cartridge maps preserve the full WRAM banks");
    if (board == Board::LoRom) {
      Check(!chip->SelectsDevice(0x208000) && chip->SelectsDevice(0xb08000) &&
                !chip->RomAddress(0x208000),
            "RAM-less boards expose DSP ports in banks $30-$3F");
    }
    if (board == Board::LoRomSmallRam)
      Check(chip->SramAddress(0x708123) == 0x8123 && chip->SramAddress(0xf18123) == 0x18123,
            "Small RAM boards retain the full SRAM bank address");
    if (board == Board::LoRomLargeRam)
      Check(chip->SramAddress(0x710123) == 0x8123 && !chip->SramAddress(0x718123),
            "Large RAM boards select SRAM below $8000");
    if (board == Board::HiRom || board == Board::Seta)
      Check(chip->RomAddress(0xfe8000) && chip->RomAddress(0xff8000),
            "High ROM mirrors retain banks $FE and $FF");
    if (board == Board::Seta) {
      chip->Write(0xe81abc, 0x72);
      Check(chip->Read(0x680abc) == 0x72 && chip->SaveRam()[0xabc] == 0x72,
            "SETA RAM repeats through its mirrored CPU windows and save bytes");
      chip->Reset();
      Check(chip->Read(0x6f7abc) == 0x72, "SETA reset retains physical RAM");
    }
  }
}

std::vector<uint8_t> Rom(uint8_t mode, uint8_t type, uint8_t maker, bool st011, bool original) {
  std::vector<uint8_t> bytes(0x100000);
  const size_t header = mode == 0x21 ? 0xffc0 : 0x7fc0;
  std::fill_n(bytes.begin() + header, 21, ' ');
  const std::string title = original                    ? "PILOTWINGS"
                            : st011                     ? "2DAN MORITA SHOUGI"
                            : type == 0xf6              ? "F1 ROC II"
                            : mode == 0x20 && type == 5 ? "DUNGEON MASTER"
                            : mode == 0x30 && type == 5 ? "SD\xb6\xde\xdd\xc0\xde\xd1GX"
                            : mode == 0x30 && type == 3 ? "TOP GEAR 3000"
                                                        : "TEST DSP";
  std::copy(title.begin(), title.end(), bytes.begin() + header);
  bytes[header - 1] = type == 0xf6 ? 1 : 0;
  bytes[header + 0x15] = mode;
  bytes[header + 0x16] = type;
  bytes[header + 0x17] = st011 ? 9 : 10;
  bytes[header + 0x19] = 1;
  bytes[header + 0x1a] = maker;
  bytes[header + 0x1c] = bytes[header + 0x1d] = 0xff;
  bytes[header + 0x3d] = 0x80;
  const size_t entry = mode == 0x21 ? 0x8000 : 0;
  bytes[entry] = 0x78;
  bytes[entry + 1] = 0xdb;
  return bytes;
}

void ConsoleIntegration() {
  auto firmware = std::make_shared<CartridgeFirmware>();
  for (unsigned i = 0; i < static_cast<unsigned>(FirmwareKind::Count); ++i) {
    const auto kind = static_cast<FirmwareKind>(i);
    Check(firmware->Set(kind, Firmware(kind, static_cast<uint8_t>(0x30 + i))),
          "The firmware catalog stores each processor program independently");
  }
  struct Case {
    uint8_t mode, type, maker;
    bool st011, original;
    FirmwareKind kind;
  };
  for (const auto test : {Case{0x20, 3, 0, false, true, FirmwareKind::Dsp1},
                          Case{0x20, 3, 0, false, false, FirmwareKind::Dsp1b},
                          Case{0x21, 5, 0, false, false, FirmwareKind::Dsp1b},
                          Case{0x20, 5, 0, false, false, FirmwareKind::Dsp2},
                          Case{0x30, 5, 0xb2, false, false, FirmwareKind::Dsp3},
                          Case{0x30, 3, 0, false, false, FirmwareKind::Dsp4},
                          Case{0x30, 0xf6, 0, false, false, FirmwareKind::St010},
                          Case{0x30, 0xf6, 0, true, false, FirmwareKind::St011}}) {
    Emulator::Config config;
    config.cartridgeFirmware = firmware;
    auto machine = std::make_unique<Emulator>(config);
    Check(
        machine->LoadCartridge(Rom(test.mode, test.type, test.maker, test.st011, test.original)) &&
            machine->LoadedCartridge()->HasSignalProcessor(),
        "The console selects a clocked processor from the cartridge and firmware inputs");
    machine->StepFrame();
    const bool seta = test.type == 0xf6, hiRom = test.mode == 0x21;
    const uint32_t dataPort = seta ? 0x600000 : hiRom ? 0x006000 : 0x308000;
    const uint32_t statusPort = seta ? 0x600001 : hiRom ? 0x007000 : 0x30c000;
    auto &bus = machine->GetBus();
    Check(bus.Read(statusPort) == 0x84 &&
              bus.Read(dataPort) == 0x30 + static_cast<unsigned>(test.kind),
          "Firmware selection, execution and host ports reach the console bus");
    if (seta) {
      bus.Write(0x680123, 0x5a);
      Check(machine->LoadedCartridge()->SramData().size() == 4096 &&
                machine->LoadedCartridge()->SramData()[0x123] == 0x5a,
            "The battery save API reads the processor's physical RAM");
    }
    Check(machine->Reset() && bus.Read(statusPort) == 0,
          "Console reset clears the processor registers before resuming firmware");
    if (seta)
      Check(machine->LoadedCartridge()->SramData()[0x123] == 0x5a,
            "Console reset preserves SETA battery RAM");
    machine->StepFrame();
    Check(bus.Read(statusPort) == 0x84, "Firmware restarts after a whole-console reset");
  }
}
} // namespace

int main() {
  try {
    ClockAndHandshake();
    ClockDomains();
    BoardAddressLines();
    ConsoleIntegration();
    std::puts("Clocked cartridge DSP integration tests passed");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
