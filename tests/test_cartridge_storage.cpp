#include "snes/core/Emulator.hpp"

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
                                        nullptr);
    Check(cart && cart->SramData().size() == board.ram, "The board exposes its physical RAM size");
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
    auto machine = std::make_unique<Emulator>();
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
  auto cart = Cartridge::FromSufamiTurbo(bios, slot, slot);
  Check(cart.has_value(), "The generated Sufami Turbo cartridges load");
  Check(cart->SlotSramData(0).size() == 0x4000 && Filled(cart->SlotSramData(0), 255) &&
            Filled(cart->SlotSramData(1), 255),
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
}
void BroadcastStorage() {
  auto bios = Image();
  std::copy_n("Satellaview BS-X     ", 21, bios.begin() + 0x7fc0);
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
    Check(cart && cart->HasSignalProcessor() && cart->SramData().size() == 4096,
          "SETA save RAM belongs to its signal processor");
    RetainedSave(*cart, 0);
  }
  auto machine = std::make_unique<Emulator>();
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
  Check(Filled(cart->SramData(), 0), "Reset clears ST010 command work RAM");
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
    std::puts("Cartridge storage tests passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
