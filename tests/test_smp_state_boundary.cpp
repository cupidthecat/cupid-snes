#include "snes/core/Dsp.hpp"
#include "snes/core/FilePath.hpp"
#include "snes/core/Smp.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace snes::core;
using snes::core::detail::StateArchive;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct SoundMachine {
    Smp smp;
    Dsp dsp;
    std::array<int16_t, 4096> samples{};

    SoundMachine(Smp::DspMode mode = Smp::DspMode::PerCycle, unsigned internal = 0,
                 unsigned external = 0) {
        smp.SetDspMode(mode);
        smp.SetDsp(dsp);
        dsp.SetRam(smp.Ram());
        dsp.SetOutput(samples.data(), int(samples.size() / 2));
        smp.IoState().iplRomEnable = false;
        smp.IoState().internalWaitStates = uint8_t(internal);
        smp.IoState().externalWaitStates = uint8_t(external);
        smp.r.pc = 0x200;
        smp.r.a = 0x73;
        smp.r.x = 0x11;
        smp.r.y = 0x39;
        smp.r.s = 0xf3;
        for (unsigned address = 0; address < Smp::RamSize; ++address)
            smp.Ram()[address] = uint8_t(address * 37 + (address >> 8) * 13);
        smp.GetTimer0().enable = smp.GetTimer1().enable = smp.GetTimer2().enable = true;
        smp.GetTimer0().target = 3;
        smp.GetTimer1().target = 7;
        smp.GetTimer2().target = 5;
        dsp.Write(Dsp::kFlg, 0x1f);
        dsp.Write(Dsp::kNon, 1);
        dsp.Write(Dsp::kMVolL, 127);
        dsp.Write(Dsp::kMVolR, 97);
        dsp.Write(0, 113);
        dsp.Write(1, 79);
        dsp.Write(7, 0x7f);
        dsp.Write(Dsp::kKon, 1);
    }

    void Program(std::initializer_list<uint8_t> bytes) {
        std::copy(bytes.begin(), bytes.end(), smp.Ram() + 0x200);
    }

    std::vector<uint8_t> Capture() {
        StateArchive archive;
        smp.SerializeState(archive);
        dsp.SerializeState(archive);
        return archive.Bytes();
    }

    bool Prepare(uint64_t hardwareDeadline) {
        return smp.TrySynchronizeState(hardwareDeadline, uint32_t(smp.HardwareClockFrequency()));
    }

    void NormalizeClockTarget() {
        const auto before = smp.HardwareClockCount();
        smp.SynchronizeCpu(0, uint32_t(smp.HardwareClockFrequency()));
        Check(smp.HardwareClockCount() == before, "Normalizing a past deadline must not run sound");
    }
};

void DeferredInputLatches() {
    constexpr unsigned durations[]{2, 4, 10, 20};
    for (unsigned internal = 0; internal < 4; ++internal)
        for (unsigned external = 0; external < 4; ++external)
            for (uint8_t port = 0; port < 4; ++port) {
                auto machine =
                    std::make_unique<SoundMachine>(Smp::DspMode::PerCycle, internal, external);
                auto& smp = machine->smp;
                machine->Program({0xe4, uint8_t(0xf4 + port), 0xab, 0x20}); // MOV A,port; INC $20
                smp.r.a = 0;
                smp.PortWrite(port, 0x11);
                const auto midpoint = 2 * durations[external] + durations[internal] / 2;
                const auto finished = 2 * durations[external] + durations[internal];
                smp.SynchronizeCpu(midpoint, uint32_t(smp.HardwareClockFrequency()));
                Check(smp.HardwareClockCount() == midpoint && smp.InstructionInProgress(),
                      "The input fixture stops at a deferred communication midpoint");
                Check(!machine->Prepare(midpoint) && smp.HardwareClockCount() == midpoint &&
                          smp.r.a == 0,
                      "State preparation must not sample a deferred input while sound is ahead");
                smp.PortWrite(port, 0x77);
                const bool completed = machine->Prepare(midpoint + 1);
                Check(smp.HardwareClockCount() == midpoint + 1 &&
                          completed == (finished == midpoint + 1),
                      "State preparation advances no farther than its hardware deadline");
                smp.PortWrite(port, 0x99);
                Check(machine->Prepare(finished + 1) && smp.HardwareClockCount() == finished &&
                          smp.r.a == 0x77 && smp.ApuInput(port) == 0x99 && smp.r.pc == 0x202,
                      "A prepared input retains the sampled byte without starting the next "
                      "instruction");
                const auto captured = machine->Capture();
                Check(machine->Prepare(finished + 1000) && machine->Capture() == captured,
                      "Preparing an existing sound boundary leaves all state and clocks unchanged");
            }
}

void DeferredOutputLatches() {
    constexpr unsigned durations[]{2, 4, 10, 20};
    for (unsigned internal = 0; internal < 4; ++internal)
        for (unsigned external = 0; external < 4; ++external)
            for (unsigned destination = 0; destination < 5; ++destination) {
                auto machine =
                    std::make_unique<SoundMachine>(Smp::DspMode::Batch32, internal, external);
                auto& smp = machine->smp;
                const auto address = uint8_t(destination < 4 ? 0xf4 + destination : 0xf1);
                const auto value = uint8_t(destination < 4 ? 0xa5 : 0x30);
                machine->Program({0x8f, value, address, 0xab, 0x20}); // MOV address,#value; INC $20
                for (unsigned port = 0; port < 4; ++port)
                    smp.PortWrite(uint8_t(port), uint8_t(0x11 + port));
                const auto finished = 3 * durations[external] + 2 * durations[internal];
                smp.SynchronizeCpu(finished, uint32_t(smp.HardwareClockFrequency()));
                Check(smp.HardwareClockCount() == finished && smp.InstructionInProgress() &&
                          smp.Ram()[address] == value,
                      "The output fixture has written RAM and deferred its communication latch");
                Check(!machine->Prepare(finished) && smp.HardwareClockCount() == finished,
                      "State preparation must retain a deferred output at the console deadline");
                Check(destination < 4 ? smp.CpuOutput(uint8_t(destination)) == 0
                                      : smp.ApuInput(0) == 0x11 && smp.ApuInput(3) == 0x14,
                      "Preparing a pending output must not publish or clear communication latches");
                Check(machine->Prepare(finished + 1) && smp.HardwareClockCount() == finished &&
                          smp.r.pc == 0x203,
                      "A returned output handoff completes without adding a hardware clock");
                Check(destination < 4 ? smp.CpuOutput(uint8_t(destination)) == value
                                      : smp.ApuInput(0) == 0 && smp.ApuInput(3) == 0,
                      "The output latch changes after the console advances past its handoff");
            }
}

unsigned OpcodeContinuations() {
    unsigned continuations = 0;
    for (auto mode : {Smp::DspMode::PerCycle, Smp::DspMode::Batch32})
        for (unsigned internal = 0; internal < 4; ++internal)
            for (unsigned external = 0; external < 4; ++external)
                for (bool page : {false, true})
                    for (unsigned opcode = 0; opcode < 256; ++opcode) {
                        auto expected = std::make_unique<SoundMachine>(mode, internal, external);
                        expected->Program({uint8_t(opcode), 0xf4, 0x00, 0xab, 0x20});
                        expected->smp.r.p.Unpack(uint8_t(page ? 0xff : 0));
                        expected->smp.Step();
                        const auto finished = expected->smp.HardwareClockCount();
                        Check(finished >= 2 && finished < 1024,
                              "The opcode fixture finishes one instruction");
                        expected->NormalizeClockTarget();
                        const auto complete = expected->Capture();
                        std::array<uint64_t, 5> cuts{1, 2, 3, finished / 2, finished - 1};
                        std::sort(cuts.begin(), cuts.end());
                        uint64_t previous = 0;
                        for (const auto cut : cuts) {
                            if (cut == previous || cut >= finished)
                                continue;
                            previous = cut;
                            auto machine = std::make_unique<SoundMachine>(mode, internal, external);
                            machine->Program({uint8_t(opcode), 0xf4, 0x00, 0xab, 0x20});
                            machine->smp.r.p.Unpack(uint8_t(page ? 0xff : 0));
                            machine->smp.RunUntilHardwareClock(cut);
                            Check(machine->smp.InstructionInProgress(),
                                  "Each opcode cut retains an instruction");
                            Check(!machine->Prepare(cut) &&
                                      machine->smp.HardwareClockCount() == cut,
                                  "A sound deadline already reached cannot advance a pending "
                                  "instruction");
                            bool ready = false;
                            for (uint64_t deadline = cut + 1; deadline <= finished + 1 && !ready;
                                 ++deadline) {
                                ready = machine->Prepare(deadline);
                                Check(machine->smp.HardwareClockCount() <= deadline,
                                      "Finishing sound state never crosses the console deadline");
                            }
                            Check(ready && machine->smp.HardwareClockCount() == finished,
                                  "Bounded preparation ends at the original instruction boundary");
                            machine->NormalizeClockTarget();
                            Check(machine->Capture() == complete,
                                  "Bounded preparation preserves opcode results, RAM, timers, DSP "
                                  "and PCM");
                            ++continuations;
                        }
                    }
    return continuations;
}

void FractionalDeadline() {
    for (const auto masterFrequency : {21'477'272u, 21'281'370u})
        for (unsigned setting = 0; setting < 4; ++setting) {
            auto machine = std::make_unique<SoundMachine>(Smp::DspMode::PerCycle, setting, setting);
            machine->Program({0xe4, 0xf4, 0xab, 0x20});
            machine->smp.RunUntilHardwareClock(1);
            const auto hardwareFrequency = machine->smp.HardwareClockFrequency();
            bool ready = false;
            for (uint64_t master = 1; master < 1000 && !ready; ++master) {
                ready = machine->smp.TrySynchronizeState(master, masterFrequency);
                const auto limit =
                    (master * hardwareFrequency + masterFrequency - 1) / masterFrequency;
                Check(machine->smp.HardwareClockCount() <= limit,
                      "Fractional NTSC and PAL deadlines permit at most the next audio hardware "
                      "edge");
            }
            Check(ready && machine->smp.r.pc == 0x202,
                  "Fractional clock preparation completes only the pending instruction");
        }
}

void PreparedContinuation(StateArchive& portable) {
    for (auto mode : {Smp::DspMode::PerCycle, Smp::DspMode::Batch32})
        for (unsigned internal = 0; internal < 4; ++internal)
            for (unsigned external = 0; external < 4; ++external)
                for (const auto frequency : {21'477'272u, 21'281'370u}) {
                    auto original = std::make_unique<SoundMachine>(mode, internal, external);
                    auto restored =
                        std::make_unique<SoundMachine>(mode, 3 - internal, 3 - external);
                    original->Program(
                        {0xe4, 0xf4, 0xc4, 0xf5, 0xab, 0x20, 0x8f, 0x30, 0xf1, 0x2f, uint8_t(-11)});
                    original->smp.PortWrite(0, 0x53);
                    uint64_t master = 25'000;
                    original->smp.SynchronizeCpu(master, frequency);
                    Check(original->smp.InstructionInProgress(),
                          "The continuation fixture starts inside a communication handoff");
                    unsigned attempts = 0;
                    while (!original->smp.TrySynchronizeState(master, frequency)) {
                        master += 22;
                        Check(++attempts < 1000,
                              "A console clock advance allows sound preparation to finish");
                    }
                    auto saved = original->Capture();
                    portable.Expect(uint32_t(mode));
                    portable.Expect(internal);
                    portable.Expect(external);
                    portable.Expect(frequency);
                    portable.Vector(saved, StateArchive::MaximumBytes);
                    restored->smp.RunUntilHardwareClock(1);
                    StateArchive archive(saved);
                    restored->smp.SerializeState(archive);
                    restored->dsp.SerializeState(archive);
                    archive.Commit();
                    Check(
                        restored->Capture() == saved,
                        "Restoring prepared sound state replaces an unfinished target instruction");
                    for (unsigned step = 0; step < 100; ++step) {
                        master += 997;
                        for (auto* machine : {original.get(), restored.get()}) {
                            machine->smp.PortWrite(uint8_t(step % 4), uint8_t(step * 37));
                            machine->smp.SynchronizeCpu(master, frequency);
                        }
                    }
                    original->smp.SynchronizeState();
                    restored->smp.SynchronizeState();
                    Check(original->Capture() == restored->Capture(),
                          "Prepared sound state resumes port traffic, clock phases, timers and PCM "
                          "exactly");
                    Check(
                        std::any_of(original->samples.begin(),
                                    original->samples.begin() + original->dsp.SamplesWritten() * 2,
                                    [](int16_t value) { return value != 0; }),
                        "Prepared continuation includes audible PCM");
                }
}
} // namespace

int main(int argc, char** argv) {
    try {
        DeferredInputLatches();
        DeferredOutputLatches();
        FractionalDeadline();
        const auto continuations = OpcodeContinuations();
        StateArchive portable;
        PreparedContinuation(portable);
        if (argc > 1) {
            std::ofstream output(FilePath(argv[1]), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(portable.Bytes().data()),
                         std::streamsize(portable.Bytes().size()));
            Check(bool(output), "Write portable prepared sound states");
        }
        std::printf("Sound state boundaries passed: %u opcode continuations\n", continuations);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
