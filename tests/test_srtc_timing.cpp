#include "ConsoleConfig.hpp"
#include "snes/core/SnesCpu.hpp"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace snes::core;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

std::vector<uint8_t> Image(bool pal) {
    std::vector<uint8_t> rom(0x500000, 0);
    rom[0x408000] = 0x80;
    rom[0x408001] = 0xfe;
    rom[0x40ffc0] = 'T';
    rom[0x40ffd5] = 0x35;
    rom[0x40ffd6] = 0x55;
    rom[0x40ffd7] = 13;
    rom[0x40ffd8] = 3;
    rom[0x40ffd9] = pal ? 2 : 1;
    rom[0x40ffdc] = rom[0x40ffdd] = 0xff;
    rom[0x40fffd] = 0x80;
    return rom;
}

unsigned Seconds(Emulator& emu) {
    auto& bus = emu.GetBus();
    bus.Write(0x802801, 13);
    Check(bus.Read(0x3f2800) == 15, "The clock read marker is shared across CPU mirrors");
    const unsigned units = bus.Read(0xbf2800);
    return units + 10 * bus.Read(0x002800);
}

void ConsoleClock() {
    for (bool pal : {false, true}) {
        auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
        const auto image = Image(pal);
        Check(emu->LoadCartridge(image), "The generated clock cartridge loads");
        auto& bus = emu->GetBus();
        bus.Write(0x002801, 14);
        bus.Write(0xbf2801, 4);
        Check(Seconds(*emu) == 0, "Clearing the clock sets seconds to zero");
        emu->StepFrame();
        Check(Seconds(*emu) == 1,
              "A running console advances the cartridge clock without waiting for host time");
        Check(emu->Reset(), "The clock cartridge supports console reset");
        Check(Seconds(*emu) == 1, "Console reset retains calendar storage");
        emu->StepFrame();
        Check(Seconds(*emu) == 2, "Console reset restarts the one-Hz oscillator phase");
        for (unsigned field = 1; field < (pal ? 50u : 60u); ++field)
            emu->StepFrame();
        Check(Seconds(*emu) == 2, "The last field before one second keeps the next tick pending");
        const uint64_t deadline = (pal ? 21'281'370u : 21'477'272u) + 32;
        while (emu->CurrentMasterCycles() < deadline)
            emu->GetCpu()->Step();
        Check(Seconds(*emu) == 3,
              "The running console supplies its selected region's oscillator frequency");
    }
}

void ConsoleBatteryReload() {
    for (bool pal : {false, true}) {
        auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
        Check(emu->LoadCartridge(Image(pal)),
              "The generated clock cartridge loads for battery reload");
        emu->StepFrame();
        const auto saved = emu->SaveRtc();
        Check(emu->LoadRtc(saved), "The running console accepts its clock battery data");
        const unsigned resumed = Seconds(*emu);
        emu->StepFrame();
        Check(Seconds(*emu) == resumed,
              "Loading clock battery data retains the console-driven oscillator phase");
    }
}

void ColdStorage() {
    auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    Check(emu->LoadCartridge(Image(false)), "The generated battery-backed clock board loads");
    const auto ram = emu->LoadedCartridge()->SramData();
    Check(ram.size() == 8192, "The clock board exposes its declared battery RAM");
    for (uint8_t byte : ram)
        Check(byte == 0xff, "Fresh S-RTC battery RAM contains erased bytes");
    const auto clock = emu->SaveRtc();
    for (unsigned index = 0; index < 13; ++index)
        Check(clock[index] == 0, "The console retains the clock's cleared power-on calendar");
}

void HeaderFlags() {
    for (uint8_t mode : {uint8_t(0x25), uint8_t(0x35)}) {
        auto image = Image(false);
        image[0x40ffd5] = mode;
        auto cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
        Check(cart && cart->Header().chip == EnhancementChip::Srtc &&
                  cart->Header().mapping == MappingType::ExHiRom,
              "Slow and fast ExHiROM flags retain the cartridge clock");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1 || std::string_view(argv[1]) == "timing") {
            ConsoleClock();
            ConsoleBatteryReload();
        }
        if (argc == 1 || std::string_view(argv[1]) == "cold")
            ColdStorage();
        if (argc == 1 || std::string_view(argv[1]) == "header")
            HeaderFlags();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("S-RTC console timing tests passed");
    return 0;
}
