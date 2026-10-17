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
  std::vector<uint8_t> image(0xa8000, 0);
  std::fill_n(image.begin(), 0x80000, 0xea);
  std::fill_n(image.begin() + 0x7fc0, 64, 0);
  image[0x7fc0] = 'T';
  image[0x7fd5] = 0x30;
  image[0x7fd6] = 0xf5;
  image[0x7fbf] = 2;
  image[0x7fd7] = 9;
  image[0x7fd8] = 3;
  image[0x7fd9] = 1;
  image[0x7fdc] = image[0x7fdd] = 0xff;
  image[0x7ffd] = 0x80;
  const uint32_t program[]{0xe3a00101, 0xe3a0102a, 0xe5801000, 0xe5801010, 0xeafffffe};
  for (unsigned i = 0; i < std::size(program); ++i)
    for (unsigned byte = 0; byte < 4; ++byte)
      image[0x80000 + i * 4 + byte] = uint8_t(program[i] >> (byte * 8));
  return image;
}
void FirmwareMailbox() {
  auto cart = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
  Check(cart.has_value(), "Generated ST018 image loads");
  Check(cart->Read(0x3804, 0xff) == 0, "The processor starts with the ready flag clear");
  cart->AdvanceHardware(70000);
  Check(cart->Read(0x3804) == 0x85, "Firmware publishes output, signal, and ready flags");
  Check(cart->Read(0x3800) == 0x2a, "CPU data reads consume the firmware mailbox");
  Check(cart->Read(0x3804) == 0x84, "A data read clears only the output-ready flag");
  cart->Read(0x3802);
  Check(cart->Read(0x3804) == 0x80, "A signal read clears only the signal flag");
}
void BatteryLayout() {
  auto cart = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
  Check(cart && cart->SramData().size() == 8192,
        "ST018 preserves the battery RAM size declared by its cartridge");
  cart->Write(0x700000, 0x11);
  cart->Write(0x701000, 0x22);
  Check(cart->Read(0x700000) == 0x11 && cart->Read(0x701000) == 0x22,
        "The two 4 KiB halves of an 8 KiB save are independent");
  cart->Write(0xf09000, 0x33);
  Check(cart->Read(0x701000) == 0x33 && cart->Read(0x708000) == 0x11,
        "Battery RAM mirrors across both halves of the save banks");
}
void BootClockAndReset() {
  for (bool pal : {false, true}) {
    auto cart = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
    cart->SetPal(pal);
    const uint64_t frequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
    const uint32_t boot = uint32_t(65536 * frequency / 21'440'000);
    cart->AdvanceHardware(boot);
    Check(cart->Read(0x3804) == 0, "Boot ready stays clear until the reset delay has elapsed");
    cart->AdvanceHardware(1);
    Check(cart->Read(0x3804) == 0x80, "Ready rises before the first instruction has completed");
    cart->AdvanceHardware(32);
    Check(cart->Read(0x3804) == 0x85, "Firmware runs after the region-adjusted boot delay");
    cart->Write(0x3804, 0);
    Check(cart->Read(0x3804) == 0x85, "A released reset bit does not restart a running processor");
    cart->Write(0x3804, 1);
    cart->AdvanceHardware(70000);
    Check(cart->Read(0x3804) == 0, "An asserted reset holds the processor before its boot delay");
    cart->Write(0x3804, 0);
    cart->AdvanceHardware(70000);
    Check(cart->Read(0x3804) == 0x85 && cart->Read(0x3800) == 0x2a,
          "Releasing reset boots firmware again");
  }
}
void FirmwareCatalogAndPartitioning() {
  auto image = Image();
  CartridgeFirmware firmware;
  Check(firmware.Set(FirmwareKind::St018, std::span(image).subspan(0x80000)),
        "The firmware catalog accepts a complete ST018 program and data image");
  Check(!firmware.Set(FirmwareKind::St018, std::span(image).subspan(0x80001)),
        "An incomplete firmware image is rejected");
  image.resize(0x80000);
  auto one = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr, &firmware);
  auto split = Cartridge::FromRomImage(Image(), nullptr, nullptr, nullptr);
  Check(one && split && one->HasArmProcessor() && split->HasArmProcessor() &&
            one->RomData().size() == split->RomData().size(),
        "Catalog and appended firmware select the processor and preserve cartridge ROM size");
  one->AdvanceHardware(70000);
  for (unsigned i = 0; i < 70000; ++i)
    split->AdvanceHardware(1);
  Check(one->Read(0x3804) == split->Read(0x3804) && one->Read(0x3800) == split->Read(0x3800),
        "Clock partitioning preserves firmware output");
}
void MappedFirmwarePorts() {
  auto machine = std::make_unique<Emulator>();
  Check(machine->LoadCartridge(Image()), "The system loader accepts appended ST018 firmware");
  auto &bus = machine->GetBus();
  Check(bus.Read(0x003804) == 0, "System-bus status starts before the processor boot delay");
  bus.Write(0x7e0000, 0x66);
  Check(bus.Read(0x003904) == 0x66, "The firmware bridge does not occupy the next expansion page");
  machine->StepFrame();
  Check(bus.Read(0x0038fd) == 0x85 && bus.Read(0xbf38f9) == 0x2a,
        "CPU execution clocks firmware and mirrored ports consume its output");
  Check(bus.Read(0x3f3805) == 0x84, "Odd status mirrors preserve the signal flag");
  bus.Read(0x8038f3);
  Check(bus.Read(0x003804) == 0x80, "System-bank mirrors share the signal acknowledgment");
  bus.Write(0xf09000, 0x33);
  Check(bus.Read(0x701000) == 0x33, "The system bus exposes both halves of battery RAM");
}
void ConsoleMailboxExchange() {
  auto image = Image();
  const uint32_t firmware[]{0xe3a00101, 0xe5901020, 0xe3110008, 0x0afffffc,
                            0xe5901010, 0xe2811001, 0xe5801000, 0xeafffff8};
  for (unsigned i = 0; i < std::size(firmware); ++i)
    for (unsigned byte = 0; byte < 4; ++byte)
      image[0x80000 + i * 4 + byte] = uint8_t(firmware[i] >> (byte * 8));
  const uint8_t console[]{0x78, 0xa9, 0x80, 0x8d, 0x00, 0x21, 0x9c, 0x21, 0x21, 0xa9, 0x39,
                          0x8d, 0x02, 0x38, 0xad, 0x04, 0x38, 0x29, 0x01, 0xf0, 0xf9, 0xad,
                          0x00, 0x38, 0x8f, 0x00, 0x00, 0x7e, 0x8d, 0x22, 0x21, 0x9c, 0x22,
                          0x21, 0xa9, 0x0f, 0x8d, 0x00, 0x21, 0xcb, 0x80, 0xfd};
  std::copy(std::begin(console), std::end(console), image.begin());
  auto machine = std::make_unique<Emulator>();
  Check(machine->LoadCartridge(image), "The generated console and ARM programs load together");
  machine->StepFrame();
  Check(machine->GetBus().WramData()[0] == 0x3a && machine->GetPpu().CgramData()[0] == 0x3a,
        "Console code sends a mailbox byte and uses the firmware reply as a palette color");
  Check(machine->GetBus().Read(0x003804) == 0x80,
        "Both processors consume their mailboxes during the console-driven exchange");
}
void WorkRamReset() {
  auto image = Image();
  image[0] = 0x80;
  image[1] = 0xfe;
  // Read a work RAM counter, increment it, and report it through the mailbox.
  const uint32_t firmware[]{0xe3a0020e, 0xe5d01000, 0xe2811001, 0xe5c01000,
                            0xe3a02101, 0xe5821000, 0xeafffffe};
  for (unsigned i = 0; i < std::size(firmware); ++i)
    for (unsigned byte = 0; byte < 4; ++byte)
      image[0x80000 + i * 4 + byte] = uint8_t(firmware[i] >> (byte * 8));
  auto machine = std::make_unique<Emulator>();
  Check(machine->LoadCartridge(image), "The work RAM counter firmware loads");
  auto &bus = machine->GetBus();
  machine->StepFrame();
  Check(bus.Read(0x3800) == 1, "Fresh ST018 work RAM starts cleared");
  bus.Write(0x3804, 1);
  machine->StepFrame();
  bus.Write(0x3804, 0);
  machine->StepFrame();
  Check(bus.Read(0x3800) == 2, "Bridge reset retains processor work RAM");
  bus.Write(0x700123, 0x63);
  Check(machine->Reset(), "Console reset succeeds");
  machine->StepFrame();
  Check(bus.Read(0x3800) == 1 && bus.Read(0x700123) == 0x63,
        "Console reset clears ST018 work RAM while retaining battery RAM");
}
} // namespace
int main() {
  try {
    FirmwareMailbox();
    BatteryLayout();
    BootClockAndReset();
    FirmwareCatalogAndPartitioning();
    MappedFirmwarePorts();
    ConsoleMailboxExchange();
    WorkRamReset();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("ST018 firmware tests passed");
}
