#include "audio/SpcBusFixture.hpp"
#include "snes/core/Sa1.hpp"
#include "snes/core/Smp.hpp"
#include "snes/core/SnesCpu.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <vector>

using namespace snes::core;
using snes::core::detail::StateArchive;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class Operation> void Reject(Operation operation, const char* message) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Check(rejected, message);
}

template <class Device> std::vector<uint8_t> Capture(Device& device) {
    StateArchive writer;
    device.SerializeState(writer);
    return writer.Bytes();
}

template <class Device> void Restore(Device& device, std::span<const uint8_t> bytes) {
    StateArchive reader(bytes);
    device.SerializeState(reader);
    reader.Commit();
}

struct Bus : Spc700 {
    std::array<uint8_t, 65536> memory{};
    uint64_t digest = spc_bus_test::InitialDigest;

    void Record(unsigned kind, uint16_t address, uint8_t data) {
        spc_bus_test::Record(digest, kind, address, data, r);
        ++cycles_;
    }

    void Idle() override {
        Record(0, 0, 0);
    }

    uint8_t Read(uint16_t address) override {
        Record(1, address, memory[address]);
        return memory[address];
    }

    void Write(uint16_t address, uint8_t data) override {
        Record(2, address, data);
        memory[address] = data;
    }
};

struct ConsoleBus : ICpuBus {
    std::array<uint8_t, 65536> memory{};
    uint64_t digest = spc_bus_test::InitialDigest;
    uint8_t latch = 0;

    uint8_t Read(uint32_t address) override {
        const auto value = memory[address & 0xffff];
        digest = (digest ^ (uint64_t(address) << 8) ^ value) * 0x100000001b3;
        return value;
    }

    void Write(uint32_t address, uint8_t value) override {
        memory[address & 0xffff] = value;
        digest = (digest ^ (uint64_t(address) << 8) ^ value ^ 0x8000000000000000) * 0x100000001b3;
        latch = value;
    }

    void SetOpenBus(uint8_t value) noexcept override {
        latch = value;
    }

    void Program(unsigned kind) {
        memory.fill(0xea);
        const uint8_t code[]{0x58, 0xa9, 0x5a, 0x8d, 0, 0x20, 0xee, 1, 0x20, 0x80, 0xfb};
        std::copy(std::begin(code), std::end(code), memory.begin() + 0x8000);
        if (kind == 1 || kind == 2)
            memory[0x8006] = kind == 1 ? 0xcb : 0xdb;
        if (kind == 3) {
            const uint8_t native[]{0x18, 0xfb, 0xc2, 0x30, 0xa9, 0x34, 0x12, 0x18, 0x69, 1,
                                   0,    0x8d, 0,    0x20, 0xee, 2,    0x20, 0x80, 0xfb};
            std::copy(std::begin(native), std::end(native), memory.begin() + 0x8000);
        }
        memory[0x9000] = 0x40;
        for (unsigned vector : {0xfffau, 0xffeau, 0xfffeu, 0xffeeu}) {
            memory[vector] = 0;
            memory[vector + 1] = 0x90;
        }
        memory[0xfffc] = 0;
        memory[0xfffd] = 0x80;
    }
};

void ConsoleCpuContinuation() {
    for (unsigned kind = 0; kind < 4; ++kind)
        for (unsigned cut = 0; cut < 16; ++cut) {
            ConsoleBus originalBus, restoredBus;
            originalBus.Program(kind);
            SnesCpu original(originalBus), restored(restoredBus);
            original.Reset();
            for (unsigned instruction = 0; instruction < cut; ++instruction) {
                if (instruction == 7)
                    original.RequestNmi();
                original.Step();
            }
            const auto saved = Capture(original);
            restoredBus.memory = originalBus.memory;
            restoredBus.latch = originalBus.latch;
            uint64_t callbackClocks = 0;
            restored.SetClockCallback([&](uint32_t clocks) { callbackClocks += clocks; });
            Restore(restored, saved);
            const auto before = restored.Cycles();
            originalBus.digest = restoredBus.digest = spc_bus_test::InitialDigest;
            for (unsigned instruction = 0; instruction < 32; ++instruction) {
                if (instruction == 3) {
                    original.RequestNmi();
                    restored.RequestNmi();
                }
                if (instruction == 7 || instruction == 10) {
                    original.SetIrqLevel(instruction == 7);
                    restored.SetIrqLevel(instruction == 7);
                }
                Check(original.Step() == restored.Step(),
                      "Restored CPU retains each instruction's clock count");
            }
            Check(originalBus.digest == restoredBus.digest &&
                      originalBus.memory == restoredBus.memory &&
                      originalBus.latch == restoredBus.latch &&
                      Capture(original) == Capture(restored) &&
                      callbackClocks == restored.Cycles() - before,
                  "CPU state retains reset, native widths, wait/stop, interrupts and platform "
                  "callbacks");
            Check(originalBus.memory[0x2000] == (kind == 3 ? 0x35 : 0x5a),
                  "Restored console code publishes the expected arithmetic result");
        }
}

void EverySpcInstruction() {
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        for (unsigned scenario = 0; scenario < 4; ++scenario) {
            const auto fixture = spc_bus_test::Prepare(uint8_t(opcode), scenario);
            Bus full;
            full.memory = fixture.memory;
            full.r = fixture.registers;
            full.Step();
            const auto cycles = full.CycleCount();
            for (unsigned cut = 0; cut <= cycles; ++cut) {
                Bus original;
                original.memory = fixture.memory;
                original.r = fixture.registers;
                for (unsigned cycle = 0; cycle < cut; ++cycle)
                    original.StepCycle();
                if (cut && cut < cycles)
                    Reject([&] { Capture(original); },
                           "A retained coroutine cannot be written directly");
                original.SynchronizeState();
                if (cut)
                    Check(original.digest == full.digest && original.memory == full.memory,
                          "Finishing an SPC700 instruction preserves its complete bus sequence");
                const auto saved = Capture(original);
                Bus restored;
                restored.memory = original.memory;
                restored.StepCycle();
                Check(restored.InstructionInProgress(),
                      "Restore starts with an unrelated live coroutine");
                Restore(restored, saved);
                original.digest = restored.digest = spc_bus_test::InitialDigest;
                for (unsigned instruction = 0; instruction < 8; ++instruction) {
                    original.Step();
                    restored.Step();
                }
                Check(
                    original.digest == restored.digest && original.memory == restored.memory &&
                        Capture(original) == Capture(restored),
                    "A rebuilt SPC700 coroutine continues every opcode, branch and wrapping case");
            }
        }
    }
}

void Program(Smp& smp, unsigned internal, unsigned external) {
    smp.Power();
    smp.IoState().iplRomEnable = false;
    smp.IoState().internalWaitStates = uint8_t(internal);
    smp.IoState().externalWaitStates = uint8_t(external);
    smp.r.pc = 0x200;
    const uint8_t code[]{0x8f, 0xa5, 0xf4, 0xe4, 0xf4, 0xab, 0x20, 0x2f, 0xf7};
    std::copy(std::begin(code), std::end(code), smp.Ram() + 0x200);
    smp.GetTimer0().enable = smp.GetTimer1().enable = smp.GetTimer2().enable = true;
    smp.GetTimer0().target = 3;
    smp.GetTimer1().target = 7;
    smp.GetTimer2().target = 2;
    smp.PortWrite(0, 0x51);
}

void SmpContinuation() {
    for (auto mode : {Smp::DspMode::PerCycle, Smp::DspMode::Batch32})
        for (unsigned internal = 0; internal < 4; ++internal)
            for (unsigned external = 0; external < 4; ++external)
                for (unsigned cut : {1u, 3u, 11u, 53u, 127u, 301u}) {
                    Smp original, restored;
                    original.SetDspMode(mode);
                    restored.SetDspMode(mode);
                    Program(original, internal, external);
                    if (cut == 301)
                        original.SynchronizeCpu(301, 21'477'272);
                    else
                        original.RunUntilHardwareClock(cut);
                    original.SynchronizeState();
                    Check(!original.InstructionInProgress(),
                          "SMP synchronization finishes the active opcode");
                    const auto saved = Capture(original);
                    const auto at = original.HardwareClockCount();
                    original.SynchronizeState();
                    Check(original.HardwareClockCount() == at && Capture(original) == saved,
                          "Synchronizing an idle instruction boundary consumes no extra clocks");
                    Program(restored, 0, 0);
                    restored.RunUntilHardwareClock(1);
                    auto* mappedRam = restored.Ram();
                    StateArchive reader(saved);
                    restored.SerializeState(reader);
                    Check(restored.InstructionInProgress() && restored.HardwareClockCount() == 1,
                          "Parsing a valid SMP state leaves its current coroutine running");
                    reader.Commit();
                    Check(
                        restored.Ram() == mappedRam && Capture(restored) == saved,
                        "Restore retains the shared audio RAM address and replaces all SMP state");
                    for (auto* device : {&original, &restored}) {
                        device->PortWrite(0, 0x73);
                        device->SynchronizeCpu(25'000, 21'477'272);
                        device->RunUntilHardwareClock(at + 6000);
                        device->SynchronizeState();
                    }
                    Check(original.Ram()[0x20] != 0 && original.CpuOutput(0) == 0xa5 &&
                              original.ApuInput(0) == 0x73 &&
                              Capture(original) == Capture(restored),
                          "SMP restores wait ratios, port latches, timers and CPU handoff clock "
                          "debt");
                }

    Smp held;
    Program(held, 3, 0);
    held.Ram()[0x200] = 0xe4;
    held.Ram()[0x201] = 0xf4;
    held.RunUntilHardwareClock(14);
    held.PortWrite(0, 0x99);
    held.SynchronizeState();
    Check(held.r.a == 0x51 && held.ApuInput(0) == 0x99,
          "Checkpoint synchronization retains a port value sampled before a stretched read "
          "completes");
}

void InvalidSmpPreservesExecution() {
    Smp source, original, control;
    Program(source, 0, 0);
    source.RunUntilHardwareClock(121);
    source.SynchronizeState();
    auto invalid = Capture(source);
    invalid.push_back(0xff);
    for (auto* device : {&original, &control}) {
        Program(*device, 3, 2);
        device->RunUntilHardwareClock(1);
    }
    StateArchive bad(invalid);
    original.SerializeState(bad);
    Reject([&] { bad.Commit(); }, "Trailing SMP state data cancels the restore");
    Check(original.InstructionInProgress() && original.HardwareClockCount() == 1,
          "A rejected SMP state retains its unfinished bus operation");
    for (auto* device : {&original, &control}) {
        device->RunUntilHardwareClock(1000);
        device->SynchronizeState();
    }
    Check(Capture(original) == Capture(control),
          "A rejected SMP restore continues the original timeline");
    Smp different;
    different.SetDspMode(Smp::DspMode::Batch32);
    Reject([&] { Restore(different, Capture(source)); },
           "SMP execution mode must match its snapshot");
}

void Set(Sa1& chip, uint32_t address, uint32_t value, unsigned bytes = 2) {
    for (unsigned index = 0; index < bytes; ++index)
        chip.WriteSa1(address + index, uint8_t(value >> (index * 8)));
}

struct CartridgeProcessor {
    std::vector<uint8_t> rom = std::vector<uint8_t>(0x10000, 0xea);
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x2000);
    Sa1 chip{rom, ram};

    void Start(unsigned kind, bool contention) {
        const uint8_t code[]{0xa9, 0x5a, 0x8f, 0, 1, 0x40, 0xee, 1, 0x30, 0x80, 0xfb};
        std::copy(std::begin(code), std::end(code), rom.begin());
        if (kind == 1 || kind == 2)
            rom[6] = kind == 1 ? 0xcb : 0xdb;
        chip.SetBusContention(contention);
        chip.WriteSa1(0x222a, 0xff);
        chip.WriteSa1(0x2227, 0x80);
        chip.WriteCpu(0x2203, 0);
        chip.WriteCpu(0x2204, 0x80);
        chip.WriteCpu(0x2200, 0);
        if (kind == 3) {
            for (unsigned index = 0; index < 8; ++index)
                rom[0x100 + index] = uint8_t(0x91 + index);
            chip.WriteSa1(0x2230, 0x84);
            Set(chip, 0x2232, 0xc00100, 3);
            Set(chip, 0x2238, 8);
            Set(chip, 0x2235, 0x400, 3);
        }
    }
};

void Sa1Continuation() {
    for (bool contention : {false, true})
        for (unsigned kind = 0; kind < 4; ++kind)
            for (unsigned cut : {0u, 1u, 2u, 7u, 12u, 19u, 31u, 48u, 93u}) {
                CartridgeProcessor original, restored;
                original.Start(kind, contention);
                original.chip.Advance(cut, 0xc00000, false, contention);
                original.chip.SynchronizeState();
                const auto saved = Capture(original.chip);
                Check(original.chip.MasterClocks() == cut,
                      "SA-1 checkpoint synchronization preserves the console's requested clock");
                original.chip.SynchronizeState();
                Check(Capture(original.chip) == saved,
                      "An SA-1 boundary does not repeat a pending timer edge");
                restored.Start(kind, contention);
                restored.chip.Advance(1);
                auto* mappedRam = restored.ram.data();
                Restore(restored.chip, saved);
                Check(restored.ram.data() == mappedRam && Capture(restored.chip) == saved,
                      "SA-1 restore replaces its native stack while keeping cartridge RAM mapped");
                for (auto* device : {&original.chip, &restored.chip}) {
                    device->Advance(1000, 0x7e1000);
                    device->SynchronizeState();
                }
                Check(
                    original.ram[0x100] == 0x5a && original.ram == restored.ram &&
                        Capture(original.chip) == Capture(restored.chip),
                    "SA-1 continuation restores instructions, wait/stop state and deferred clocks");
                if (kind == 3)
                    Check(
                        original.ram[0x400] == 0x91 && original.ram[0x407] == 0x98 &&
                            !original.chip.DmaActive(),
                        "A partially completed SA-1 DMA resumes every remaining byte exactly once");
            }
}

void InvalidSa1PreservesExecution() {
    CartridgeProcessor source, original, control;
    source.Start(0, false);
    source.chip.Advance(43);
    source.chip.SynchronizeState();
    auto invalid = Capture(source.chip);
    invalid.back() = 2;
    for (auto* fixture : {&original, &control}) {
        fixture->Start(0, false);
        fixture->chip.Advance(12);
    }
    StateArchive reader(invalid);
    Reject([&] { original.chip.SerializeState(reader); }, "Invalid conversion state is rejected");
    Reject([&] { reader.Commit(); },
           "Invalid SA-1 state cannot discard the active execution stack");
    for (auto* fixture : {&original, &control}) {
        fixture->chip.Advance(100);
        fixture->chip.SynchronizeState();
    }
    Check(Capture(original.chip) == Capture(control.chip) && original.ram == control.ram,
          "A rejected SA-1 restore preserves pending stores and clock debt");
    CartridgeProcessor different;
    different.Start(0, true);
    Reject([&] { Restore(different.chip, Capture(source.chip)); },
           "SA-1 bus timing configuration must match its snapshot");
}
} // namespace

int main() {
    try {
        ConsoleCpuContinuation();
        EverySpcInstruction();
        SmpContinuation();
        InvalidSmpPreservesExecution();
        Sa1Continuation();
        InvalidSa1PreservesExecution();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::puts("Processor state boundaries, rebuilt execution and atomic rejection passed");
    return 0;
}
