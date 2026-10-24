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

std::vector<uint8_t> Rom(uint8_t chip = 0, bool pal = false) {
    std::vector<uint8_t> rom(0x100000);
    std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
    rom[0x7fc0] = 'T';
    rom[0x7fd5] = chip == 0x34 ? 0x23 : chip == 0x25 ? 0x30 : chip == 0x55 ? 0x35 : 0x20;
    rom[0x7fd6] = chip;
    rom[0x7fbf] = chip == 0xf3 ? 0x10 : 0;
    rom[0x7fd7] = 10;
    rom[0x7fd8] = chip == 0xf3 ? 0 : 3;
    rom[0x7fd9] = pal ? 2 : 1;
    rom[0x7fdc] = rom[0x7fdd] = 0xff;
    rom[0x7ffd] = 0x80;
    rom[0] = 0xdb;
    return rom;
}

std::unique_ptr<Emulator> Machine(uint8_t chip = 0, bool pal = false) {
    auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    Check(machine->LoadCartridge(Rom(chip, pal)), "Reset fixture loads");
    return machine;
}

struct Output final : IVideoOutput, IAudioOutput {
    unsigned frames = 0, blocks = 0;
    size_t samples = 0;

    void Present(const VideoFrame&) override {
        ++frames;
    }

    void Submit(const AudioBuffer& buffer) override {
        ++blocks;
        samples += buffer.interleavedStereo.size() / 2;
        Check(buffer.sampleRate == AudioTiming::SampleRate, "Reset retains the native audio rate");
        Check(buffer.interleavedStereo.size() < 1400, "Reset does not deliver stale queued audio");
    }
};

void RetainedMemoryAndRegisters() {
    for (bool pal : {false, true}) {
        auto machine = Machine(0, pal);
        machine->StepFrame();
        auto& bus = machine->GetBus();
        auto& ppu = machine->GetPpu();
        auto& smp = machine->GetSmp();
        auto& dsp = machine->GetDsp();
        for (size_t i = 0; i < MemoryBus::WramSize; ++i)
            bus.WramData()[i] = static_cast<uint8_t>(i * 73 + (i >> 7) + 0x35);
        for (size_t i = 0; i < 65536; ++i)
            smp.Ram()[i] = static_cast<uint8_t>(i * 29 + 0x45);
        for (size_t i = 0; i < Ppu::VramWords; ++i)
            ppu.VramData()[i] = static_cast<uint16_t>(i * 177 + 0x1253);
        std::fill_n(ppu.OamData(), Ppu::OamSize, 0xa5);
        std::fill_n(ppu.CgramData(), Ppu::CgramColors, 0x4567);
        std::vector<uint8_t> sram(machine->LoadedCartridge()->SramData().size(), 0x5a);
        machine->LoadSram(sram);
        for (unsigned i = 0; i < Dsp::RegisterCount; ++i)
            dsp.Write(static_cast<uint8_t>(i), static_cast<uint8_t>(i * 11 + 5));
        std::array<uint8_t, Dsp::RegisterCount> registers{};
        for (unsigned i = 0; i < registers.size(); ++i)
            registers[i] = dsp.Read(static_cast<uint8_t>(i));
        bus.Write(0x2181, 0x7b);
        bus.Write(0x2182, 0x45);
        bus.Write(0x2183, 1);
        bus.Write(0x420d, 1);
        bus.Write(0x4200, 0xb1);
        bus.Write(0x420b, 0xff);
        smp.PortWrite(0, 0x56);
        smp.Write(0xf4, 0x78);
        smp.Ram()[0xf4] = static_cast<uint8_t>(0xf4 * 29 + 0x45);
        ppu.GetIO().displayDisable = false;
        ppu.GetIO().overscan = ppu.GetIO().interlace = true;
        ppu.GetLatch().ppu1.mdr = ppu.GetLatch().ppu2.mdr = 0x25;
        auto* cpu = machine->GetCpu();
        auto* vram = ppu.VramData();
        Check(machine->Reset(), "Loaded console resets");
        Check(machine->GetCpu() == cpu && ppu.VramData() == vram,
              "Reset keeps attached subsystem storage stable");
        Check(machine->CurrentFrame() == 0 && machine->CurrentMasterCycles() == 0 &&
                  smp.HardwareClockCount() == 0 && cpu->Cycles() == 0,
              "Reset restarts all console clocks");
        for (size_t i = 0; i < MemoryBus::WramSize; ++i)
            Check(bus.WramData()[i] == static_cast<uint8_t>(i * 73 + (i >> 7) + 0x35),
                  "Warm reset retains every WRAM byte");
        for (size_t i = 0; i < 65536; ++i)
            Check(smp.Ram()[i] == static_cast<uint8_t>(i * 29 + 0x45),
                  "Warm reset retains every APURAM byte");
        for (size_t i = 0; i < Ppu::VramWords; ++i)
            Check(ppu.VramData()[i] == static_cast<uint16_t>(i * 177 + 0x1253),
                  "Warm reset retains every VRAM word");
        Check(std::equal(sram.begin(), sram.end(), machine->LoadedCartridge()->SramData().begin()),
              "Warm reset retains cartridge save memory");
        Check(std::all_of(ppu.OamData(), ppu.OamData() + Ppu::OamSize,
                          [](auto value) { return value == 0; }) &&
                  std::all_of(ppu.CgramData(), ppu.CgramData() + Ppu::CgramColors,
                              [](auto value) { return value == 0; }),
              "Warm reset clears OAM and palette memory");
        for (unsigned i = 0; i < registers.size(); ++i)
            Check(dsp.Read(static_cast<uint8_t>(i)) == (i == 0x6c ? 0xe0 : registers[i]),
                  "Warm reset retains DSP registers except FLG");
        Check(ppu.GetIO().displayDisable && !ppu.GetIO().overscan && !ppu.GetIO().interlace &&
                  ppu.GetIO().vramIncrementMode && ppu.GetLatch().ppu1.mdr == 0xff &&
                  ppu.GetLatch().ppu2.mdr == 0xff,
              "Reset restores PPU display, addressing and bus defaults");
        Check(bus.Speed(0x808000) == 8 && !machine->LoadedCartridge()->MemselFast() &&
                  !machine->GetDma().AnyDmaEnabled() && !machine->GetDma().AnyHdmaEnabled(),
              "Reset clears fast ROM and transfer requests");
        Check(smp.PortRead(0) == 0 && smp.Read(0xf4) == 0,
              "Reset clears both audio port directions");
        bus.Write(0x2180, 0x9a);
        Check(bus.WramData()[0] == 0x9a, "Reset clears the WRAM address latch");
        Output output;
        machine->AttachVideoOutput(&output);
        machine->AttachAudioOutput(&output);
        Check(machine->Reset(), "Repeated reset succeeds");
        for (unsigned i = 0; i < 3; ++i)
            machine->StepFrame();
        Check(output.frames == 3 && output.blocks == 3,
              "Reset retains output attachments across repeated fields");
        const auto clocks = smp.HardwareClockCount();
        Check(output.samples > 0 && output.samples == sound_clock_test::UnityStreamFrames(clocks) &&
                  dsp.Phase() == sound_clock_test::DspClocks(clocks) % 32,
              "Sound reset restarts stream priming and the DSP half-cycle phase");
        Check(machine->GetTiming().MasterClockHz() ==
                  (pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz),
              "Reset retains the cartridge region");
        std::array<uint8_t, Smp::RamSize> audioRam{};
        std::copy_n(smp.Ram(), audioRam.size(), audioRam.begin());
        Check(machine->LoadCartridge(Rom()), "A later cartridge load succeeds");
        Check(ppu.VramData()[0] == 0 && dsp.Read(0) == 0 &&
                  std::equal(audioRam.begin(), audioRam.end(), smp.Ram()),
              "Cartridge replacement clears video and DSP registers while retaining all audio RAM");
    }
}

void CancelSuspendedDma(bool powerCycle = false) {
    auto machine = Machine();
    unsigned writes = 0;
    machine->GetBus().Map(
        0, 0, 0x21e0, 0x21e0, [](uint32_t, uint8_t value) { return value; },
        [&](uint32_t, uint8_t) { ++writes; });
    auto& channel = machine->GetDma().Channel(0);
    channel.writeControl(8);
    channel.sourceBank = 0x7e;
    channel.targetAddress = 0xe0;
    channel.transferSize = 0;
    machine->GetDma().EnableDma(1);
    machine->StepFrame();
    Check(writes > 0 && writes < 65536 && machine->GetDma().InDma(),
          "Reset fixture pauses inside an unfinished DMA");
    const auto previous = writes;
    Check(powerCycle ? machine->PowerCycle() : machine->Reset(),
          "Console restarts during suspended DMA");
    for (unsigned i = 0; i < 3; ++i)
        machine->StepFrame();
    Check(writes == previous && !machine->GetDma().InDma() && machine->GetCpu()->regs().stp,
          "Cancelled DMA cannot publish an old byte after reset");
    machine->GetBus().Write(0x21e0, 0x44);
    Check(writes == previous + 1, "Reset retains existing bus handlers");
}

void CancelSuspendedInstruction(bool powerCycle = false) {
    auto machine = Machine();
    bool afterReset = false;
    unsigned writes = 0;
    const std::array<uint8_t, 7> program{0xa9, 0x5a, 0x8f, 0x34, 0x12, 0x7e, 0xdb};
    machine->GetBus().Map(
        0, 0, 0x8000, 0x8006,
        [&](uint32_t address, uint8_t) {
            return afterReset ? uint8_t{0xdb} : program[address - 0x8000];
        },
        [](uint32_t, uint8_t) {});
    machine->GetBus().Map(
        0x7e, 0x7e, 0x1234, 0x1234, [](uint32_t, uint8_t value) { return value; },
        [&](uint32_t, uint8_t) { ++writes; });
    machine->GetCpu()->Step();
    machine->GetCpu()->Step();
    Check(machine->GetCpu()->regs().pc == 0x8002, "CPU reset fixture is about to execute a store");
    auto& timing = machine->GetTiming();
    timing.Tick(225 * 1364 - timing.MasterClocksElapsed() - 4);
    machine->StepFrame();
    Check(writes == 0, "The display event suspends the CPU before its store completes");
    Check(powerCycle ? machine->PowerCycle() : machine->Reset(),
          "Console restarts during a suspended instruction");
    afterReset = true;
    machine->StepFrame();
    Check(writes == 0 && machine->GetCpu()->regs().stp,
          "Reset cancels the pending store before fetching the reset program");
}

void CartridgeMemory() {
    for (const uint8_t chip : {0x25, 0x34, 0x13}) {
        auto machine = Machine(chip);
        auto& bus = machine->GetBus();
        std::vector<uint8_t> bytes(machine->LoadedCartridge()->SramData().size(), 0x96);
        machine->LoadSram(bytes);
        if (chip == 0x34) {
            bus.Write(0x2229, 0xff);
            bus.Write(0x003123, 0x55);
            Check(bus.Read(0x003123) == 0x55, "SA-1 internal RAM is primed");
        }
        Check(machine->Reset(), "Cartridge device resets");
        Check(
            std::equal(bytes.begin(), bytes.end(), machine->LoadedCartridge()->SramData().begin()),
            "Device reset retains the complete physical cartridge RAM");
        if (chip == 0x34)
            Check(bus.Read(0x003123) == 0, "SA-1 reset clears internal RAM");
        if (chip == 0x13)
            Check(bus.Read(0x003030) == 0, "GSU reset stops execution and clears flags");
    }
}

void Obc1SaveBacking() {
    auto machine = Machine(0x25);
    Check(machine->LoadedCartridge()->Header().chip == EnhancementChip::Obc1,
          "The object RAM fixture selects the OBC1 board");
    auto& bus = machine->GetBus();
    bus.Write(0x006123, 0x5a);
    Check(machine->LoadedCartridge()->SramData()[0x123] == 0x5a,
          "OBC1 CPU writes reach cartridge saves");
    std::vector<uint8_t> save(8192);
    save[0x1ff5] = 0;
    save[0x1ff6] = 7;
    save[0x1c1c] = 0x6d;
    save[0x1e01] = 0x81;
    save[0x1ff0] = 0x33;
    machine->LoadSram(save);
    Check(bus.Read(0x007ff0) == 0x6d && bus.Read(0x807ff4) == 0x81,
          "Loading OBC1 saves restores object and base selection");
    bus.Write(0x807ff0, 0x72);
    Check(machine->LoadedCartridge()->SramData()[0x1c1c] == 0x72 &&
              machine->LoadedCartridge()->SramData()[0x1ff0] == 0x33,
          "OBC1 object writes change physical RAM without overwriting port bytes");
    Check(machine->Reset() && bus.Read(0x3f7ff0) == 0x72,
          "OBC1 retained selector bytes are reapplied at reset");
    for (uint32_t bank : {0x70u, 0x71u, 0xf0u, 0xf1u}) {
        Check(bus.Read((bank << 16) | 0x7ff0) == 0x72 && bus.Read((bank << 16) | 0xfff0) == 0x72,
              "OBC1 expansion-bank aliases select the same object registers");
        bus.Write((bank << 16) | 0xe123, 0x6e);
        Check(machine->LoadedCartridge()->SramData()[0x123] == 0x6e,
              "OBC1 high aliases write the shared save memory");
    }
    for (uint32_t address : {0x700123u, 0x708123u, 0x726123u, 0x72e123u, 0x406123u, 0xc06123u}) {
        bus.SetOpenBus(0x93);
        Check(bus.ReadCpu(address) == 0x93, "Unpopulated OBC1 windows leave the bus undriven");
    }
}

void ClockAndWorkRam() {
    Srtc clock([] { return int64_t{1'700'000'000}; });
    const auto saved = clock.Save();
    clock.Write(0x0e);
    Check(clock.Read() == 0, "RTC fixture enters command mode");
    clock.Reset();
    Check(clock.Save() == saved && clock.Read() == 15,
          "RTC reset preserves time and returns to the read protocol");
    for (unsigned i = 0; i < 13; ++i)
        Check(clock.Read() == saved[i], "Reset retains every RTC calendar digit");
    auto machine = Machine(0x55);
    Check(machine->LoadedCartridge()->Header().chip == EnhancementChip::Srtc,
          "The clock fixture selects the RTC board");
    machine->GetBus().Write(0x2801, 0x0e);
    Check(machine->GetBus().Read(0x2800) == 0, "Console RTC enters command mode");
    Check(machine->Reset() && machine->GetBus().Read(0x2800) == 15,
          "Console reset restarts the existing RTC protocol");

    machine = Machine(0xf3);
    Check(machine->LoadedCartridge()->Header().chip == EnhancementChip::Cx4,
          "The graphics fixture selects the Cx4 board");
    for (unsigned i = 0; i < 3072; ++i)
        machine->GetBus().Write(0x6000 + i, static_cast<uint8_t>(i * 17 + 0x59));
    machine->GetBus().Write(0x7f80, 0xa5);
    Check(machine->Reset(), "Graphics processor resets");
    for (unsigned i = 0; i < 3072; ++i)
        Check(machine->GetBus().Read(0x6000 + i) == static_cast<uint8_t>(i * 17 + 0x59),
              "Graphics processor reset retains its complete work RAM");
    Check(machine->GetBus().Read(0x7f80) == 0, "Graphics processor registers restart");
}

void ControllerAttachments() {
    struct Input final : IInputProvider {
        std::vector<int> players;

        InputState Poll(uint64_t) override {
            return {};
        }

        InputState PollController(int player, uint64_t) override {
            players.push_back(player);
            return InputState{.b = player == 3};
        }
    } input;

    auto machine = Machine();
    machine->AttachInputProvider(&input);
    machine->GetAutoJoypad().Ports().Configure(0, ControllerDevice::Multitap, {3, 4, 5, 6});
    machine->GetAutoJoypad().Ports().Configure(1, ControllerDevice::None);
    machine->GetBus().Write(0x4016, 1);
    machine->GetBus().Write(0x4016, 0);
    machine->GetBus().Read(0x4016);
    input.players.clear();
    Check(machine->Reset(), "Configured controller console resets");
    machine->GetBus().Write(0x4016, 1);
    machine->GetBus().Write(0x4016, 0);
    Check((machine->GetBus().Read(0x4016) & 1) == 1,
          "Reset restarts controller shifting with the attached input provider");
    for (int player : {3, 4, 5, 6})
        Check(std::find(input.players.begin(), input.players.end(), player) != input.players.end(),
              "Reset retains controller devices and player assignments");
    machine->StepFrame();
}

void PowerCycleStorage() {
    for (bool pal : {false, true}) {
        auto machine = Machine(0, pal);
        Output output;
        machine->AttachVideoOutput(&output);
        machine->AttachAudioOutput(&output);
        machine->StepFrame();
        auto& bus = machine->GetBus();
        auto& smp = machine->GetSmp();
        auto& ppu = machine->GetPpu();
        auto& dsp = machine->GetDsp();
        std::fill_n(bus.WramData(), MemoryBus::WramSize, 0xa6);
        std::fill_n(smp.Ram(), Smp::RamSize, 0xd3);
        std::fill_n(ppu.VramData(), Ppu::VramWords, 0x4567);
        dsp.Write(0, 0x7f);
        std::vector<uint8_t> save(machine->LoadedCartridge()->SramData().size(), 0x5a);
        machine->LoadSram(save);
        const auto* cart = machine->LoadedCartridge();
        Check(machine->PowerCycle() && machine->LoadedCartridge() == cart,
              "Power cycle restarts the loaded cartridge without replacing it");
        Check(std::all_of(bus.WramData(), bus.WramData() + MemoryBus::WramSize,
                          [](auto value) { return value == 0; }) &&
                  std::all_of(ppu.VramData(), ppu.VramData() + Ppu::VramWords,
                              [](auto value) { return value == 0; }) &&
                  dsp.Read(0) == 0,
              "Cold power initializes work RAM, video RAM and DSP registers");
        Check(std::all_of(smp.Ram(), smp.Ram() + Smp::RamSize,
                          [](auto value) { return value == 0xd3; }) &&
                  std::equal(save.begin(), save.end(), cart->SramData().begin()),
              "Cold power retains the physical audio RAM and cartridge battery");
        Check(machine->CurrentFrame() == 0 && machine->CurrentMasterCycles() == 0 &&
                  smp.HardwareClockCount() == 0 && machine->GetCpu()->Cycles() == 0,
              "Cold power queues the reset vector without executing a frame");
        const auto pictures = output.frames, blocks = output.blocks;
        machine->StepFrame();
        Check(output.frames == pictures + 1 && output.blocks == blocks + 1,
              "Cold power preserves audio and video attachments");
    }
    auto graphics = Machine(0xf3);
    for (unsigned index = 0; index < 3072; ++index)
        graphics->GetBus().Write(0x6000 + index, uint8_t(index * 17 + 0x59));
    Check(graphics->PowerCycle(), "The graphics cartridge powers again");
    for (unsigned index = 0; index < 3072; ++index)
        Check(graphics->GetBus().Read(0x6000 + index) == uint8_t(index * 17 + 0x59),
              "Cold power retains graphics cartridge work RAM");
}
} // namespace

int main() {
    try {
        auto empty = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
        Check(!empty->Reset() && !empty->PowerCycle() && !empty->Initialized(),
              "An empty console has nothing to restart");
        RetainedMemoryAndRegisters();
        CancelSuspendedDma();
        CancelSuspendedInstruction();
        CancelSuspendedDma(true);
        CancelSuspendedInstruction(true);
        PowerCycleStorage();
        CartridgeMemory();
        Obc1SaveBacking();
        ClockAndWorkRam();
        ControllerAttachments();
        std::puts("Console reset tests passed");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
