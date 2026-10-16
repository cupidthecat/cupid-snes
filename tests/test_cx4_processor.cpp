#include "snes/core/Cartridge.hpp"
#include "snes/core/Emulator.hpp"
#include "snes/core/Timing.hpp"

#include <algorithm>
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
  std::vector<uint8_t> image(0x80c00, 0);
  std::fill_n(image.begin(), 0x80000, 0xea);
  std::fill_n(image.begin() + 0x7fc0, 64, 0);
  image[0x7fc0] = 'T';
  image[0x7fd5] = 0x20;
  image[0x7fd6] = 0xf3;
  image[0x7fd7] = 9;
  image[0x7fd9] = 1;
  image[0x7fdc] = image[0x7fdd] = 0xff;
  image[0x7ffd] = 0x80;
  const uint16_t program[]{0x642a, 0xe060, 0xfc00};
  for (unsigned i = 0; i < std::size(program); ++i) {
    image[0x1004 + i * 2] = uint8_t(program[i]);
    image[0x1005 + i * 2] = uint8_t(program[i] >> 8);
  }
  return image;
}
void ProgramExecution() {
  for (uint32_t start : {0x007f4fu, 0x006f4fu}) {
    auto cart = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
    Check(cart.has_value(), "The generated Cx4 image loads");
    cart->Write(0x7f49, 0);
    cart->Write(0x7f4a, 0x90);
    cart->Write(0x7f4b, 0);
    cart->Write(start, 2);
    Check(cart->Read(0x7f5e) == 0x40, "The start register runs the Cx4 program");
    cart->AdvanceHardware(10000);
    Check(cart->Read(0x7f80) == 0x2a, "The processor loads and publishes a register result");
    Check(cart->Read(0x7f5e) == 2 && cart->CpuIrqPending(),
          "Program halt sets the interrupt flag and the CPU IRQ line");
    cart->Write(0x7f5e, 0);
    Check(cart->Read(0x7f5e) == 0 && cart->CpuIrqPending(),
          "Acknowledging the processor flag leaves the CPU IRQ line asserted");
    cart->Write(0x7f51, 1);
    Check(!cart->CpuIrqPending(), "Disabling processor IRQ clears the CPU IRQ line");
  }
}
void DmaLengthLatch() {
  auto cart = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
  cart->Write(0x7f46, 0x60);
  cart->Write(0x7f44, 0x41);
  cart->Write(0x7f47, 0x91);
  cart->AdvanceHardware(1);
  Check(cart->Read(0x7f46) == 0,
        "DMA continues to its latched length after overwriting its own length registers");
}
void MappingAndRomOwnership() {
  auto image = Image();
  image[0x7fd8] = 3;
  auto machine = std::make_unique<Emulator>();
  Check(machine->LoadCartridge(image) && machine->LoadedCartridge()->HasCx4Processor(),
        "The system loader selects processor execution with appended data firmware");
  auto &bus = machine->GetBus();
  bus.Write(0x006017, 0x5a);
  Check(bus.Read(0x807017) == 0x5a, "Data RAM mirrors across offset and bank windows");
  bus.Write(0x006c00, 0xa5);
  Check(bus.Read(0x007c00) == 0, "Unused I/O locations do not become extra work RAM");
  bus.Write(0x700123, 0x66);
  Check(bus.Read(0x770123) == 0x66 && machine->LoadedCartridge()->SramData().size() == 8192,
        "The save window mirrors the declared battery RAM");
  bus.Write(0x7e0000, 0x43);
  Check(bus.Read(0x408000) == 0x43 && bus.Read(0x708000) == 0x43 && bus.Read(0xc08000) == 0x43 &&
            bus.Read(0xff8000) == 0x43,
        "Unmapped cartridge banks and the upper save-bank half preserve open bus");
  auto dma = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
  dma->Write(0x7f40, 0x00);
  dma->Write(0x7f41, 0x80);
  dma->Write(0x7f42, 0xc0);
  dma->Write(0x7f43, 1);
  dma->Write(0x7f44, 0);
  dma->Write(0x7f45, 0x18);
  dma->Write(0x7f46, 0x60);
  dma->Write(0x7f47, 0);
  dma->AdvanceHardware(100);
  Check(dma->Read(0x6018) == image[0], "DSP DMA retains its own upper-bank ROM decoder");
  bus.Write(0x7f7c, 0x34);
  bus.Write(0x7f7d, 0x12);
  bus.Write(0x7f48, 0);
  bus.Write(0x7e0000, 0x6a);
  Check(bus.Read(0x008000) == 0x6a, "CPU ROM reads retain open bus while cache loading owns ROM");
  Check(bus.Read(0x00fffc) == 0x34 && bus.Read(0x80fffd) == 0x12,
        "Busy ROM reads expose reset-vector overrides through header mirrors");
  Check(machine->Reset(), "The console resets while the processor owns ROM");
  Check(bus.Read(0x006017) == 0x5a && bus.Read(0x008000) == image[0],
        "Reset retains work RAM and returns ROM ownership to the CPU");
  image[0x7fd8] = 6;
  Check(machine->LoadCartridge(image), "The generated board accepts a larger save allocation");
  bus.Write(0x700123, 0x71);
  Check(bus.Read(0x710123) == 0x71,
        "CPU save-bank addresses mirror their physical address without DSP address reduction");
}
void ConsoleExchange() {
  auto image = Image();
  const uint8_t worker[]{0x9c, 0x49, 0x7f, 0xa9, 0x90, 0x8d, 0x4a, 0x7f, 0x9c, 0x4b, 0x7f, 0x9c,
                         0x48, 0x7f, 0xa9, 0x02, 0x8d, 0x4f, 0x7f, 0xad, 0x5e, 0x7f, 0x29, 0x40,
                         0xd0, 0xf9, 0xad, 0x80, 0x7f, 0x8f, 0x00, 0x01, 0x7e, 0x8d, 0x22, 0x21,
                         0x9c, 0x22, 0x21, 0xa9, 0x0f, 0x8d, 0x00, 0x21, 0xcb, 0x80, 0xfd};
  const uint8_t boot[]{0x78, 0xa9, 0x80, 0x8d, 0x00, 0x21, 0x9c,
                       0x21, 0x21, 0xa2, 0x00, 0xbd, 0x00, 0x81,
                       0x9f, 0x00, 0x00, 0x7e, 0xe8, 0xe0, uint8_t(std::size(worker)),
                       0xd0, 0xf4, 0x5c, 0x00, 0x00, 0x7e};
  std::copy(std::begin(boot), std::end(boot), image.begin());
  std::copy(std::begin(worker), std::end(worker), image.begin() + 0x100);
  auto machine = std::make_unique<Emulator>();
  Check(machine->LoadCartridge(image), "The generated console and Cx4 programs load together");
  machine->StepFrame();
  Check(machine->GetBus().WramData()[0x100] == 0x2a && machine->GetPpu().CgramData()[0] == 0x2a,
        "Console code waits in WRAM while the processor owns ROM, then uses its reply as a color");
}
void FirmwareSelectionAndFastRom() {
  auto image = Image();
  const uint16_t program[]{0x7400, 0x6008, 0xe060, 0xfc00};
  for (unsigned i = 0; i < std::size(program); ++i) {
    image[0x1004 + i * 2] = uint8_t(program[i]);
    image[0x1005 + i * 2] = uint8_t(program[i] >> 8);
  }
  image[0x80000] = 0x2a;
  std::vector<uint8_t> external(3072, 0);
  external[0] = 0x11;
  CartridgeFirmware firmware;
  Check(firmware.Set(FirmwareKind::Cx4, external), "The firmware catalog accepts a Cx4 data ROM");
  Check(!firmware.Set(FirmwareKind::Cx4, std::span(external).first(3071)),
        "An incomplete Cx4 data ROM is rejected");
  auto embedded = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr, &firmware);
  image.resize(0x80000);
  auto catalog = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr, &firmware);
  for (auto *cart : {&*embedded, &*catalog}) {
    cart->Write(0x7f4a, 0x90);
    cart->Write(0x7f4f, 2);
    cart->AdvanceHardware(10000);
  }
  Check(embedded->Read(0x7f80) == 0x2a && catalog->Read(0x7f80) == 0x11,
        "Embedded data firmware takes precedence, and the catalog supplies images without it");
  image[0x7fd5] = 0x30;
  image[0x7fbf] = 0x10;
  image[0x7fda] = 0x33;
  auto fast = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr, &firmware);
  Check(fast && fast->HasCx4Processor(),
        "The fast-ROM header flag preserves Cx4 processor detection");
  image[0x7fbf] = 1;
  auto other = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr, &firmware);
  Check(other && !other->HasCx4Processor(),
        "Another extended processor subtype does not select Cx4 execution");
  Check(other->Header().chip == EnhancementChip::St010,
        "The extended signal-processor subtype retains its own cartridge family");
  std::fill_n(image.begin() + 0x7fc0, 21, ' ');
  constexpr char title[] = "2DAN MORITA SHOUGI";
  std::copy(std::begin(title), std::end(title) - 1, image.begin() + 0x7fc0);
  auto shogi = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr, &firmware);
  Check(shogi && shogi->Header().chip == EnhancementChip::St011,
        "The shogi cartridge selects the correct extended signal-processor revision");
  image[0x7fbf] = 2;
  auto arm = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr, &firmware);
  Check(arm && arm->Header().chip == EnhancementChip::St018,
        "The extended ARM subtype remains distinct from Cx4");
}
void IdleResumeAndPendingInstruction() {
  for (bool pal : {false, true}) {
    auto one = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
    auto split = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
    one->SetPal(pal);
    split->SetPal(pal);
    one->AdvanceHardware(100000);
    for (unsigned i = 0; i < 100000; ++i)
      split->AdvanceHardware(1);
    for (auto *cart : {&*one, &*split}) {
      cart->Write(0x7f4a, 0x90);
      cart->Write(0x7f4f, 2);
      cart->AdvanceHardware(10000);
    }
    Check(one->Read(0x7f80) == 0x2a && split->Read(0x7f80) == 0x2a &&
              one->Read(0x7f5e) == split->Read(0x7f5e),
          "Clock partitioning during idle time preserves the next program and IRQ result");
    auto pending = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
    pending->SetPal(pal);
    pending->Write(0x7f4a, 0x90);
    pending->Write(0x7f4f, 2);
    const uint64_t frequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
    pending->AdvanceHardware(uint32_t(2050 * frequency / 20'000'000));
    Check(pending->Read(0x7f80) == 0,
          "The register store remains pending at its instruction clock");
    pending->Write(0x7f53, 0);
    pending->AdvanceHardware(1);
    Check(pending->Read(0x7f80) == 0x2a && !pending->CpuIrqPending(),
          "Stopping execution still completes an already fetched store before entering idle");
  }
}
} // namespace
int main() {
  try {
    ProgramExecution();
    DmaLengthLatch();
    MappingAndRomOwnership();
    ConsoleExchange();
    FirmwareSelectionAndFastRom();
    IdleResumeAndPendingInstruction();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("Cx4 processor tests passed");
}
