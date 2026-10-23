#include "ConsoleConfig.hpp"
#include "audio/MsuFixture.hpp"
#include "gameboy/CameraProgram.hpp"
#include "gameboy/StartupProgram.hpp"
#include "snes/core/gameboy/AudioMixer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace snes::core;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<uint8_t> Bios(bool secondModel, bool pal = false, uint8_t control = 0x81) {
    std::vector<uint8_t> bytes(0x80000);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = uint8_t(i >> 15);
    std::fill_n(bytes.begin() + 0x7fb0, 80, 0);
    std::fill_n(bytes.begin() + 0x7fc0, 21, ' ');
    const std::string_view title = secondModel ? "Super GAMEBOY2" : "Super GAMEBOY";
    std::copy(title.begin(), title.end(), bytes.begin() + 0x7fc0);
    bytes[0x7fd5] = secondModel ? 0x30 : 0x20;
    bytes[0x7fd6] = secondModel ? 0 : 0xe3;
    if (secondModel) {
        bytes[0x7fda] = 0x33;
        std::copy_n("042J", 4, bytes.begin() + 0x7fb2);
    }
    bytes[0x7fd7] = 9;
    bytes[0x7fd9] = pal ? 2 : 0;
    bytes[0x7fdc] = bytes[0x7fdd] = 0xff;
    bytes[0x7ffd] = 0x80;
    const std::array<uint8_t, 7> program{0x78, 0xa9, control, 0x8d, 0x03, 0x60, 0xdb};
    std::copy(program.begin(), program.end(), bytes.begin());
    return bytes;
}

std::array<uint8_t, 256> Boot() {
    std::array<uint8_t, 256> bytes{};
    bytes[0] = 0xc3;
    bytes[1] = 0xfc;
    bytes[0xfc] = 0x3e;
    bytes[0xfd] = 1;
    bytes[0xfe] = 0xe0;
    bytes[0xff] = 0x50;
    return bytes;
}

std::vector<uint8_t> Game(bool sound = false) {
    std::vector<uint8_t> bytes(32768);
    bytes[0x147] = 3;
    bytes[0x149] = 2;
    std::vector<uint8_t> program{0x3e, 0x0a, 0xea, 0, 0};
    if (sound)
        for (const auto [address, value] : std::array<std::array<uint8_t, 2>, 7>{{{0x26, 0x80},
                                                                                  {0x24, 0x77},
                                                                                  {0x25, 0xff},
                                                                                  {0x11, 0x40},
                                                                                  {0x12, 0xf3},
                                                                                  {0x13, 0x87},
                                                                                  {0x14, 0x83}}})
            program.insert(program.end(), {0x3e, value, 0xe0, address});
    program.insert(program.end(), {0x21, 0, 0xa0, 0x34, 0x18, 0xfd});
    std::copy(program.begin(), program.end(), bytes.begin() + 0x100);
    return bytes;
}

void ConsoleStartupEntropy() {
    constexpr uint64_t expected[2][2][2][2] = {
        {
            {
                {0x83dcd331ece72507ull, 0x83dcd331ece72507ull},
                {0x83dcd331ece72507ull, 0x83dcd331ece72507ull},
            },
            {
                {0xa2b002b734b29813ull, 0xcd274d4cba8ada78ull},
                {0x9c4e429010c917d7ull, 0x33cdbc1dd08f98c9ull},
            },
        },
        {
            {
                {0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull},
                {0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull},
            },
            {
                {0x06d32353afee2433ull, 0xb6e2eb304dc672eeull},
                {0x6cabe51aea7063f1ull, 0x016c241a40b9d947ull},
            },
        },
    };
    constexpr std::array<uint32_t, 2> seeds{1, 0xc0ffee};
    for (unsigned model = 0; model < 2; ++model)
        for (const auto mode : {EntropyMode::None, EntropyMode::Low, EntropyMode::High})
            for (unsigned seed = 0; seed < seeds.size(); ++seed) {
                auto config = snes::tests::DeterministicConsoleConfig();
                config.entropyMode = mode;
                config.entropySeed = seeds[seed];
                auto machine = std::make_unique<Emulator>(config);
                Check(machine->LoadSuperGameBoy(Bios(model != 0),
                                                snes::tests::gameboy::StartupProgram(), Boot()),
                      "The startup-memory copier cartridge loads");
                const std::vector<uint8_t> initial(32768, 0);
                Check(machine->LoadSlotBattery(0, initial), "Load an empty copier battery");
                for (unsigned warm = 0; warm < 2; ++warm) {
                    if (warm)
                        Check(machine->Reset(), "The copier also executes after console reset");
                    for (unsigned frame = 0; frame < 8; ++frame)
                        machine->StepFrame({false, false, false});
                    const auto saved = machine->SaveSlotBattery(0);
                    Check(saved.size() == 32768 && saved[8495] == warm + 1,
                          "The guest finishes a new startup-memory copy after each reset");
                    uint64_t signature = 14695981039346656037ull;
                    for (unsigned address = 0; address < 8495; ++address) {
                        signature ^= saved[address];
                        signature *= 1099511628211ull;
                    }
                    Check(signature == expected[model][mode != EntropyMode::None][seed][warm],
                          "Console entropy settings and reset order reach the handheld memory");
                }
            }
}

uint64_t StartupSignature(std::span<const uint8_t> bytes) {
    uint64_t value = 14695981039346656037ull;
    for (uint8_t byte : bytes)
        value = (value ^ byte) * 1099511628211ull;
    return value;
}

void ConsoleReloadEntropy() {
    constexpr uint64_t expected[2][2][3][3][2] = {
        {{{{0x83dcd331ece72507ull, 0x83dcd331ece72507ull},
           {0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull},
           {0x83dcd331ece72507ull, 0x83dcd331ece72507ull}},
          {{0x83dcd331ece72507ull, 0x83dcd331ece72507ull},
           {0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull},
           {0x83dcd331ece72507ull, 0x83dcd331ece72507ull}},
          {{0x83dcd331ece72507ull, 0x83dcd331ece72507ull},
           {0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull},
           {0x83dcd331ece72507ull, 0x83dcd331ece72507ull}}},
         {{{0xa2b002b734b29813ull, 0xcd274d4cba8ada78ull},
           {0x2f2684a3979cb046ull, 0x26dd76fbadd81b98ull},
           {0x1d2cdfcda64ef7f1ull, 0x0e52dcbb01ea5053ull}},
          {{0x9c4e429010c917d7ull, 0x33cdbc1dd08f98c9ull},
           {0x1e352b8020b2114eull, 0x1586e73ac2cdea8eull},
           {0x56e75f5f76074fe9ull, 0x8a98aceed3ac227eull}},
          {{0x5983ef54bb40ae55ull, 0x13c5b072cfbb3f1full},
           {0x715683d8ee0b073dull, 0x9eb87e95c78ca1f3ull},
           {0x343170d3be63cd43ull, 0x4b4fd2e9e36b24edull}}}},
        {{{{0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull},
           {0x83dcd331ece72507ull, 0x83dcd331ece72507ull},
           {0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull}},
          {{0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull},
           {0x83dcd331ece72507ull, 0x83dcd331ece72507ull},
           {0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull}},
          {{0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull},
           {0x83dcd331ece72507ull, 0x83dcd331ece72507ull},
           {0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull}}},
         {{{0x06d32353afee2433ull, 0xb6e2eb304dc672eeull},
           {0x26721664876e98faull, 0xff3d85cee190484dull},
           {0x1332c1cdf4edb750ull, 0x174c43738b229b41ull}},
          {{0x6cabe51aea7063f1ull, 0x016c241a40b9d947ull},
           {0x9a2f7e40d6fe0f4full, 0xc06fb68e83f58729ull},
           {0xb9229fffc8ead116ull, 0xd3033d090e385fe0ull}},
          {{0x055801007060c1b7ull, 0x8cecaaa9f095e92full},
           {0x52250348017e2171ull, 0x6f18717148d3a66dull},
           {0x894569376903d19full, 0x56ea3e25c993441cull}}}}};
    constexpr std::array<uint64_t, 3> seeds{1, 0xc0ffee, 0x100000001ull};
    for (unsigned model = 0; model < 2; ++model)
        for (const auto mode : {EntropyMode::None, EntropyMode::Low, EntropyMode::High})
            for (unsigned seed = 0; seed < seeds.size(); ++seed) {
                auto config = snes::tests::DeterministicConsoleConfig();
                config.entropyMode = mode;
                config.entropySeed = uint32_t(seeds[seed]);
                config.entropySequence = uint32_t(seeds[seed] >> 32);
                auto machine = std::make_unique<Emulator>(config);
                for (unsigned load = 0; load < 3; ++load) {
                    const bool secondModel = (load & 1) ? model == 0 : model != 0;
                    Check(machine->LoadSuperGameBoy(Bios(secondModel),
                                                    snes::tests::gameboy::StartupProgram(), Boot()),
                          "The same console replaces its cartridge and adapter model");
                    Check(machine->LoadSlotBattery(0, std::vector<uint8_t>(32768)),
                          "Each replacement receives its own empty battery");
                    for (unsigned warm = 0; warm < 2; ++warm) {
                        if (warm)
                            Check(machine->Reset(), "The replacement also supports console reset");
                        for (unsigned frame = 0; frame < 8; ++frame)
                            machine->StepFrame({false, false, false});
                        const auto saved = machine->SaveSlotBattery(0);
                        Check(saved.size() == 32768 && saved[8495] == warm + 1,
                              "Every replacement executes a complete new startup-memory copy");
                        Check(
                            StartupSignature({saved.data(), 8495}) ==
                                expected[model][mode != EntropyMode::None][seed][load][warm],
                            "Startup noise continues across replacement, model changes and reset");
                    }
                }
            }
}

void CameraReloadEntropy() {
    constexpr uint64_t expected[2][3][4][2] = {{{{0xb25ddc92772e7c1full, 0xb25ddc92772e7c1full},
                                                 {0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull},
                                                 {0xb25ddc92772e7c1full, 0xb25ddc92772e7c1full},
                                                 {0x83dcd331ece72507ull, 0x83dcd331ece72507ull}},
                                                {{0x558662e1dc6c39a8ull, 0x53bdd6421639f21bull},
                                                 {0x0db6b407c14bcb3dull, 0xca634973b135627full},
                                                 {0x6016e019252a9d67ull, 0x0f9b6f81124b1f9dull},
                                                 {0x0819e7024ecb5cadull, 0xbc21980d5679c020ull}},
                                                {{0x558662e1dc6c39a8ull, 0x53bdd6421639f21bull},
                                                 {0x0db6b407c14bcb3dull, 0xca634973b135627full},
                                                 {0x6016e019252a9d67ull, 0x0f9b6f81124b1f9dull},
                                                 {0x0819e7024ecb5cadull, 0xbc21980d5679c020ull}}},
                                               {{{0xb25ddc92772e7c1full, 0xb25ddc92772e7c1full},
                                                 {0x83dcd331ece72507ull, 0x83dcd331ece72507ull},
                                                 {0xb25ddc92772e7c1full, 0xb25ddc92772e7c1full},
                                                 {0x38df0d2b727c8507ull, 0x38df0d2b727c8507ull}},
                                                {{0x45700a1220a5a951ull, 0xbf10a13f6e726ccdull},
                                                 {0x80b8f1d1420e9436ull, 0xf9f3293b240f8dddull},
                                                 {0xe0687cc7545183c0ull, 0xbf859f5dc37066e9ull},
                                                 {0x877197d5444bee80ull, 0x65ac5481e863fdb9ull}},
                                                {{0x45700a1220a5a951ull, 0xbf10a13f6e726ccdull},
                                                 {0x80b8f1d1420e9436ull, 0xf9f3293b240f8dddull},
                                                 {0xe0687cc7545183c0ull, 0xbf859f5dc37066e9ull},
                                                 {0x877197d5444bee80ull, 0x65ac5481e863fdb9ull}}}};
    for (unsigned model = 0; model < 2; ++model)
        for (unsigned mode = 0; mode < 3; ++mode) {
            auto config = snes::tests::DeterministicConsoleConfig();
            config.entropyMode = EntropyMode(mode);
            config.entropySeed = 1;
            auto machine = std::make_unique<Emulator>(config);
            for (unsigned load = 0; load < 4; ++load) {
                const bool camera = !(load & 1);
                const bool secondModel = load == 0 || load == 3 ? model != 0 : model == 0;
                Check(machine->LoadSuperGameBoy(Bios(secondModel),
                                                camera ? snes::tests::gameboy::CameraProgram()
                                                       : snes::tests::gameboy::StartupProgram(),
                                                Boot()),
                      "Camera and startup-copier cartridges replace each other");
                Check(machine->LoadSlotBattery(0, std::vector<uint8_t>(camera ? 131072 : 32768)),
                      "Each Camera or copier receives its own empty battery");
                for (unsigned warm = 0; warm < 2; ++warm) {
                    if (warm)
                        Check(machine->Reset(), "The Camera sequence also covers console reset");
                    for (unsigned frame = 0; frame < 16; ++frame)
                        machine->StepFrame({false, false, false});
                    uint64_t signature;
                    if (camera) {
                        const auto saved = machine->SaveSlotBattery(0);
                        Check(saved.size() == 131072 && saved[16383] == warm + 1,
                              "The guest completes a new Camera capture and image copy after every "
                              "reset");
                        signature = StartupSignature({saved.data() + 8192, 3584});
                    } else {
                        const auto saved = machine->SaveSlotBattery(0);
                        Check(saved.size() == 32768 && saved[8495] == warm + 1,
                              "The copier reads memory after Camera noise consumption");
                        signature = StartupSignature({saved.data(), 8495});
                    }
                    Check(signature == expected[model][mode][load][warm],
                          "Camera capture and later startup memory share the console's persistent "
                          "noise");
                }
            }
        }
}

void BoardMapping() {
    for (bool secondModel : {false, true}) {
        const auto bios = Bios(secondModel);
        auto cartridge = Cartridge::FromRomImage(bios, nullptr, nullptr, nullptr);
        Check(cartridge && cartridge->BoardConnected() &&
                  cartridge->Header().chip == EnhancementChip::SuperGameBoy &&
                  cartridge->SramData().empty(),
              "Both adapter BIOS headers select their cartridge device");
        auto bus = std::make_unique<MemoryBus>();
        bus->MapCartridge(*cartridge);
        for (unsigned bank = 0; bank < 256; ++bank) {
            if (bank == 0x7e || bank == 0x7f)
                continue;
            for (unsigned offset :
                 {0x6000u, 0x600fu, 0x67ffu, 0x6800u, 0x7000u, 0x7800u, 0x7fffu, 0x8123u}) {
                const auto address = (bank << 16) | offset;
                uint8_t expected = 0xa5;
                if ((bank & 0x40) || offset >= 0x8000)
                    expected = bios[((bank << 15) | (offset & 0x7fff)) & (bios.size() - 1)];
                else if (offset < 0x6800 || offset >= 0x7000)
                    expected = offset == 0x600f ? 0x21 : offset == 0x7800 ? 0xff : 0;
                bus->SetOpenBus(0xa5);
                Check(bus->Read(address) == expected,
                      "Adapter ports, ROM mirrors and open-bus gaps retain their bank selection");
            }
        }
    }
    auto bytes = Bios(false);
    bytes[0x7fd6] = 0;
    auto ordinary = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
    Check(ordinary && ordinary->Header().chip == EnhancementChip::None &&
              ordinary->Read(0x600f, 0xa5) == 0xa5,
          "An adapter title alone does not replace ordinary cartridge wiring");
    bytes[0x7fd6] = 0xe3;
    std::fill_n(bytes.begin() + 0x7fc0, 21, ' ');
    auto generic = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
    Check(generic && generic->Header().chip == EnhancementChip::SuperGameBoy &&
              generic->Read(0x600f) == 0x21,
          "The cartridge type selects adapter wiring without a recognized title");
}

void ClockAndBattery() {
    for (bool secondModel : {false, true})
        for (bool pal : {false, true})
            for (uint8_t divider = 0; divider < 4; ++divider) {
                auto whole = Cartridge::FromSuperGameBoy(Bios(secondModel), Game(), Boot());
                auto split = Cartridge::FromSuperGameBoy(Bios(secondModel), Game(), Boot());
                Check(whole && split,
                      "Generated slot and boot images load for both adapter models");
                const std::vector<uint8_t> saved(8192, 0);
                Check(whole->LoadSlotBattery(0, saved) && split->LoadSlotBattery(0, saved),
                      "The inserted cartridge accepts its own battery data");
                whole->SetPal(pal);
                split->SetPal(pal);
                whole->AdvanceHardware(10000);
                Check(whole->SaveSlotBattery(0) == saved,
                      "A held handheld leaves battery RAM unchanged");
                whole->Write(0x006003, uint8_t(0x80 | divider));
                split->Write(0x806003, uint8_t(0x80 | divider));
                std::vector<std::array<int16_t, 2>> a, b;
                constexpr std::array<unsigned, 4> divisors{4, 5, 7, 9};
                const auto master = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
                const auto rate = (secondModel ? 20'971'520 : master) / divisors[divider] / 128;
                const auto sink = [rate](auto& samples) {
                    return [&samples, rate](int16_t left, int16_t right, uint32_t sampleRate) {
                        Check(sampleRate == rate,
                              "Audio samples retain the selected physical oscillator rate");
                        samples.push_back({left, right});
                    };
                };
                whole->SetGameBoyAudioOutput(sink(a));
                split->SetGameBoyAudioOutput(sink(b));
                whole->AdvanceHardware(12345);
                unsigned elapsed = 0;
                for (unsigned i = 0; elapsed < 12345; ++i) {
                    const auto clocks = std::min(12345 - elapsed, (i * 17) % 257 + 1);
                    split->AdvanceHardware(clocks);
                    elapsed += clocks;
                }
                auto battery = whole->SaveSlotBattery(0);
                Check(
                    battery.size() == 8192 && battery[0] != 0 &&
                        battery == split->SaveSlotBattery(0) && !a.empty() && a == b,
                    "Master-clock batching preserves handheld execution, battery bytes and sound");
                whole->Write(0x6003, divider);
                whole->AdvanceHardware(10000);
                Check(whole->SaveSlotBattery(0) == battery,
                      "Clearing run control holds processor execution");
                whole->Write(0x6003, uint8_t(0x80 | divider));
                Check(whole->SaveSlotBattery(0) == battery,
                      "A control-port reset retains the inserted cartridge battery");
                whole->Reset();
                Check(whole->SaveSlotBattery(0) == battery && whole->Read(0x6000) == 0,
                      "Console reset retains the battery and resets the adapter link");
                Check(!whole->LoadSlotBattery(1, saved) && whole->SaveSlotBattery(1).empty(),
                      "An absent slot cannot overwrite the inserted cartridge battery");
            }
}

void ConsoleExecutionAndFailedLoads() {
    for (bool secondModel : {false, true}) {
        auto emulator = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
        const auto bios = Bios(secondModel);
        const auto game = Game();
        const auto boot = Boot();
        Check(emulator->LoadSuperGameBoy(bios, game, boot),
              "The console accepts a complete adapter set");
        Check(emulator->LoadSlotBattery(0, std::vector<uint8_t>(8192, 0)),
              "A slot save loads before boot");
        emulator->StepFrame();
        const auto battery = emulator->SaveSlotBattery(0);
        Check(battery.size() == 8192 && battery[0] != 0 &&
                  emulator->GetBus().Read(0x80600f) == 0x21,
              "SNES control-port writes start the handheld on the console clock bus");
        emulator->GetBus().Write(0x7e1234, 0x5a);
        const auto frame = emulator->CurrentFrame();
        std::string error;
        Check(!emulator->LoadSuperGameBoy(bios, game, std::span<const uint8_t>(boot).first(255),
                                          &error) &&
                  !error.empty(),
              "An incomplete handheld boot image is rejected");
        auto ordinary = bios;
        std::fill_n(ordinary.begin() + 0x7fc0, 21, ' ');
        ordinary[0x7fd6] = ordinary[0x7fda] = 0;
        Check(!emulator->LoadSuperGameBoy(ordinary, game, boot, &error) && !error.empty(),
              "An ordinary cartridge cannot replace the adapter BIOS");
        Check(emulator->CurrentFrame() == frame && emulator->GetBus().Read(0x7e1234) == 0x5a &&
                  emulator->SaveSlotBattery(0) == battery,
              "Failed adapter loads preserve the running console, RAM and slot battery");
    }
}

void ConsoleClockBoundaries() {
    constexpr std::array<std::array<uint8_t, 3>, 16> expected{{{114, 252, 135},
                                                               {243, 149, 55},
                                                               {25, 104, 183},
                                                               {131, 250, 112},
                                                               {114, 195, 20},
                                                               {243, 205, 168},
                                                               {25, 254, 227},
                                                               {131, 54, 232},
                                                               {39, 90, 140},
                                                               {183, 19, 111},
                                                               {238, 11, 40},
                                                               {98, 177, 1},
                                                               {67, 84, 100},
                                                               {206, 117, 27},
                                                               {254, 191, 127},
                                                               {111, 4, 154}}};
    unsigned fixture = 0;
    for (bool secondModel : {false, true})
        for (bool pal : {false, true})
            for (uint8_t divider = 0; divider < 4; ++divider, ++fixture) {
                for (bool overrideRegion : {false, true}) {
                    auto config = snes::tests::DeterministicConsoleConfig();
                    if (overrideRegion)
                        config.regionOverride = pal ? Region::PAL : Region::NTSC;
                    auto emulator = std::make_unique<Emulator>(config);
                    Check(emulator->LoadSuperGameBoy(Bios(secondModel, overrideRegion ? !pal : pal,
                                                          uint8_t(0x80 | divider)),
                                                     Game(true), Boot()),
                          "A generated console program selects the adapter model, region and "
                          "oscillator");
                    std::vector<uint8_t> battery(8192, 0);
                    Check(emulator->LoadSlotBattery(0, battery),
                          "The timing fixture starts with a known save");
                    for (unsigned frame = 0; frame < 3; ++frame) {
                        emulator->StepFrame();
                        battery[0] = expected[fixture][frame];
                        Check(
                            emulator->SaveSlotBattery(0) == battery,
                            "Console frame pauses retain the handheld's pending CPU bus interval");
                    }
                }
            }
}

uint64_t PcmDigest(std::span<const int16_t> samples) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (int16_t sample : samples) {
        const auto word = uint16_t(sample);
        for (unsigned shift : {0u, 8u}) {
            hash ^= uint8_t(word >> shift);
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}

void ConsoleAudioHandoffs() {
    struct Expected {
        size_t rawFrames;
        uint64_t rawHash;
        size_t mixedFrames;
        uint64_t mixedHash;
        uint8_t battery;
    };

    constexpr std::array<Expected, 32> expected{{
        {11029u, 0xd20f79c689d3b609ull, 8455u, 0xbc220bc4a5e9e401ull, 22u},
        {11029u, 0x1182d2bd50a314b9ull, 8455u, 0x780f83204c91ec31ull, 44u},
        {8824u, 0x7858ef16845f3545ull, 8455u, 0x9268ea00b9848e5ull, 238u},
        {8825u, 0xab0d54220d75bef1ull, 8455u, 0xd3abc93f116ef5d9ull, 220u},
        {6368u, 0x6fb3bba546253149ull, 8455u, 0x7af02a2c9c3f37d9ull, 123u},
        {6368u, 0xab3a27d3f2515fcdull, 8455u, 0xf3ce9626675608b5ull, 246u},
        {4983u, 0x2973d80d2118dcc9ull, 8455u, 0xd5678ffd8e8c4085ull, 29u},
        {4984u, 0xde428f147c5e4971ull, 8455u, 0xbb86cafa0bf421a5ull, 58u},
        {13027u, 0x44a4d9b1b3416465ull, 10073u, 0x8119ac43871b7bd1ull, 183u},
        {13027u, 0xc826ce8b46d32515ull, 10073u, 0xd9a928adfa8e48d1ull, 110u},
        {10423u, 0xf82b885eed38a069ull, 10073u, 0x8089450991292e1dull, 59u},
        {10423u, 0x221624b3deae00b1ull, 10073u, 0xa31fb43cf389f475ull, 118u},
        {7510u, 0x5b2db67fcb5e2f19ull, 10073u, 0x85c7040d50430b4dull, 68u},
        {7510u, 0xc9e57577428c0855ull, 10073u, 0xf2bb23d9b0db42c5ull, 136u},
        {5871u, 0x93361342c189a735ull, 10073u, 0x171e16c30cfac365ull, 157u},
        {5872u, 0x3e68e0a80075d57dull, 10073u, 0xb52c5e0b132a5b41ull, 58u},
        {10770u, 0x5c2bdb7cf50706d1ull, 8455u, 0xa7ec23328c573f59ull, 177u},
        {10770u, 0x87fc35249638fdull, 8455u, 0xd79723bcb9c5d15ull, 98u},
        {8617u, 0x43b858c64d685285ull, 8455u, 0x3915d2c8cf6f49a9ull, 158u},
        {8618u, 0xba21af54306e45f1ull, 8455u, 0x8fa4997c1ae67931ull, 60u},
        {6218u, 0xbc788a51d44ddfcdull, 8455u, 0xf361096522bf1f4dull, 98u},
        {6219u, 0x7a20f85ed75acf09ull, 8455u, 0x71c4b7fb64432521ull, 196u},
        {4867u, 0xb5e84cc5dff76935ull, 8455u, 0xa7c3ce3d1b5e18e1ull, 181u},
        {4867u, 0x7d578ecb2f42a2f1ull, 8455u, 0x9e619ba7134252c5ull, 106u},
        {12838u, 0x151b2552d14328e1ull, 10073u, 0x8e81ddcf51bc42a5ull, 198u},
        {12838u, 0xd807bb7354dc6331ull, 10073u, 0x9f929a74e706b199ull, 140u},
        {10272u, 0xb0b5377b1d7fd3f9ull, 10073u, 0x17b80f687a0be221ull, 21u},
        {10272u, 0x6280671ccb705011ull, 10073u, 0x7e19c9b164d19acdull, 42u},
        {7401u, 0x12254cf4d85207adull, 10073u, 0x2635bebbfc11fefdull, 2u},
        {7402u, 0x6d012bdea9d9d6adull, 10073u, 0xcbe00929f8f33ff5ull, 4u},
        {5786u, 0x7de65b806fccba99ull, 10073u, 0x52137d1c89adce65ull, 219u},
        {5786u, 0x44db9b7da6c1afd9ull, 10073u, 0x10988d2dce254e51ull, 182u},
    }};

    struct Output final : IAudioOutput {
        std::vector<int16_t> samples;

        void Submit(const AudioBuffer& buffer) override {
            Check(buffer.sampleRate == 32040, "Handoff output retains the fixed stream rate");
            for (float sample : buffer.interleavedStereo)
                samples.push_back(
                    int16_t(std::clamp<long>(std::lrint(double(sample) * 32768), -32768, 32767)));
        }
    };

    unsigned index = 0;
    for (bool secondModel : {false, true})
        for (bool pal : {false, true})
            for (uint8_t divider = 0; divider < 4; ++divider) {
                auto bios = Bios(secondModel, pal, 0);
                msu_test::Program p;
                p.Emit({0x78});
                p.Delay(1 + divider);
                p.Store(0x6003, uint8_t(0x80 | divider));
                p.Delay(3);
                p.Store(0x6003, uint8_t((divider + 1) & 3));
                p.Delay(1);
                p.Store(0x6003, uint8_t(0x80 | ((divider + 2) & 3)));
                p.Delay(4);
                p.Store(0x6003, uint8_t(0x80 | divider));
                p.Emit({0xdb});
                std::copy(p.code.begin(), p.code.end(), bios.begin());
                Output output;
                std::vector<int16_t> raw;
                auto emulator =
                    std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
                Check(emulator->LoadSuperGameBoy(bios, Game(true), Boot()),
                      "The generated reset and divider program loads");
                std::vector<uint8_t> battery(8192, 0);
                Check(emulator->LoadSlotBattery(0, battery),
                      "The handoff program starts with an empty save");
                emulator->AttachAudioOutput(&output);
                const_cast<Cartridge*>(emulator->LoadedCartridge())
                    ->SetGameBoyAudioOutput([&](int16_t left, int16_t right, uint32_t) {
                        raw.push_back(left);
                        raw.push_back(right);
                    });
                for (bool warm : {false, true}) {
                    raw.clear();
                    output.samples.clear();
                    if (warm)
                        Check(emulator->Reset(),
                              "Warm console reset retains the handheld save input");
                    for (unsigned frame = 0; frame < 16; ++frame)
                        emulator->StepFrame();
                    const auto& snapshot = expected[index++];
                    Check(std::count_if(raw.begin(), raw.end(),
                                        [](int16_t value) { return value > 1 || value < -1; }) >
                              1000,
                          "The handheld produces audible samples before its signature is checked");
                    Check(raw.size() == snapshot.rawFrames * 2 &&
                              PcmDigest(raw) == snapshot.rawHash,
                          "Scanline handoffs preserve every raw sample across divider changes and "
                          "repeated "
                          "resets");
                    Check(output.samples.size() == snapshot.mixedFrames * 2 &&
                              PcmDigest(output.samples) == snapshot.mixedHash,
                          "The complete mixed stream retains its sample count and phase");
                    battery[0] = snapshot.battery;
                    Check(emulator->SaveSlotBattery(0) == battery,
                          "Sound handoffs preserve the complete generated save");
                }
            }
}

void StereoAudioMix() {
    struct Fixture {
        uint32_t rate;
        unsigned pattern, frames;
        uint64_t digest;
    };

    constexpr Fixture fixtures[]{
        {18204, 0, 513, 0x07b7d0126b1d1ebcULL}, {18204, 1, 513, 0xfdd18d6c4f00241cULL},
        {18204, 2, 513, 0x2ea0746f40726c75ULL}, {18473, 0, 513, 0xeba162e704833beeULL},
        {18473, 1, 513, 0x0caf84c579596953ULL}, {18473, 2, 513, 0xe625c9fbfcaaac0cULL},
        {18643, 0, 513, 0xd8d5643790966efcULL}, {18643, 1, 513, 0x5fea83f84077bfabULL},
        {18643, 2, 513, 0xdf30b3e64bf1f65bULL}, {23405, 0, 513, 0x8b3c82664c4b2f5bULL},
        {23405, 1, 513, 0x75faf91aae5d64bcULL}, {23405, 2, 513, 0xdb55b81c91e8dfbeULL},
        {23751, 0, 513, 0x4fc085dc64207b19ULL}, {23751, 1, 513, 0x82b7dd09fc30c531ULL},
        {23751, 2, 513, 0x59c5a0d00df3c647ULL}, {23970, 0, 513, 0x6f162be46563e7baULL},
        {23970, 1, 513, 0x918525fa2f73e147ULL}, {23970, 2, 513, 0x56bd035c7c001b65ULL},
        {32768, 0, 513, 0x4a6755c8832bc810ULL}, {32768, 1, 513, 0xfc8fa51b1686af54ULL},
        {32768, 2, 513, 0xb32c96f9ad5ad3d2ULL}, {33252, 0, 513, 0xd8e847852ea17272ULL},
        {33252, 1, 513, 0x5387097a42fe7071ULL}, {33252, 2, 513, 0xf9341bfb1db3befeULL},
        {33558, 0, 513, 0xae4b6e5de8194d06ULL}, {33558, 1, 513, 0xcd979cfda6bd331fULL},
        {33558, 2, 513, 0xfb666f32ffe5d6baULL}, {40960, 0, 489, 0x8b3f43aa241ab220ULL},
        {40960, 1, 489, 0xa638d6576602519bULL}, {40960, 2, 489, 0x005b930f5751bc29ULL},
        {41565, 0, 484, 0xd473618d02e8a4cfULL}, {41565, 1, 484, 0xdceaab8e47698d61ULL},
        {41565, 2, 484, 0x2d9bb9ae6eee02d8ULL}, {41947, 0, 481, 0xceb07bbc10857201ULL},
        {41947, 1, 481, 0x8f46b72c7b0a3394ULL}, {41947, 2, 481, 0xfde0cdf5a65cc532ULL},
    };
    for (const auto& fixture : fixtures) {
        gameboy::AudioMixer mixer;
        uint64_t digest = 0xcbf29ce484222325ULL;
        unsigned frames = 0;
        mixer.Reset(fixture.rate);
        mixer.SetOutput([&](float left, float right) {
            ++frames;
            for (float sample : {left, right}) {
                const auto value =
                    uint16_t(std::clamp<long>(std::lrint(double(sample) * 32768), -32768, 32767));
                for (unsigned shift : {0u, 8u}) {
                    digest ^= uint8_t(value >> shift);
                    digest *= 0x100000001b3ULL;
                }
            }
        });
        for (unsigned index = 0; index < 512; ++index) {
            if (index == 127 || index == 383)
                mixer.SetHandheldRate(index == 127 ? fixture.rate : 32768);
            int16_t left, right;
            if (fixture.pattern == 0) {
                left = (index & 1) ? -32768 : 32767;
                right = int16_t(int((index * 7919) % 65536) - 32768);
            } else if (fixture.pattern == 1) {
                left = 32767;
                right = -32768;
            } else {
                left = index == 8 ? 32767 : 0;
                right = index == 12 ? -32768 : 0;
            }
            mixer.WriteHandheld(left, right);
            if (index % 7 == 0)
                mixer.WriteHandheld(int16_t(-right), left);
            mixer.WriteConsole((index & 2) ? 24000 : -24000, (index & 4) ? 12000 : -12000);
        }
        Check(frames == fixture.frames && digest == fixture.digest,
              "Stereo mixing retains interpolation, clipping and both kinds of stream restart");
    }
}
} // namespace

int main() {
    try {
        ConsoleStartupEntropy();
        ConsoleReloadEntropy();
        CameraReloadEntropy();
        BoardMapping();
        ClockAndBattery();
        ConsoleExecutionAndFailedLoads();
        ConsoleClockBoundaries();
        ConsoleAudioHandoffs();
        StereoAudioMix();
        std::puts("Super Game Boy cartridge mapping, execution, clocks and battery checks passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
