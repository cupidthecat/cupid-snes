#include "ConsoleConfig.hpp"
#include "audio/SoundClockExpectations.hpp"
#include "snes/core/SnesCpu.hpp"

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

std::unique_ptr<Emulator> Machine() {
    std::vector<uint8_t> rom(0x8000, 0);
    std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
    rom[0x7fc0] = 'T';
    rom[0x7fd5] = 0x20;
    rom[0x7fd7] = 5;
    rom[0x7fdc] = 0xff;
    rom[0x7fdd] = 0xff;
    rom[0x7ffd] = 0x80;
    rom[0] = 0xdb; // STP, so only hardware events advance in frame tests.
    auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    std::string error;
    if (!emu->LoadCartridge(rom, &error))
        throw std::runtime_error(error);
    emu->GetCpu()->Step();
    return emu;
}

void ClockedTransfers() {
    auto emu = Machine();
    emu->GetCpu()->idle();
    const auto startedAt = emu->GetTiming().MasterClocksElapsed();
    auto& dma = emu->GetDma();
    auto& bus = emu->GetBus();
    std::vector<uint64_t> clocks;
    bus.Map(
        0, 0, 0x2110, 0x2110, [](uint32_t, uint8_t b) { return b; },
        [&](uint32_t, uint8_t) { clocks.push_back(emu->GetTiming().MasterClocksElapsed()); });
    auto& ch = dma.Channel(0);
    ch.writeControl(0);
    ch.sourceBank = 0x7e;
    ch.sourceAddress = 0xff00;
    ch.targetAddress = 0x10;
    ch.transferSize = 512;
    const auto cpuPc = emu->GetCpu()->regs().pc;
    dma.EnableDma(1);
    const uint32_t ownClocks = dma.RunDma();
    Check(ownClocks == 8 + 16 + 512 * 8 + 2 && clocks.size() == 512,
          "DMA alignment, transfer and resume cycle accounting");
    Check(clocks.front() == startedAt + 32 && clocks.back() > startedAt + 4000,
          "Writes follow alignment, startup and the first eight-clock read");
    Check(std::is_sorted(clocks.begin(), clocks.end()), "DMA time is monotonic");
    unsigned refreshes = 0;
    for (size_t i = 1; i < clocks.size(); ++i)
        if (clocks[i] - clocks[i - 1] == 48)
            ++refreshes;
    Check(refreshes == 3, "Refresh pauses DMA by 40 clocks on each crossed scanline");
    Check(emu->GetTiming().MasterClocksElapsed() == startedAt + ownClocks + 120,
          "Refresh is counted once");
    Check(ch.sourceBank == 0x7e && ch.sourceAddress == 0x100, "DMA wraps within the source bank");
    Check(emu->GetCpu()->regs().pc == cpuPc, "DMA stalls CPU execution");
    Check(emu->GetSmp().CycleCount() > 0, "APU advances during DMA");
}

void CoprocessorHandoff() {
    for (unsigned cycle : {6u, 8u, 12u})
        for (unsigned phase : {0u, 2u, 4u, 6u})
            for (bool reverse : {false, true}) {
                auto bus = std::make_unique<MemoryBus>();
                bus->MapWram();
                DmaController dma;
                dma.SetBus(bus.get());
                uint64_t elapsed = phase;
                uint32_t address = 0xc00000;
                uint8_t data = 0;
                std::vector<std::array<uint64_t, 2>> observed;
                dma.SetCpuCycleClocks(cycle);
                dma.SetCpuBusLatches(address, data);
                dma.SetClockQuery([&] { return elapsed; });
                dma.SetClockCallback([&](uint32_t clocks) { elapsed += clocks; });
                dma.SetCoprocessorSyncCallback([&] { observed.push_back({elapsed, address}); });
                auto& channel = dma.Channel(0);
                channel.writeControl(reverse ? 0x80 : 0);
                channel.sourceBank = 0x7e;
                channel.sourceAddress = 0x100;
                channel.targetAddress = 0x10;
                channel.transferSize = 1;
                dma.EnableDma(1);
                dma.RunDma();
                const auto resumedAt = 32 + cycle - (32 - phase) % cycle;
                const std::vector<std::array<uint64_t, 2>> expected{
                    {8, 0xc00000}, {28, 0x7e0100}, {32, 0x7e0100}, {resumedAt, 0x7e0100}};
                Check(observed == expected, "DMA synchronizes cartridge work at alignment, both "
                                            "bus phases and CPU handoff");
            }
}

void PaletteClockUnits() {
    for (unsigned start : {200u, 1200u}) {
        auto emu = Machine();
        auto& bus = emu->GetBus();
        auto& dma = emu->GetDma();
        bus.Write(0x2100, 15);
        bus.Write(0x2121, 5);
        bus.Write(0x7e0000, 0x1f);
        bus.Write(0x7e0001, 0);
        emu->GetTiming().Tick(uint32_t(1364 + start - emu->GetTiming().MasterClocksElapsed()));
        auto& ch = dma.Channel(0);
        ch.writeControl(0);
        ch.sourceBank = 0x7e;
        ch.sourceAddress = 0;
        ch.targetAddress = 0x22;
        ch.transferSize = 2;
        dma.EnableDma(1);
        dma.RunDma();
        Check(emu->GetPpu().CgramData()[5] == (start == 1200 ? 0x1f : 0),
              "CGRAM gating uses master clocks: active writes redirect, HBlank writes use CGADD");
        Check(emu->GetPpu().CgramData()[0] == (start == 200 ? 0x1f : 0),
              "Active CGRAM write uses the rendering latch");
    }
}

void HdmaInterruptsDma() {
    for (bool sameChannel : {false, true}) {
        auto emu = Machine();
        auto& dma = emu->GetDma();
        auto& bus = emu->GetBus();
        // Let the scanline-zero setup point pass before manually initializing
        // HDMA; automatic setup is covered separately below.
        while (emu->GetTiming().HCounter() < 18)
            emu->GetCpu()->idle();
        std::vector<uint8_t> output;
        bus.Map(
            0, 0, 0x2110, 0x2110, [](uint32_t, uint8_t b) { return b; },
            [&](uint32_t, uint8_t b) { output.push_back(b); });
        auto& hdma = dma.Channel(sameChannel ? 0 : 1);
        hdma.writeControl(0);
        hdma.sourceBank = 0x7e;
        hdma.sourceAddress = 0x1000;
        hdma.targetAddress = 0x10;
        bus.Write(0x7e1000, 0x81);
        bus.Write(0x7e1001, 0xb9);
        bus.Write(0x7e1002, 0);
        dma.EnableHdma(sameChannel ? 1 : 2);
        dma.HdmaReset();
        dma.HdmaSetup();
        auto& ch = dma.Channel(0);
        ch.writeControl(0);
        ch.sourceBank = 0x7e;
        ch.sourceAddress = 0x2000;
        ch.targetAddress = 0x10;
        ch.transferSize = 256;
        std::fill_n(bus.WramData() + 0x2000, 256, 0x37);
        dma.EnableDma(1);
        dma.RunDma();
        auto marker = std::find(output.begin(), output.end(), 0xb9);
        Check(marker != output.end() && marker != output.begin(), "HDMA runs after some DMA bytes");
        if (sameChannel) {
            Check(marker + 1 == output.end() && ch.transferSize > 0 && ch.transferSize < 256,
                  "HDMA cancels DMA on its own channel with remaining count intact");
        } else {
            Check(marker + 1 != output.end() && output.size() == 257 && ch.transferSize == 0,
                  "Another channel resumes DMA after HDMA");
        }
    }
}

void LongDmaAudio() {
    struct Output final : IAudioOutput {
        AudioBuffer last;
        unsigned calls = 0;

        void Submit(const AudioBuffer& audio) override {
            last = audio;
            ++calls;
        }
    } output;

    auto emu = Machine();
    emu->AttachAudioOutput(&output);
    auto& dsp = emu->GetDsp();
    dsp.Write(Dsp::kFlg, 0x3f);
    dsp.Write(Dsp::kMVolL, 0x7f);
    dsp.Write(Dsp::kMVolR, 0x7f);
    dsp.Write(Dsp::kVolL, 0x7f);
    dsp.Write(Dsp::kVolR, 0x7f);
    dsp.Write(Dsp::kAdsr0, 0);
    dsp.Write(Dsp::kGain, 0x7f);
    dsp.Write(Dsp::kDir, 0x20);
    dsp.Write(Dsp::kNon, 1);
    dsp.Write(Dsp::kKon, 1);
    auto& dma = emu->GetDma();
    for (unsigned i = 0; i < 8; ++i) {
        auto& channel = dma.Channel(i);
        channel.writeControl(8); // Fixed source, A-to-B.
        channel.sourceBank = 0x7e;
        channel.sourceAddress = 0;
        channel.targetAddress = 0x10;
        channel.transferSize = 0; // 65536 bytes.
    }
    dma.EnableDma(0xff);
    uint64_t delivered = 0;
    bool audible = false;
    for (unsigned frame = 0; frame < 15; ++frame) {
        emu->StepFrame();
        const auto generated =
            sound_clock_test::UnityStreamFrames(emu->GetSmp().HardwareClockCount());
        Check(output.calls == frame + 1 && output.last.sampleRate == AudioTiming::SampleRate &&
                  output.last.interleavedStereo.size() == (generated - delivered) * 2 &&
                  generated - delivered < 700,
              "A suspended DMA delivers every DSP sample in its own field");
        audible |= std::any_of(
            output.last.interleavedStereo.begin(), output.last.interleavedStereo.end(),
            [](float sample) { return sample > 1.0f / 32768 || sample < -1.0f / 32768; });
        delivered = generated;
    }
    const auto before = emu->GetSmp().HardwareClockCount();
    // Output occurs at phase 27, followed by one output every 32 SMP clocks.
    const auto generated = sound_clock_test::UnityStreamFrames(before);
    Check(!dma.AnyDmaEnabled() && generated > 2048 && audible,
          "Long DMA completes with audible samples preserved across its fields");
    emu->StepFrame();
    const auto next =
        sound_clock_test::UnityStreamFrames(emu->GetSmp().HardwareClockCount()) - generated;
    Check(output.calls == 16 && output.last.interleavedStereo.size() == next * 2 && next < 700,
          "The next frame contains only newly generated samples, with no stale overflow");
}

void DividerAlignment() {
    const std::array<uint32_t, 4> expectedDuration{36, 36, 30, 30};
    for (unsigned phase = 0; phase < 8; phase += 2) {
        auto bus = std::make_unique<MemoryBus>();
        DmaController dma;
        dma.SetBus(bus.get());
        uint64_t elapsed = phase;
        uint64_t readAt = 0, writeAt = 0;
        dma.SetClockCallback([&](uint32_t clocks) { elapsed += clocks; });
        dma.SetClockQuery([&]() { return elapsed; });
        bus->Map(
            0x40, 0x40, 0, 0,
            [&](uint32_t, uint8_t) {
                readAt = elapsed;
                return uint8_t(0x6b);
            },
            [](uint32_t, uint8_t) {});
        bus->Map(
            0, 0, 0x2110, 0x2110, [](uint32_t, uint8_t value) { return value; },
            [&](uint32_t, uint8_t value) {
                Check(value == 0x6b, "DMA carries the sampled byte");
                writeAt = elapsed;
            });
        auto& channel = dma.Channel(0);
        channel.writeControl(0);
        channel.sourceBank = 0x40;
        channel.sourceAddress = 0;
        channel.targetAddress = 0x10;
        channel.transferSize = 1;
        dma.EnableDma(1);
        Check(dma.RunDma() == expectedDuration[phase / 2],
              "DMA returns at the next CPU cycle boundary");
        Check(elapsed == phase + expectedDuration[phase / 2],
              "Reported DMA duration matches hardware time");
        Check(readAt == 28 && writeAt == 32,
              "Divider alignment puts the read and write on distinct bus phases");
    }
}

void TimingBatchInvariance() {
    std::vector<uint64_t> expected;
    Timing batched, split;
    batched.onDramRefresh = [&]() { expected.push_back(batched.MasterClocksElapsed()); };
    std::vector<uint64_t> actual;
    split.onDramRefresh = [&]() { actual.push_back(split.MasterClocksElapsed()); };
    unsigned setups = 0;
    split.onHdmaSetup = [&]() {
        Check(split.HCounter() == 12, "Initial HDMA setup follows the divider phase");
        ++setups;
    };
    batched.Tick(4092);
    for (unsigned i = 0; i < 4092; ++i)
        split.Tick(1);
    Check(expected == std::vector<uint64_t>{538, 1898, 3266},
          "DRAM refresh alternates 538/534 on normal scanlines");
    Check(actual == expected && setups == 1,
          "Odd clock batches preserve event positions and counts");
    Check(split.HCounter() == batched.HCounter() && split.VCounter() == batched.VCounter() &&
              split.MasterClocksElapsed() == batched.MasterClocksElapsed(),
          "Odd clock batches preserve counters");
    Timing shortLine;
    shortLine.Tick(357368 + 240 * 1364);
    Check(shortLine.VCounter() == 240 && shortLine.HPeriod() == 1360,
          "Test reaches the odd-field short line");
    const auto refresh = shortLine.DramRefreshPosition();
    shortLine.Tick(1360);
    Check(shortLine.DramRefreshPosition() == refresh,
          "A short line preserves the refresh divider phase");
}

void CpuRevisionTiming() {
    for (uint8_t revision : {1, 2}) {
        const std::vector<uint64_t> expected = revision == 1
                                                   ? std::vector<uint64_t>{530, 1894, 3258}
                                                   : std::vector<uint64_t>{538, 1898, 3266};
        for (unsigned batch : {1, 2, 7, 4092}) {
            Timing timing(Region::NTSC, revision);
            std::vector<uint64_t> refresh;
            unsigned setup = 0;
            timing.onDramRefresh = [&]() { refresh.push_back(timing.MasterClocksElapsed()); };
            timing.onHdmaSetup = [&]() {
                Check(timing.HCounter() == (revision == 1 ? 20 : 12),
                      "CPU revision selects the initial HDMA setup edge");
                ++setup;
            };
            for (unsigned elapsed = 0; elapsed < 4092;) {
                const unsigned clocks = std::min(batch, 4092 - elapsed);
                timing.Tick(clocks);
                elapsed += clocks;
            }
            Check(refresh == expected && setup == 1,
                  "CPU revision selects refresh timing across odd and even clock batches");
            timing.Reset();
            Check(timing.CpuRevision() == revision &&
                      timing.DramRefreshPosition() == (revision == 1 ? 530 : 538) &&
                      timing.HdmaSetupPosition() == (revision == 1 ? 20 : 12),
                  "Timing reset preserves the selected CPU hardware");
        }
    }
}

std::vector<uint8_t> RefreshProgram(unsigned nops, bool pal) {
    std::vector<uint8_t> rom(0x8000, 0xea);
    unsigned position = nops;
    for (uint8_t byte : {0xadu, 0x37u, 0x21u, 0xadu, 0x3cu, 0x21u, 0x8fu, 0u,   1u,
                         0x7eu, 0xadu, 0x3cu, 0x21u, 0x29u, 1u,    0x8fu, 1u,   1u,
                         0x7eu, 0xa9u, 0xa5u, 0x8fu, 15u,   1u,    0x7eu, 0xdbu})
        rom[position++] = byte;
    std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
    rom[0x7fc0] = 'T';
    rom[0x7fd5] = 0x20;
    rom[0x7fd6] = 0;
    rom[0x7fd7] = 5;
    rom[0x7fd8] = 0;
    rom[0x7fd9] = pal ? 2 : 1;
    rom[0x7fdc] = rom[0x7fdd] = 0xff;
    rom[0x7fde] = rom[0x7fdf] = 0;
    rom[0x7ffc] = 0;
    rom[0x7ffd] = 0x80;
    return rom;
}

void CpuRevisionBusTiming() {
    // Only the 23-NOP program samples the first refresh boundary differently.
    constexpr std::array<uint8_t, 16> expected{116, 119, 123, 126, 130, 133, 147, 150,
                                               154, 157, 161, 164, 168, 171, 175, 178};
    for (uint8_t requested : {0, 1, 2, 3, 255}) {
        const uint8_t revision = requested < 1 ? 1 : requested > 2 ? 2 : requested;
        for (bool pal : {false, true}) {
            for (unsigned index = 0; index < expected.size(); ++index) {
                auto config = snes::tests::DeterministicConsoleConfig();
                config.cpuRevision = requested;
                auto machine = std::make_unique<Emulator>(config);
                Check(machine->LoadCartridge(RefreshProgram(index + 18, pal)),
                      "CPU revision timing program loads");
                for (bool warm : {false, true}) {
                    if (warm)
                        Check(machine->Reset(), "CPU revision console resets");
                    Check(
                        machine->GetTiming().CpuRevision() == revision &&
                            (machine->GetBus().Read(0x4210) & 15) == revision,
                        "Configured CPU hardware and version readback agree after power and reset");
                    machine->StepFrame();
                    const auto* data = machine->GetBus().WramData() + 0x100;
                    const uint8_t h = revision == 1 && index == 5 ? 143 : expected[index];
                    Check(data[0] == h && data[1] == 0 && data[15] == 0xa5,
                          "CPU revision changes actual bus timing at the refresh boundary");
                }
            }
        }
    }
}

void CpuRevisionFieldTiming() {
    struct Profile {
        bool pal, interlace;
        std::array<uint16_t, 9> setup1, setup2, refresh2;
    };

    constexpr std::array profiles{Profile{false,
                                          false,
                                          {20, 20, 20, 16, 16, 20, 20, 16, 16},
                                          {12, 12, 12, 16, 16, 12, 12, 16, 16},
                                          {538, 534, 534, 538, 538, 534, 534, 538, 538}},
                                  Profile{false,
                                          true,
                                          {20, 20, 20, 20, 16, 16, 20, 20, 16},
                                          {12, 12, 12, 12, 16, 16, 12, 12, 16},
                                          {538, 534, 534, 534, 538, 538, 534, 534, 538}},
                                  Profile{true,
                                          false,
                                          {20, 20, 20, 20, 20, 20, 20, 20, 20},
                                          {12, 12, 12, 12, 12, 12, 12, 12, 12},
                                          {538, 534, 534, 534, 534, 534, 534, 534, 534}},
                                  Profile{true,
                                          true,
                                          {20, 20, 20, 16, 20, 16, 20, 16, 20},
                                          {12, 12, 12, 16, 12, 16, 12, 16, 12},
                                          {538, 534, 534, 538, 534, 538, 534, 538, 534}}};
    for (const auto& profile : profiles) {
        auto rom = RefreshProgram(0, profile.pal);
        const std::array<uint8_t, 11> code{
            0xa9, 0x80, 0x8d, 0, 0x21, 0xa9, uint8_t(profile.interlace), 0x8d, 0x33, 0x21, 0xdb};
        std::copy(code.begin(), code.end(), rom.begin());
        for (uint8_t revision : {1, 2}) {
            auto config = snes::tests::DeterministicConsoleConfig();
            config.cpuRevision = revision;
            auto machine = std::make_unique<Emulator>(config);
            Check(machine->LoadCartridge(rom), "CPU field timing program loads");
            for (bool warm : {false, true}) {
                if (warm)
                    Check(machine->Reset(), "CPU field timing program resets");
                for (unsigned frame = 0; frame < profile.setup1.size(); ++frame) {
                    if (frame)
                        machine->StepFrame();
                    const auto& timing = machine->GetTiming();
                    Check(
                        timing.DramRefreshPosition() ==
                                (revision == 1 ? 530 : profile.refresh2[frame]) &&
                            timing.HdmaSetupPosition() ==
                                (revision == 1 ? profile.setup1[frame] : profile.setup2[frame]),
                        "CPU revision retains refresh and HDMA phase across short and long fields");
                }
            }
        }
    }
}

struct Video : IVideoOutput {
    VideoFrame frame;

    void Present(const VideoFrame& value) override {
        frame = value;
    }
};

void FieldTimingAndPresentation() {
    for (bool pal : {false, true}) {
        auto emu = Machine();
        auto& timing = emu->GetTiming();
        timing.SetRegion(pal ? Region::PAL : Region::NTSC);
        emu->GetBus().Write(0x2133, 0x09); // Interlace and pseudo-hires.
        Video video;
        emu->AttachVideoOutput(&video);
        for (unsigned frame = 1; frame <= 4; ++frame) {
            emu->StepFrame();
            Check(timing.FrameCount() == frame - 1, "StepFrame stops at each display event");
            Check(timing.VCounter() == 225 && timing.HCounter() == 0,
                  "The display deadline is exact inside a CPU cycle");
            const uint32_t height = frame == 1 ? 224 : 448;
            Check(video.frame.width == 512 && video.frame.height == height &&
                      video.frame.pixels.size() == 512 * height,
                  "Presentation retains hires pixels and latches interlace at the frame edge");
            Check(timing.Interlace() == (frame > 1),
                  "The timing counter samples the frame's latched display mode");
            const uint64_t completed = frame - 1;
            const uint64_t base = pal ? 312 : 262;
            const uint64_t following = completed ? completed - 1 : 0;
            const uint64_t expected = (completed ? base * 1364 : 0) +
                                      (following / 2) * ((base * 2 + 1) * 1364 + (pal ? 4 : 0)) +
                                      (following % 2 ? base * 1364 + (pal ? 4 : 0) : 0) +
                                      225 * 1364;
            Check(timing.MasterClocksElapsed() == expected,
                  "Interlaced long and short fields have correct duration");
        }
    }
}
} // namespace

int main() {
    try {
        CoprocessorHandoff();
        ClockedTransfers();
        PaletteClockUnits();
        HdmaInterruptsDma();
        LongDmaAudio();
        DividerAlignment();
        TimingBatchInvariance();
        CpuRevisionTiming();
        CpuRevisionBusTiming();
        CpuRevisionFieldTiming();
        FieldTimingAndPresentation();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    std::puts("DMA timing, HDMA interruption, and field presentation checks passed");
    return 0;
}
