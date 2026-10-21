#include "ConsoleConfig.hpp"
#include "cx4/IrqProgram.hpp"
#include "snes/core/Cartridge.hpp"
#include "snes/core/SnesCpu.hpp"
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
void ConsoleInterruptTiming() {
  constexpr std::array<std::array<uint8_t, 5>, 240> expected{
      {{23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {15, 15, 5, 4, 0},    {49, 49, 5, 4, 0},    {113, 112, 6, 6, 0},  {8, 9, 7, 6, 0},
       {15, 15, 5, 4, 0},    {49, 49, 5, 4, 0},    {113, 112, 6, 6, 0},  {8, 9, 7, 6, 0},
       {15, 15, 5, 4, 0},    {49, 49, 5, 4, 0},    {113, 112, 6, 6, 0},  {8, 9, 7, 6, 0},
       {20, 21, 5, 4, 0},    {55, 55, 5, 4, 0},    {119, 118, 6, 6, 0},  {14, 15, 7, 6, 0},
       {48, 49, 5, 4, 0},    {81, 81, 5, 4, 0},    {156, 156, 6, 6, 0},  {41, 41, 7, 6, 0},
       {80, 81, 5, 4, 0},    {30, 30, 6, 6, 0},    {178, 178, 6, 6, 0},  {73, 73, 7, 6, 0},
       {23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {23, 22, 6, 6, 0},    {25, 24, 6, 6, 0},    {25, 24, 6, 6, 0},    {22, 22, 8, 8, 0},
       {9, 9, 5, 4, 0},      {44, 45, 5, 4, 0},    {108, 108, 6, 6, 0},  {3, 3, 7, 6, 0},
       {9, 9, 5, 4, 0},      {44, 45, 5, 4, 0},    {108, 108, 6, 6, 0},  {3, 3, 7, 6, 0},
       {9, 9, 5, 4, 0},      {44, 45, 5, 4, 0},    {108, 108, 6, 6, 0},  {3, 3, 7, 6, 0},
       {15, 15, 5, 4, 0},    {49, 49, 5, 4, 0},    {113, 112, 6, 6, 0},  {8, 9, 7, 6, 0},
       {42, 43, 5, 4, 0},    {76, 77, 5, 4, 0},    {151, 150, 6, 6, 0},  {36, 37, 7, 6, 0},
       {74, 75, 5, 4, 0},    {25, 24, 6, 6, 0},    {173, 172, 6, 6, 0},  {68, 69, 7, 6, 0},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22},
       {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22}, {192, 192, 6, 6, 24}, {186, 186, 8, 8, 24},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22},
       {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22}, {187, 186, 6, 6, 24}, {180, 180, 8, 8, 24},
       {30, 30, 6, 6, 16},   {23, 22, 8, 8, 16},   {30, 30, 6, 6, 16},   {23, 22, 8, 8, 16},
       {30, 30, 6, 6, 16},   {23, 22, 8, 8, 16},   {163, 162, 6, 6, 20}, {157, 156, 8, 8, 20},
       {175, 174, 6, 6, 21}, {168, 168, 8, 8, 21}, {229, 228, 6, 6, 23}, {222, 222, 8, 8, 23},
       {30, 30, 6, 6, 16},   {23, 22, 8, 8, 16},   {30, 30, 6, 6, 16},   {23, 22, 8, 8, 16},
       {30, 30, 6, 6, 16},   {23, 22, 8, 8, 16},   {163, 162, 6, 6, 20}, {157, 156, 8, 8, 20},
       {163, 162, 6, 6, 20}, {157, 156, 8, 8, 20}, {217, 216, 6, 6, 22}, {211, 210, 8, 8, 22},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22},
       {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22}, {192, 192, 6, 6, 24}, {186, 186, 8, 8, 24},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22},
       {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22}, {187, 186, 6, 6, 24}, {180, 180, 8, 8, 24},
       {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},   {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},
       {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},   {188, 188, 6, 6, 19}, {181, 180, 8, 8, 19},
       {192, 192, 6, 6, 19}, {185, 184, 8, 8, 19}, {2, 3, 6, 6, 21},     {251, 250, 8, 8, 21},
       {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},   {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},
       {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},   {188, 188, 6, 6, 19}, {181, 180, 8, 8, 19},
       {188, 188, 6, 6, 19}, {181, 180, 8, 8, 19}, {250, 250, 6, 6, 21}, {243, 242, 8, 8, 21},
       {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},   {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},
       {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},   {188, 188, 6, 6, 19}, {181, 180, 8, 8, 19},
       {192, 192, 6, 6, 19}, {185, 184, 8, 8, 19}, {2, 3, 6, 6, 21},     {251, 250, 8, 8, 21},
       {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},   {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},
       {21, 20, 6, 6, 13},   {26, 26, 8, 8, 14},   {188, 188, 6, 6, 19}, {181, 180, 8, 8, 19},
       {188, 188, 6, 6, 19}, {181, 180, 8, 8, 19}, {250, 250, 6, 6, 21}, {243, 242, 8, 8, 21},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22},
       {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22}, {192, 192, 6, 6, 24}, {186, 186, 8, 8, 24},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},
       {27, 26, 6, 6, 18},   {26, 26, 8, 8, 18},   {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22},
       {127, 126, 6, 6, 22}, {120, 120, 8, 8, 22}, {187, 186, 6, 6, 24}, {180, 180, 8, 8, 24}}};
  unsigned scene = 0;
  for (unsigned probe = 0; probe < 7; ++probe)
    for (bool pal : {false, true})
      for (bool immediate : {false, true}) {
        constexpr std::array<uint8_t, 6> counts{0, 1, 7, 31, 127, 251};
        constexpr std::array<uint8_t, 3> probeCounts{0, 7, 251};
        constexpr std::array<unsigned, 4> delays{0, 1, 5, 19};
        constexpr std::array<unsigned, 2> probeDelays{0, 19};
        const auto selectedCounts =
            probe == 0 ? std::span<const uint8_t>(counts) : std::span<const uint8_t>(probeCounts);
        const auto selectedDelays =
            probe == 0 ? std::span<const unsigned>(delays) : std::span<const unsigned>(probeDelays);
        for (uint8_t count : selectedCounts)
          for (unsigned delay : selectedDelays) {
            auto config = snes::tests::DeterministicConsoleConfig();
            config.cx4DelayedSync = !immediate;
            auto machine = std::make_unique<Emulator>(config);
            Check(machine->LoadCartridge(snes::tests::cx4::IrqProgram(
                      count, delay, pal, snes::tests::cx4::Probe(probe))),
                  "The graphics interrupt cartridge loads");
            for (bool warm : {false, true}) {
              if (warm)
                Check(machine->Reset(), "Warm reset restarts graphics interrupt timing");
              machine->StepFrame({false, false, false});
              const auto *memory = machine->GetBus().WramData() + 0x1100;
              const auto &result = expected[scene];
              Check(std::equal(result.begin(), result.end(), memory) && memory[8] == 0xa5 &&
                        machine->GetCpu()->regs().stp &&
                        !machine->LoadedCartridge()->CpuIrqPending(),
                    "Graphics IRQ timing and CPU polling follow the selected handoffs");
              for (unsigned byte = 5; byte < 16; ++byte)
                Check(memory[byte] == (byte == 8 ? 0xa5 : 0),
                      "Graphics interrupt results retain the complete guest output area");
            }
            ++scene;
          }
      }
  Check(scene == expected.size(), "Every interrupt timing and CPU access case executes");
}
std::vector<uint8_t> Image() {
  std::vector<uint8_t> image(0x80c00, 0);
  std::fill_n(image.begin(), 0x80000, 0xea);
  std::fill_n(image.begin() + 0x7fb0, 80, 0);
  image[0x7fc0] = 'T';
  image[0x7fd5] = 0x20;
  image[0x7fd6] = 0xf3;
  image[0x7fbf] = 0x10;
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
void DeferredHandoffs() {
  for (bool delayed : {false, true})
    for (bool pal : {false, true}) {
      auto cart = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
      cart->SetCx4DelayedSync(delayed);
      cart->SetPal(pal);
      for (bool warm : {false, true}) {
        if (warm)
          cart->Reset();
        cart->Write(0x7f4a, 0x90);
        cart->Write(0x7f4f, 2);
        cart->AdvanceHardware(5000, 0x7e0000, false, false);
        Check(cart->Read(0x7f80) == (delayed ? 0 : 0x2a) && cart->CpuIrqPending() == !delayed,
              "Deferred CPU reads retain pending graphics work until a forced handoff");
        cart->Write(0x7f80, 0xa5);
        Check(cart->Read(0x7f80) == 0xa5 && cart->CpuIrqPending() == !delayed,
              "CPU writes preserve their order ahead of deferred processor execution");
        cart->AdvanceHardware(0, 0x7e0000, false, true);
        Check(cart->Read(0x7f80) == (delayed ? 0x2a : 0xa5) && cart->CpuIrqPending(),
              "A zero-clock handoff consumes retained debt without replacing elapsed time");
      }
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
  auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Check(machine->LoadCartridge(image) && machine->LoadedCartridge()->HasCx4Processor(),
        "The system loader selects processor execution with appended data firmware");
  auto &bus = machine->GetBus();
  bus.Write(0x006017, 0x5a);
  Check(bus.Read(0x807017) == 0x5a, "Data RAM mirrors across offset and bank windows");
  bus.Write(0x006c00, 0xa5);
  Check(bus.Read(0x007c00) == 0, "Unused I/O locations do not become extra work RAM");
  bus.Write(0x700123, 0x66);
  bus.Write(0x7e0000, 0xa5);
  Check(bus.Read(0x770123) == 0 && machine->LoadedCartridge()->SramData().empty(),
        "The graphics interface returns zero when its save storage is absent");
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
  Check(machine->LoadCartridge(image) && !machine->LoadedCartridge()->BoardConnected(),
        "A save declaration does not invent a different graphics board profile");
  bus.Write(0x7e0000, 0x71);
  Check(bus.Read(0x008000) == 0x71 && bus.Read(0x700123) == 0x71,
        "An unconnected graphics profile retains open bus in ROM and save regions");
}
void ExplicitSaveWiring() {
  for (uint8_t declaration : {3, 6}) {
    auto image = Image();
    image[0x7fd8] = declaration;
    auto detected = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
    Check(detected && !detected->BoardConnected(), "The raw graphics save profile is unconnected");
    CartridgeDatabase database;
    DatabaseOverride layout;
    layout.forceMapping = MappingType::LoRom;
    database.AddOverride(detected->RomCrc32(), layout);
    auto cart = Cartridge::FromRomImage(image, nullptr, &database, nullptr);
    Check(cart && cart->HasCx4Processor() &&
              cart->SramData().size() == (size_t(1024) << declaration),
          "An explicit board override retains its requested save wiring");
    MemoryBus bus;
    bus.MapWram();
    bus.MapCartridge(*cart);
    bus.Write(0x700123, 0x71);
    Check(bus.Read(declaration == 3 ? 0x770123 : 0x710123) == 0x71,
          "Explicit save windows retain their physical bank mirrors");
    cart->Reset();
    Check(bus.Read(0x700123) == 0x71, "Reset preserves explicitly connected save storage");
  }
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
  auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
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
  for (auto kind : {FirmwareKind::St011, FirmwareKind::St018})
    Check(firmware.Set(kind, std::vector<uint8_t>(CartridgeFirmware::Size(kind))),
          "Other processor subtypes provide their own complete firmware images");
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
    ConsoleInterruptTiming();
    DeferredHandoffs();
    ProgramExecution();
    DmaLengthLatch();
    MappingAndRomOwnership();
    ExplicitSaveWiring();
    ConsoleExchange();
    FirmwareSelectionAndFastRom();
    IdleResumeAndPendingInstruction();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("Cx4 processor tests passed");
}
