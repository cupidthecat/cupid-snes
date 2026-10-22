#include "ConsoleConfig.hpp"
#include "cartridge/Sa1DmaFixture.hpp"
#include "cartridge/Sa1VectorFixture.hpp"

#include <algorithm>
#include <array>
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
bool Filled(std::span<const uint8_t> bytes, uint8_t value) {
  return std::all_of(bytes.begin(), bytes.end(), [=](uint8_t byte) { return byte == value; });
}
std::vector<uint8_t> Image(uint8_t mode = 0x20, uint8_t type = 2, size_t size = 0x100000) {
  std::vector<uint8_t> bytes(size, 0);
  const size_t header = (mode & 15) == 5                               ? 0x40ffc0
                        : mode == 0x3a || ((mode & 1) && mode != 0x23) ? 0xffc0
                                                                       : 0x7fc0;
  std::fill_n(bytes.begin() + header, 21, ' ');
  bytes[header] = 'T';
  bytes[header + 0x15] = mode;
  bytes[header + 0x16] = type;
  bytes[header - 1] = type == 0xf3 ? 0x10 : type == 0xf6 ? 1 : type == 0xf5 && mode == 0x30 ? 2 : 0;
  bytes[header + 0x17] = 10;
  bytes[header + 0x18] = (type >> 4) == 1 || type == 0xf3 || type == 0xf6 ? 0 : 3;
  bytes[header + 0x19] = 1;
  bytes[header + 0x1c] = bytes[header + 0x1d] = 255;
  bytes[header + 0x3d] = 0x80;
  const size_t entry = header & ~size_t(0x7fff);
  bytes[entry] = 0x80;
  bytes[entry + 1] = 0xfe;
  return bytes;
}
void RetainedSave(Cartridge &cart, uint8_t coldValue) {
  const auto size = cart.SramData().size();
  Check(size != 0 && Filled(cart.SramData(), coldValue),
        "Fresh cartridge RAM has its device default");
  const std::array<uint8_t, 3> prefix{0, 0x39, 0x72};
  cart.LoadSram(prefix);
  Check(std::equal(prefix.begin(), prefix.end(), cart.SramData().begin()) &&
            Filled(cart.SramData().subspan(prefix.size()), coldValue),
        "A partial save replaces only its supplied bytes");
  cart.LoadSram({});
  cart.Reset();
  Check(std::equal(prefix.begin(), prefix.end(), cart.SramData().begin()) &&
            Filled(cart.SramData().subspan(prefix.size()), coldValue),
        "Empty loads and reset retain cartridge save bytes");
  const std::vector<uint8_t> oversized(size + 7, 0x51);
  cart.LoadSram(oversized);
  cart.Reset();
  Check(cart.SramData().size() == size && Filled(cart.SramData(), 0x51),
        "Oversized saves truncate without changing the storage allocation");
}
void BoardDefaults() {
  CartridgeFirmware firmware;
  Check(firmware.Set(FirmwareKind::St018,
                     std::vector<uint8_t>(CartridgeFirmware::Size(FirmwareKind::St018))),
        "The ARM storage fixture provides a complete zero-operation firmware image");
  struct Board {
    uint8_t mode, type;
    size_t size, ram;
  };
  for (const auto board : {Board{0x20, 2, 0x100000, 8192}, Board{0x21, 2, 0x100000, 8192},
                           Board{0x35, 2, 0x500000, 8192}, Board{0x23, 0x35, 0x100000, 8192},
                           Board{0x20, 0x15, 0x100000, 32768}, Board{0x30, 0xf5, 0x100000, 8192},
                           Board{0x20, 3, 0x100000, 8192}, Board{0x20, 5, 0x100000, 8192},
                           Board{0x20, 0xf3, 0x100000, 0}, Board{0x32, 0x43, 0x100000, 8192},
                           Board{0x35, 0x55, 0x500000, 8192}, Board{0x3a, 0xf5, 0x300000, 8192},
                           Board{0x30, 0x25, 0x100000, 8192}}) {
    auto cart = Cartridge::FromRomImage(Image(board.mode, board.type, board.size), nullptr, nullptr,
                                        nullptr, &firmware);
    Check(cart && cart->SramData().size() == board.ram && cart->SramLoadSize() == board.ram,
          "The board exposes its physical RAM size for save input and output");
    if (board.ram)
      RetainedSave(*cart, 255);
    else
      Check(cart->BoardConnected() && cart->Read(0x008000) == 0x80,
            "A graphics board without save RAM retains its ROM wiring");
  }
  auto image = Image();
  image[0x7fd8] = 0;
  auto cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
  Check(cart && cart->SramData().empty(), "A cartridge without RAM creates no storage");
}
void CpuLoadResetReload() {
  for (bool hi : {false, true}) {
    auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    const auto image = Image(hi ? 0x21 : 0x20);
    Check(machine->LoadCartridge(image), "A generated save cartridge loads");
    auto &bus = machine->GetBus();
    const uint32_t address = hi ? 0x206123 : 0x700123;
    Check(bus.Read(address) == 255, "The CPU sees erased RAM before its first instruction");
    bus.Write(address, 0x62);
    bus.Write(0x7e0123, 0x49);
    Check(machine->Reset() && bus.Read(address) == 0x62 && bus.Read(0x7e0123) == 0x49,
          "Console reset retains save RAM and work RAM");
    Check(machine->LoadCartridge(image) && bus.Read(address) == 255,
          "Reloading a cartridge without a save creates erased RAM again");
  }
}
void SufamiStorage() {
  auto bios = Image(0x20, 0, 0x40000);
  std::copy_n("BANDAI SFC-ADX", 14, bios.begin());
  std::copy_n("SFC-ADX BACKUP", 14, bios.begin() + 0x10);
  std::vector<uint8_t> slot(0x80000);
  std::copy_n("BANDAI SFC-ADX", 14, slot.begin());
  slot[0x37] = 4;
  auto cart = Cartridge::FromSufamiTurbo(bios, slot, slot);
  Check(cart.has_value(), "The generated Sufami Turbo cartridges load");
  Check(cart->SlotSramData(0).size() == 0x2000 && cart->SlotSramData(1).size() == 0x2000 &&
            Filled(cart->SlotSramData(0), 255) && Filled(cart->SlotSramData(1), 255),
        "Both Sufami Turbo slots start erased");
  const std::array<uint8_t, 2> prefix{0x34, 0};
  cart->LoadSlotSram(0, prefix);
  cart->Reset();
  Check(cart->SlotSramData(0)[0] == 0x34 && cart->SlotSramData(0)[1] == 0 &&
            Filled(cart->SlotSramData(0).subspan(2), 255) && Filled(cart->SlotSramData(1), 255),
        "A partial slot save and reset preserve the other slot");
  cart = Cartridge::FromSufamiTurbo(bios, {}, slot);
  Check(cart && cart->SlotSramData(0).empty() && Filled(cart->SlotSramData(1), 255),
        "An absent slot exposes no save while the occupied slot starts erased");
  for (unsigned units : {0u, 1u, 3u, 8u, 32u, 255u}) {
    auto other = slot;
    other[0x37] = static_cast<uint8_t>(units);
    cart = Cartridge::FromSufamiTurbo(bios, other, slot);
    Check(cart && cart->SlotSramData(0).size() == units * 0x800 &&
              cart->SlotSramData(1).size() == 0x2000 &&
              cart->SramData().size() == units * 0x800 + 0x2000,
          "Sufami RAM size comes from each slot header in 2 KiB units");
    const std::array<uint8_t, 1> save{0x68};
    cart->LoadSlotSram(1, save);
    cart->Reset();
    Check(cart->SlotSramData(1)[0] == 0x68 && Filled(cart->SlotSramData(0), 255),
          "Different slot capacities keep battery storage independent after reset");
  }
  slot.resize(0x20000);
  cart = Cartridge::FromSufamiTurbo(bios, slot, {});
  Check(cart && cart->SlotSramData(0).size() == 0x2000,
        "A 128 KiB Sufami Turbo game keeps its declared RAM");
}
void BroadcastStorage() {
  auto bios = Image();
  std::copy_n("Satellaview BS-X     ", 21, bios.begin() + 0x7fc0);
  std::copy_n("ZBSJ", 4, bios.begin() + 0x7fb2);
  bios[0x7fda] = 0x33;
  bios[0x7fd8] = 5;
  auto cart = Cartridge::FromBroadcastCartridge(bios, {});
  Check(cart && cart->Header().mapping == MappingType::Bsx && cart->SramData().size() == 0x8000,
        "The broadcast base cartridge exposes its save storage");
  RetainedSave(*cart, 255);
}
void ProcessorOwnedMemory() {
  CartridgeFirmware firmware;
  const std::vector<uint8_t> bytes(0xd000);
  Check(firmware.Set(FirmwareKind::St010, bytes) && firmware.Set(FirmwareKind::St011, bytes),
        "Generated signal firmware loads");
  for (uint8_t shift : {9, 10}) {
    auto image = Image(0x30, 0xf6);
    image[0x7fd7] = shift;
    if (shift == 9)
      std::copy_n("2DAN MORITA SHOUGI", 18, image.begin() + 0x7fc0);
    auto cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr, &firmware);
    Check(cart && cart->HasSignalProcessor() && cart->SramData().size() == 4096 &&
              cart->SramLoadSize() == 4096,
          "SETA save RAM belongs to its signal processor");
    RetainedSave(*cart, 0);
  }
  auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Check(machine->LoadCartridge(Image(0x23, 0x35)), "The generated SA-1 cartridge loads");
  auto &bus = machine->GetBus();
  Check(bus.Read(0x003123) == 0, "SA-1 internal RAM starts cleared");
  bus.Write(0x2229, 255);
  bus.Write(0x003123, 0x59);
  Check(bus.Read(0x003123) == 0x59 && machine->Reset() && bus.Read(0x003123) == 0,
        "Console reset clears SA-1 internal RAM");
}
void CommandMemoryReset() {
  auto cart = Cartridge::FromRomImage(Image(0x30, 0xf6), nullptr, nullptr, nullptr);
  Check(cart && !cart->HasSignalProcessor() && Filled(cart->SramData(), 0),
        "The ST010 command device starts with cleared work RAM");
  cart->Write(0x680123, 0x57);
  Check(cart->Read(0x680123) == 0x57, "The command device writes its shared work RAM");
  cart->Reset();
  Check(cart->Read(0x680123) == 0 && Filled(cart->SramData(), 0),
        "Reset clears ST010 command work RAM and retains its cleared battery image");
}

void Sa1RomPortDma() {
  struct Scene {
    uint32_t source;
    uint16_t count;
    uint8_t mapping;
    uint8_t first;
    uint8_t second;
    uint8_t vectorPattern = 0;
    bool bwram = false;
  };
  for (const auto scene : {Scene{0x000100, 1, 0, 0x31, 0x31},
                           {0x006100, 1, 0, 0x31, 0x31},
                           {0x400100, 1, 0, 0x31, 0x31},
                           {0x800100, 1, 0, 0x31, 0x31},
                           {0x000100, 1, 0x84, 0x83, 0x83},
                           {0x008100, 1, 0, 0x31, 0x31},
                           {0xc00100, 1, 0, 0x31, 0x31},
                           {0xffff80, 256, 0, 0x54, 0x31},
                           {0x3f7f80, 256, 0, 0x54, 0xb4},
                           {0x00ffea, 6, 0, 0, 0, 1},
                           {0x007fea, 6, 0, 0, 0, 1},
                           {0x00fffa, 6, 0, 0, 0, 2},
                           {0x000100, 1, 0, 0x31, 0x31, 0, true},
                           {0x00ffea, 6, 0, 0, 0, 1, true},
                           {0xffff80, 256, 0, 0x54, 0x31, 0, true}})
    for (bool pal : {false, true})
      for (bool contention : {false, true}) {
        Emulator::Config config;
        config.entropyMode = EntropyMode::None;
        config.entropySeed = 0;
        config.sa1BusContention = contention;
        auto machine = std::make_unique<Emulator>(config);
        const auto fixture = sa1_test::RomPortDma(scene.source, scene.count, scene.mapping, pal,
                                                  scene.vectorPattern != 0, scene.bwram);
        Check(machine->LoadBroadcastCartridge(fixture.rom, fixture.pack),
              "The ROM-port DMA guest loads through the cartridge slot");
        for (bool warm : {false, true}) {
          if (warm)
            Check(machine->Reset(), "The ROM-port DMA guest resets normally");
          machine->StepFrame();
          const auto *ram = machine->GetBus().WramData();
          Check(ram[0x120] == 0xa5, "The cartridge DMA completes and the console reads its result");
          constexpr std::array<uint8_t, 6> nativeVectors{0x56, 0x78, 0, 0, 0x9a, 0xbc};
          constexpr std::array<uint8_t, 6> emulationVectors{0, 0, 0, 0xa0, 0, 0};
          for (unsigned byte = 0; byte < scene.count; ++byte) {
            const auto expected = scene.vectorPattern == 1   ? nativeVectors[byte]
                                  : scene.vectorPattern == 2 ? emulationVectors[byte]
                                  : byte < 128               ? scene.first
                                                             : scene.second;
            Check(ram[0x200 + byte] == expected,
                  "ROM-source DMA uses its dedicated port across CPU windows and bank boundaries");
          }
        }
      }
}
void Sa1VectorReads() {
  for (uint8_t switches : {0, 0x10, 0x40, 0x50})
    for (bool pal : {false, true})
      for (bool contention : {false, true})
        for (bool bitstream : {false, true}) {
          Emulator::Config config;
          config.entropyMode = EntropyMode::None;
          config.entropySeed = 0;
          config.sa1BusContention = contention;
          auto machine = std::make_unique<Emulator>(config);
          const auto rom = sa1_test::VectorReads(switches, pal, bitstream);
          Check(machine->LoadCartridge(rom),
                "The vector-read guest loads as an ordinary cartridge");
          for (bool warm : {false, true}) {
            if (warm)
              Check(machine->Reset(), "The vector-read guest resets normally");
            machine->StepFrame();
            const auto *ram = machine->GetBus().WramData();
            Check(ram[0x120] == 0xa5, "Both processors complete their actual vector-table reads");
            for (unsigned table = 0; table < 3; ++table)
              for (unsigned byte = 0; byte < 32; ++byte) {
                uint8_t expected = rom[(table == 1 ? 0x207fe0 : 0x7fe0) + byte];
                if (table == 0 && (switches & 0x10) && (byte == 10 || byte == 11))
                  expected = byte == 10 ? 0x56 : 0x78;
                if (table == 0 && (switches & 0x40) && (byte == 14 || byte == 15))
                  expected = byte == 14 ? 0x9a : 0xbc;
                Check(ram[0x200 + table * 32 + byte] == expected,
                      "Console ROM reads substitute only selected bank-zero native vectors");
                Check(
                    ram[0x300 + table * 32 + byte] == expected,
                    "Cartridge processor and bitstream reads see the selected native ROM vectors");
              }
          }
        }
}
} // namespace
int main() {
  try {
    BoardDefaults();
    CpuLoadResetReload();
    SufamiStorage();
    BroadcastStorage();
    ProcessorOwnedMemory();
    CommandMemoryReset();
    Sa1RomPortDma();
    Sa1VectorReads();
    std::puts("Cartridge storage tests passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
