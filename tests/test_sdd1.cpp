#include "ConsoleConfig.hpp"
#include "cartridge/Sdd1StreamFixture.hpp"
#include "snes/core/Sdd1.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace snes::core;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<uint8_t> Image(size_t size = 0x600000) {
    std::vector<uint8_t> rom(size);
    for (size_t p = 0; p < size; ++p)
        rom[p] = uint8_t(p >> 16);
    std::fill_n(rom.begin() + 0x7fc0, 64, 0);
    std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
    rom[0x7fc0] = 'T';
    rom[0x7fd5] = 0x32;
    rom[0x7fd6] = 0x45;
    rom[0x7fd7] = 13;
    rom[0x7fd8] = 3;
    rom[0x7fd9] = 1;
    rom[0x7fdc] = 0xff;
    rom[0x7fdd] = 0xff;
    rom[0x7ffd] = 0x80;
    rom[0] = 0x80;
    rom[1] = 0xfe; // BRA to self.
    return rom;
}

std::unique_ptr<Emulator> Load(const std::vector<uint8_t>& rom) {
    auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    std::string error;
    if (!emu->LoadCartridge(rom, &error))
        throw std::runtime_error(error);
    return emu;
}

void DecoderVectors() {
    // Digests of complete outputs, including odd lengths and all context modes.
    constexpr uint32_t expected[] = {0x7bd0036e, 0x72d5042a, 0x46e81486, 0x5f2cd4bf,
                                     0xf5ebf5a6, 0x2d4bf27e, 0xc46b0020, 0x1a2e5d2d,
                                     0x991ea7c5, 0xf2e5a48d, 0x28741861, 0x7498a2c4,
                                     0x319c80e3, 0x1af8e407, 0xf55461e7, 0xb601a7b4};
    std::vector<uint8_t> input(524300);
    uint32_t rng = 1;
    for (auto& byte : input) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        byte = uint8_t(rng);
    }
    for (unsigned mode = 0; mode < 16; ++mode) {
        input[0] = uint8_t((input[0] & 15) | (mode << 4));
        uint32_t digest = 2166136261u;
        for (unsigned length : {1u, 2u, 17u, 64u, 257u, 65536u}) {
            const auto output = DecompressSdd1([&](uint32_t p) { return input.at(p); }, length);
            Check(output.size() == length, "Decoder output length");
            for (auto byte : output) {
                digest ^= byte;
                digest *= 16777619u;
            }
        }
        Check(digest == expected[mode], "Decoder output digest");
        const auto zeros =
            DecompressSdd1([mode](uint32_t p) { return uint8_t(p ? 0 : mode << 4); }, 65536);
        Check(std::all_of(zeros.begin(), zeros.end(), [](uint8_t b) { return b == 0; }),
              "Zero runs");
    }
}

void MappingAndSaves() {
    auto rom = Image();
    auto emu = Load(rom);
    auto& bus = emu->GetBus();
    Check(emu->LoadedCartridge()->Header().mapping == MappingType::Sdd1, "S-DD1 mapper detection");
    for (unsigned window = 0; window < 4; ++window) {
        const unsigned address = 0xc01234 + (window << 20);
        Check(bus.Read(address) == window * 16, "Default MMC bank");
        for (unsigned bank = 0; bank < 16; ++bank) {
            bus.Write(0x804804 + window, uint8_t(0x70 | bank));
            Check(bus.Read(0x004804 + window) == bank, "MMC register mirrors and readback");
            const unsigned physical = bank & 7;
            Check(bus.Read(address) == (physical < 6 ? physical : physical - 2) * 16,
                  "MMC ROM line mirrors");
        }
    }
    Check(bus.Read(0x038123) == rom[0x18123], "Lower LoROM window");
    Check(bus.Read(0x838123) == rom[0x18123], "Upper LoROM window");
    Check(bus.Read(0x718123) == 0xff, "Full RAM banks retain their upper address mirror");
    for (unsigned address : {0x401234u, 0x408123u, 0x601234u, 0x746123u}) {
        bus.SetOpenBus(0xa9);
        Check(bus.Read(address) == 0xa9, "Unmapped board addresses");
    }
    bus.Write(0x700123, 0x57);
    Check(bus.Read(0xa06123) == 0x57, "SRAM alias");
    bus.Write(0xbf6124, 0x83);
    const auto saved = emu->LoadedCartridge()->SramData();
    std::vector<uint8_t> bytes(saved.begin(), saved.end());
    Check(emu->LoadCartridge(rom), "Cartridge reload");
    emu->LoadSram(bytes);
    Check(bus.Read(0x700123) == 0x57 && bus.Read(0x700124) == 0x83, "Battery save round trip");
    Check(bus.Read(0x4804) == 0 && bus.Read(0x4807) == 3 && bus.Read(0x4801) == 0,
          "Reload resets registers");
    bus.Write(0x7e1234, 0xb6);
    Check(bus.Read(0x7e1234) == 0xb6, "WRAM priority");
    rom[0x7fd8] = 0;
    Check(emu->LoadCartridge(rom), "SRAM-less image loads");
    bus.SetOpenBus(0x64);
    Check(bus.Read(0x700123) == 0x64, "Unpopulated SRAM is open bus");
}

void ControllerRegistersAndStreams() {
    auto rom = Image();
    rom[0x4802] = 0x6d;
    rom[0x4803] = 0xb4;
    rom[0x4808] = 0x8e;
    rom[0x480f] = 0xf1;
    std::fill(rom.begin() + 0x100000, rom.begin() + 0x180000, 0);
    rom[0x100000] = 0xc0;
    auto emu = Load(rom);
    auto& bus = emu->GetBus();
    bus.Write(0x4804, 0xf9);
    Check(bus.Read(0x4804) == 0x89, "MMC readback retains only connected register bits");
    for (unsigned address : {0x4802u, 0x4803u, 0x4808u, 0x480fu}) {
        Check(bus.Read(address) == rom[address],
              "Unused I/O locations read physical cartridge ROM");
        bus.Write(address, 0);
        Check(bus.Read(address | 0x800000) == rom[address],
              "Unused I/O writes leave ROM unchanged");
    }
    bus.Write(0x006123, 0x5a);
    for (unsigned address : {0x806123u, 0x706123u, 0x70e123u, 0x736123u})
        Check(bus.Read(address) == 0x5a, "All battery RAM windows share the same physical memory");
    for (unsigned address : {0x601234u, 0x748123u, 0x7d8123u}) {
        bus.SetOpenBus(0xa9);
        Check(bus.Read(address) == 0xa9, "Unpopulated full banks are open bus");
    }
    bus.Write(0x4805, 0x80);
    bus.Write(0x4807, 0x80);
    Check(bus.Read(0x218123) == rom[0x8123] && bus.Read(0xa18123) == rom[0x8123],
          "The two program-window mirror controls act independently of MMC data banks");
    bus.Write(0x4805, 1);
    bus.Write(0x4807, 3);
    const auto program = [&](uint16_t count) {
        bus.Write(0x4302, 0);
        bus.Write(0x4303, 0);
        bus.Write(0x4304, 0xd0);
        bus.Write(0x4305, uint8_t(count));
        bus.Write(0x4306, uint8_t(count >> 8));
        bus.Write(0x4800, 1);
        bus.Write(0x4801, 1);
    };
    program(3);
    Check(bus.Read(0xd00000) == 0 && bus.Read(0xd00000) == 0 && bus.Read(0x4801) == 1,
          "Matching CPU reads consume decoded bytes before a DMA request");
    Check(bus.Read(0xd00000) == 0 && bus.Read(0x4801) == 0 && bus.Read(0xd00000) == 0xc0,
          "The final decoded read disarms only the matching channel");
    Check(bus.Read(0x4305) == 3, "The cartridge count is independent of the CPU DMA count");

    auto& dma = emu->GetDma();
    dma.SetClockCallback({});
    dma.SetClockQuery({});
    dma.SetCoprocessorSyncCallback({});
    unsigned writes = 0;
    bus.Map(
        0, 0, 0x2100, 0x21ff, [](uint32_t, uint8_t) { return uint8_t(0); },
        [&](uint32_t, uint8_t value) {
            Check(value == 0, "Interrupted DMA still reads the cartridge decoder");
            ++writes;
        });
    program(4);
    bus.Write(0x4300, 8);
    bus.Write(0x4301, 0x20);
    dma.SetBoundaryCallback([&] {
        if (writes == 2)
            dma.Channel(0).dmaEnable = false;
    });
    dma.EnableDma(1);
    dma.RunDma();
    Check(writes == 2 && bus.Read(0x4801) == 1, "Cancelled DMA retains the armed partial stream");
    Check(bus.Read(0xd00000) == 0 && bus.Read(0x4801) == 1 && bus.Read(0xd00000) == 0 &&
              bus.Read(0x4801) == 0,
          "Later reads finish the retained cartridge count");
    emu->Reset();
    Check(bus.Read(0x4800) == 0 && bus.Read(0x4801) == 0 && bus.Read(0x4805) == 1 &&
              bus.Read(0x006123) == 0x5a,
          "Reset clears stream controls while retaining battery RAM");
}

void DmaDecompression() {
    auto rom = Image();
    // Header $C0 followed by zero codewords: packed output is entirely zero.
    std::fill(rom.begin() + 0x100000, rom.begin() + 0x180000, 0);
    rom[0x100000] = 0xc0;
    auto emu = Load(rom);
    auto& bus = emu->GetBus();
    auto& dma = emu->GetDma();
    dma.SetClockCallback({}); // Isolate data-path checks from video timing.
    dma.SetClockQuery({});
    std::vector<std::pair<uint32_t, uint8_t>> writes;
    bus.Map(
        0, 0, 0x2100, 0x21ff, [](uint32_t, uint8_t) { return uint8_t(0x69); },
        [&](uint32_t p, uint8_t b) { writes.emplace_back(p, b); });
    constexpr unsigned pattern[8][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
                                        {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1}};
    for (unsigned channel = 0; channel < 8; ++channel) {
        for (unsigned mode = 0; mode < 8; ++mode) {
            auto& ch = dma.Channel(channel);
            ch.writeControl(uint8_t(mode | 8));
            ch.targetAddress = 0x20;
            ch.sourceBank = 0xd0;
            ch.sourceAddress = 0;
            ch.transferSize = 17;
            const unsigned base = 0x4300 + channel * 16;
            bus.Write(base + 2, 0);
            bus.Write(base + 3, 0);
            bus.Write(base + 4, 0xd0);
            bus.Write(base + 5, 17);
            bus.Write(base + 6, 0);
            bus.Write(0x4800, 0);
            Check(bus.Read(0xd00000) == 0xc0, "Disabled channels return compressed ROM");
            bus.Write(0x4800, uint8_t(1u << channel));
            bus.Write(0x4801, 0xff);
            writes.clear();
            dma.EnableDma(uint8_t(1u << channel));
            Check(dma.RunDma() == 16 + 17 * 8, "Decompression DMA clocks");
            Check(writes.size() == 17, "Decompression DMA byte count");
            for (unsigned i = 0; i < writes.size(); ++i)
                Check(writes[i].second == 0 && writes[i].first == 0x2120 + pattern[mode][i % 4],
                      "DMA transfer pattern");
            Check(ch.sourceAddress == 0 && ch.transferSize == 0,
                  "Fixed source and completed count");
            Check(bus.Read(0x4801) == (0xff ^ (1u << channel)), "Only completed channel disarms");
        }
    }
    auto& ch = dma.Channel(0);
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        ch.writeControl(scenario == 2 ? 0 : 8);
        ch.sourceAddress = 0;
        ch.transferSize = 1;
        bus.Write(0x4302, 0);
        bus.Write(0x4303, 0);
        bus.Write(0x4304, 0xd0);
        bus.Write(0x4305, 1);
        bus.Write(0x4306, 0);
        bus.Write(0x4800, scenario == 0 ? 2 : 1);
        bus.Write(0x4801, scenario == 1 ? 2 : 1);
        writes.clear();
        dma.EnableDma(1);
        dma.RunDma();
        Check(writes.size() == 1 && writes[0].second == (scenario == 2 ? 0 : 0xc0),
              "Both channel masks select a stream independently of the DMA addressing mode");
    }
    ch.writeControl(8);
    ch.sourceAddress = 0;
    ch.transferSize = 0;
    bus.Write(0x4302, 0);
    bus.Write(0x4303, 0);
    bus.Write(0x4304, 0xd0);
    bus.Write(0x4305, 0);
    bus.Write(0x4306, 0);
    bus.Write(0x4800, 1);
    bus.Write(0x4801, 1);
    writes.clear();
    dma.EnableDma(1);
    Check(dma.RunDma() == 16 + 65536 * 8 && writes.size() == 65536, "Zero count transfers 64 KiB");
    Check(std::all_of(writes.begin(), writes.end(), [](auto w) { return w.second == 0; }),
          "Complete 64 KiB output");

    // Reverse DMA writes SRAM without consuming a compressed stream.
    ch.writeControl(0x88);
    ch.sourceBank = 0x70;
    ch.sourceAddress = 0;
    ch.transferSize = 1;
    bus.Write(0x4801, 1);
    dma.EnableDma(1);
    dma.RunDma();
    Check(bus.Read(0x700000) == 0x69 && bus.Read(0x4801) == 1, "Reverse DMA bypasses decoder");
}

void LargeImages() {
    for (size_t size : {0x800000u, 0xc00000u, 0x1000000u}) {
        auto rom = Image(size);
        rom[0x7fd7] = size > 0x800000 ? 14 : 13;
        auto emu = Load(rom);
        auto& bus = emu->GetBus();
        Check(emu->LoadedCartridge()->Header().mapping == MappingType::Sdd1,
              "Large S-DD1 images retain the controller selected by their header");
        for (unsigned bank = 0; bank < 16; ++bank) {
            unsigned physical = bank;
            if (size == 0x800000)
                physical &= 7;
            if (size == 0xc00000 && bank >= 12)
                physical -= 4;
            bus.Write(0x4804, uint8_t(bank));
            Check(bus.Read(0xc01234) == rom[(physical << 20) | 0x1234],
                  "All four MMC bank bits select the populated ROM blocks");
        }
        bus.Write(0x006123, 0x52);
        Check(bus.Read(0x706123) == 0x52, "Large ROM size does not remove battery RAM");
    }
}

void ClockedRomMirrors() {
    auto rom = Image();
    rom[0x101234] = 0x2a;
    rom[0x401234] = 0x6d;
    rom[0x501234] = 0x91;
    size_t position = 0;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (auto byte : bytes)
            rom[position++] = byte;
    };
    emit({0x78, 0xa9, 0x80, 0x8d, 0x00, 0x21});
    unsigned result = 0;
    for (unsigned window = 0; window < 4; ++window) {
        for (uint8_t bank : {5, 6, 7, 15}) {
            emit({0xa9, bank, 0x8f, uint8_t(4 + window), 0x48, uint8_t(window & 1 ? 0x80 : 0)});
            emit({0xaf, 0x34, 0x12, uint8_t(0xc0 + window * 0x10)});
            emit({0x8f, uint8_t(result++), 0x00, 0x7f});
        }
    }
    emit({0xa9, 0xa5, 0x8f, 0x10, 0x00, 0x7f, 0xdb});

    for (bool pal : {false, true}) {
        rom[0x7fd9] = pal ? 2 : 1;
        for (auto model : {Ppu::RenderModel::Progressive, Ppu::RenderModel::Scanline}) {
            auto config = snes::tests::DeterministicConsoleConfig();
            config.ppu.model = model;
            auto emu = std::make_unique<Emulator>(config);
            Check(emu->LoadCartridge(rom), "Clocked ROM-mirror guest loads");
            for (bool warm : {false, true}) {
                if (warm)
                    Check(emu->Reset(), "Clocked ROM-mirror guest resets");
                for (unsigned index = 0; index <= 16; ++index)
                    emu->GetBus().Write(0x7f0000 + index, 0);
                emu->StepFrame();
                const auto* wram = emu->GetBus().WramData();
                Check(wram[0x10010] == 0xa5, "Clocked ROM-mirror guest completes after each reset");
                constexpr uint8_t expected[] = {0x91, 0x6d, 0x91, 0x91};
                for (unsigned index = 0; index < 16; ++index)
                    Check(wram[0x10000 + index] == expected[index & 3],
                          "Unpopulated MMC banks retain bounded non-power-of-two ROM mirroring");
            }
        }
    }
}

void CompressedBankBoundary() {
    auto rom = Image();
    std::fill(rom.begin() + 0x200000, rom.begin() + 0x201000, 0);
    std::fill(rom.begin() + 0x500000, rom.begin() + 0x501000, 0xff);
    rom[0x4ffffe] = 0xc0;
    rom[0x4fffff] = 0;
    auto emu = Load(rom);
    auto& bus = emu->GetBus();
    bus.Write(0x4804, 4);
    bus.Write(0x4805, 2);
    bus.Write(0x4302, 0xfe);
    bus.Write(0x4303, 0xff);
    bus.Write(0x4304, 0xcf);
    bus.Write(0x4305, 1);
    bus.Write(0x4306, 1);
    bus.Write(0x4800, 1);
    bus.Write(0x4801, 1);
    for (unsigned i = 0; i < 257; ++i)
        Check(bus.Read(0xcffffe) == 0,
              "Compressed input crosses the MMC boundary into the selected ROM bank");
    Check(bus.Read(0x4801) == 0, "Cross-window stream completes at its programmed count");
}

void ClockedStreams() {
    // Keep CPU, DMA, HDMA and PPU clocks connected while the guest drives the stream.
    using sdd1_test::Transfer;

    struct Case {
        Transfer transfer;
        unsigned mode;
        uint32_t data;
        std::array<uint32_t, 2> ntsc, pal;
    };

    constexpr Case cases[] = {
        {Transfer::Switch, 0, 0xaefac739, {0x9ade165d, 0xd4f975fd}, {0x8da3ed8d, 0x21cc3dad}},
        {Transfer::Priority, 5, 0x08e470e1, {0xe0d571fd, 0xf101d35d}, {0xba4e942d, 0x3dd49b0d}},
        {Transfer::Alternating, 10, 0xaefac739, {0xcf5a861e, 0x6a9e06be}, {0x2804794e, 0x7f5a91ee}},
        {Transfer::Fixed, 15, 0x93a85dbc, {0x4d2fc7a9, 0x2e40d1c9}, {0x89722b99, 0xac4b3ab9}},
        {Transfer::Increment, 1, 0xf3f5abe0, {0x311487b4, 0xb449a054}, {0xc68d7c24, 0x29be2844}},
        {Transfer::Decrement, 6, 0x6d96aa6e, {0x3c73231f, 0x980a5fff}, {0x0e7479af, 0x12c08e0f}},
        {Transfer::Zero, 11, 0xc8b77acd, {0xda6e4565, 0x62bda8c5}, {0xb17e9fe9, 0x14fd2dc9}},
        {Transfer::Boundary, 12, 0x18cf15b6, {0x5e9bfb6c, 0xd7937e0c}, {0xbc1b9adc, 0x236bc67c}},
        {Transfer::HdmaShared, 3, 0x7483e349, {0x21cffca9, 0xbb758c09}, {0x8d0ef0b4, 0x9d3b5214}},
        {Transfer::HdmaSame, 9, 0x7e6b3f99, {0xdab2d073, 0xca866f13}, {0x23c6b672, 0x8a212712}},
    };
    const auto digest = [](const uint8_t* bytes, unsigned count) {
        uint32_t hash = 2166136261u;
        for (unsigned index = 0; index < count; ++index)
            hash = (hash ^ bytes[index]) * 16777619u;
        return hash;
    };
    for (auto model : {Ppu::RenderModel::Progressive, Ppu::RenderModel::Scanline}) {
        for (bool pal : {false, true}) {
            for (const auto& test : cases) {
                const auto program = sdd1_test::Stream(test.transfer, test.mode, pal);
                auto config = snes::tests::DeterministicConsoleConfig();
                config.ppu.model = model;
                auto emu = std::make_unique<Emulator>(config);
                Check(emu->LoadCartridge(program.image), "Clocked S-DD1 guest loads");
                for (bool warm : {false, true}) {
                    if (warm)
                        Check(emu->Reset(), "Clocked S-DD1 guest resets");
                    for (unsigned field = 0; field < 4; ++field)
                        emu->StepFrame();
                    const auto* wram = emu->GetBus().WramData();
                    Check(wram[0x11fff] == 0xa5, "Clocked S-DD1 guest completes");
                    Check(digest(wram, 65536) == test.data, "Clocked DMA destination digest");
                    const unsigned renderer = model == Ppu::RenderModel::Scanline ? 1 : 0;
                    Check(digest(wram + 65536, program.observations) ==
                              (pal ? test.pal : test.ntsc)[renderer],
                          "Clocked stream, channel register and beam-counter digest");
                }
            }
        }
    }
}
} // namespace

int main() {
    try {
        DecoderVectors();
        ControllerRegistersAndStreams();
        MappingAndSaves();
        DmaDecompression();
        LargeImages();
        ClockedRomMirrors();
        CompressedBankBoundary();
        ClockedStreams();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    std::puts("S-DD1 decoder, mapper, DMA, and save checks passed");
    return 0;
}
