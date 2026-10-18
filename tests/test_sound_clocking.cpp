#include "ConsoleConfig.hpp"
#include "audio/SoundUploadFixture.hpp"
#include "snes/core/SnesCpu.hpp"
#include "snes/core/Spc700.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace snes::core;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::unique_ptr<Emulator> Machine(bool pal) {
  std::vector<uint8_t> rom(0x8000);
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0x7fc0] = 'T';
  rom[0x7fd5] = 0x20;
  rom[0x7fd7] = 5;
  rom[0x7fd9] = pal ? 2 : 1;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffd] = 0x80;
  rom[0] = 0xdb;
  auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  std::string error;
  Check(machine->LoadCartridge(rom, &error), "The sound-clock fixture loads");
  return machine;
}

void PortSamplingEdges() {
  constexpr std::array<unsigned, 4> waits{2, 4, 10, 20};
  for (bool pal : {false, true}) {
    for (unsigned setting = 0; setting < 4; ++setting) {
      for (uint8_t port = 0; port < 4; ++port) {
        for (bool afterSample : {false, true}) {
          auto machine = Machine(pal);
          auto &smp = machine->GetSmp();
          auto &cpu = *machine->GetCpu();
          smp.Power();
          smp.IoState().iplRomEnable = false;
          smp.r.pc = 0x200;
          // Four NOPs occupy eight complete cycles during console reset.
          smp.Ram()[0x204] = 0xe4;
          smp.Ram()[0x205] = uint8_t(0xf4 + port); // MOV A,port
          smp.PortWrite(port, 0x11);
          cpu.Step();
          smp.IoState().internalWaitStates = uint8_t(setting);
          constexpr uint64_t hardwareHz = Smp::kHardwareClockFrequency;
          const uint64_t sampleClock = 20 + waits[setting] / 2;
          const auto masterHz = machine->GetTiming().MasterClockHz();
          const auto writeEdge =
              (sampleClock * masterHz / (6 * hardwareHz) + unsigned(afterSample)) * 6;
          Check(writeEdge > 192, "The port-store fixture follows reset");
          while (machine->GetTiming().MasterClocksElapsed() < writeEdge - 6)
            cpu.idle();
          Check(machine->GetTiming().MasterClocksElapsed() == writeEdge - 6,
                "The console store begins on the intended bus edge");
          cpu.write(0x2140 + port, 0x77);
          const auto completedCycle = (20 + waits[setting]) / 2;
          while (smp.CycleCount() < completedCycle)
            cpu.idle();
          Check(smp.r.a == (afterSample ? 0x11 : 0x77),
                "A sound-port read holds the value sampled at its half-cycle edge");
          Check(smp.ApuInput(port) == 0x77,
                "A held read preserves the later console-to-sound latch value");
        }
      }
    }
  }
}

void CommunicationHandoffs() {
  constexpr std::array<unsigned, 4> waits{2, 4, 10, 20};
  for (unsigned setting = 0; setting < waits.size(); ++setting) {
    Smp smp;
    smp.IoState().iplRomEnable = false;
    smp.IoState().internalWaitStates = uint8_t(setting);
    smp.r.pc = 0x200;
    const std::array<uint8_t, 6> program{0xe4, 0xf4, 0xc4, 0xf5, 0x2f, 0xfa};
    std::copy(program.begin(), program.end(), smp.Ram() + 0x200);
    smp.PortWrite(0, 0x11);
    const auto inputClock = 4 + waits[setting] / 2;
    const auto dummyClock = 8 + waits[setting] + waits[setting] / 2;
    const auto outputClock = 8 + 3 * waits[setting];
    const auto synchronize = [&](uint64_t clock) {
      smp.SynchronizeCpu(clock, uint32_t(Smp::kHardwareClockFrequency));
    };
    synchronize(1);
    Check(smp.HardwareClockCount() == inputClock && smp.r.a == 0,
          "The sound processor yields before sampling a stretched input port");
    smp.PortWrite(0, 0x77);
    synchronize(inputClock + 1);
    Check(smp.HardwareClockCount() == dummyClock && smp.r.a == 0x77 && smp.CpuOutput(1) == 0,
          "The resumed input sees the console write before the next communication handoff");
    synchronize(dummyClock + 1);
    Check(smp.HardwareClockCount() == outputClock && smp.CpuOutput(1) == 0,
          "A completed output write yields before changing the console-visible latch");
    synchronize(outputClock + 1);
    Check(smp.CpuOutput(1) == 0x77,
          "The next console handoff commits the pending output without another bus clock");
  }
}

void HardwareClockPhase() {
  for (bool pal : {false, true}) {
    auto machine = Machine(pal);
    auto &smp = machine->GetSmp();
    auto &cpu = *machine->GetCpu();
    smp.Power();
    smp.IoState().iplRomEnable = false;
    smp.r.pc = 0x200; // NOPs in zero-initialized sound RAM.
    cpu.Step();
    for (unsigned interval = 0; interval < 256; ++interval) {
      switch (interval & 3) {
      case 0:
        cpu.idle();
        break;
      case 1:
        cpu.read(0x7e0000);
        break;
      case 2:
        cpu.write(0x2100, 0x80);
        break;
      case 3:
        cpu.read(0x4016);
        break;
      }
      const auto elapsed = machine->GetTiming().MasterClocksElapsed();
      const auto clocks =
          elapsed * Smp::kHardwareClockFrequency / machine->GetTiming().MasterClockHz();
      const auto timerPhase = smp.GetTimer0().stage0 + 128u * smp.GetTimer0().stage1;
      Check(timerPhase == clocks % 256,
            "Sound timer dividers retain every hardware-clock edge between CPU phases");
      Check(smp.CycleCount() == clocks / 2 && machine->GetDsp().Phase() == clocks / 2 % 32,
            "Complete-cycle accounting and DSP cadence follow the hardware clock");
    }
  }
}

void HalfCycleDeadlinesAndReset() {
  constexpr std::array<unsigned, 4> waits{2, 4, 10, 20};
  for (unsigned setting = 0; setting < 4; ++setting) {
    for (uint8_t port = 0; port < 4; ++port) {
      Smp smp;
      smp.IoState().iplRomEnable = false;
      smp.IoState().internalWaitStates = uint8_t(setting);
      smp.r.pc = 0x200;
      smp.Ram()[0x200] = 0xe4;
      smp.Ram()[0x201] = uint8_t(0xf4 + port);
      const unsigned sample = 4 + waits[setting] / 2;
      const unsigned finish = 4 + waits[setting];
      smp.RunUntilHardwareClock(sample - 1);
      smp.PortWrite(port, 0x11);
      smp.RunUntilHardwareClock(sample);
      smp.PortWrite(port, 0x77);
      smp.RunUntilHardwareClock(finish);
      Check(smp.r.a == 0x11 && smp.ApuInput(port) == 0x77 && !smp.InstructionInProgress(),
            "Every TEST divider samples the input exactly at the native half-cycle edge");

      smp.Power();
      smp.IoState().iplRomEnable = false;
      smp.IoState().internalWaitStates = uint8_t(setting);
      smp.r.pc = 0x200;
      smp.Ram()[0x200] = 0x8f;
      smp.Ram()[0x201] = 0xa5;
      smp.Ram()[0x202] = uint8_t(0xf4 + port); // MOV port,#$A5
      const unsigned storeEnd = 6 + 2 * waits[setting];
      for (unsigned clock = 1; clock < storeEnd; ++clock) {
        smp.RunUntilHardwareClock(clock);
        Check(smp.HardwareClockCount() == clock && smp.CycleCount() == clock / 2 &&
                  smp.PortRead(port) == 0 && smp.Ram()[0xf4 + port] == 0,
              "A port store remains pending at every preceding hardware-clock deadline");
      }
      Check(smp.HardwareClockCount() & 1, "Power interrupts the store on an odd hardware clock");
      smp.Power();
      Check(smp.HardwareClockCount() == 0 && smp.CycleCount() == 0,
            "Power resets both sound clock domains");
      smp.IoState().iplRomEnable = false;
      smp.r.pc = 0x200;
      smp.RunUntilHardwareClock(4);
      Check(!smp.InstructionInProgress() && smp.CycleCount() == 2 && smp.PortRead(port) == 0 &&
                smp.Ram()[0xf4 + port] == 0,
            "Power cancels an odd-phase store before executing the new program");
    }
  }
}

void ConsoleAudioRate() {
  struct Audio final : IAudioOutput {
    uint64_t frames = 0;
    void Submit(const AudioBuffer &buffer) override {
      Check(buffer.sampleRate == 32'040,
            "Audio buffers report the oscillator's native sample rate");
      frames += buffer.interleavedStereo.size() / 2;
    }
  };
  for (bool pal : {false, true}) {
    auto machine = Machine(pal);
    Audio audio;
    machine->AttachAudioOutput(&audio);
    for (unsigned frame = 0; frame < 6; ++frame) {
      machine->StepFrame();
      const auto clocks =
          machine->CurrentMasterCycles() * 2'050'560 / machine->GetTiming().MasterClockHz();
      Check(machine->GetSmp().HardwareClockCount() == clocks &&
                audio.frames == (clocks / 2 + 4) / 32,
            "Both video regions generate samples at the native oscillator rate without drift");
    }
  }
}

void UploadedPortEdges() {
  constexpr std::array<unsigned, 4> ntscEdges{16, 19, 23, 28};
  constexpr std::array<unsigned, 4> palEdges{20, 15, 24, 24};
  for (bool pal : {false, true}) {
    const auto &edges = pal ? palEdges : ntscEdges;
    for (unsigned setting = 0; setting < 4; ++setting) {
      for (unsigned delay = edges[setting] - 1; delay <= edges[setting] + 1; ++delay) {
        auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
        std::string error;
        Check(machine->LoadCartridge(sound_upload_test::PortProgram(setting, delay, pal), &error),
              "The console-to-sound upload fixture loads");
        std::fill_n(machine->GetBus().WramData(), MemoryBus::WramSize, 0);
        for (unsigned frame = 0; frame < 3; ++frame)
          machine->StepFrame();
        const auto *result = machine->GetBus().WramData() + 0x100;
        Check(result[1] == 0xa5 && result[0] == (delay < edges[setting] ? 0x77 : 0x11),
              "An uploaded sound program samples console writes at the expected oscillator edge");
      }
    }
  }
}

struct HaltBus : Spc700 {
  std::array<uint8_t, 65536> ram{};
  std::vector<uint16_t> reads;
  unsigned idles = 0;
  bool readWhileHalted = false;
  void Idle() override {
    ++idles;
    ++cycles_;
  }
  uint8_t Read(uint16_t address) override {
    reads.push_back(address);
    readWhileHalted |= address == 0x201 && (r.wait || r.stop);
    ++cycles_;
    return ram[address];
  }
  void Write(uint16_t address, uint8_t value) override {
    ram[address] = value;
    ++cycles_;
  }
};

void HaltedBusSequence() {
  for (uint8_t opcode : {uint8_t(0xef), uint8_t(0xff)}) {
    HaltBus cpu;
    cpu.Power();
    cpu.r.pc = 0x200;
    cpu.ram[0x200] = opcode;
    cpu.Step();
    Check(cpu.reads == std::vector<uint16_t>{0x200, 0x201} && cpu.idles == 1 &&
              cpu.CycleCount() == 3 && cpu.readWhileHalted,
          "SLEEP and STOP enter their read-at-PC and idle sequence immediately");
    for (unsigned pair = 0; pair < 4; ++pair) {
      const auto before = cpu.CycleCount();
      const auto reads = cpu.reads.size();
      cpu.Step();
      Check(cpu.CycleCount() - before == 2 && cpu.reads.size() == reads + 1 &&
                cpu.reads.back() == 0x201 && cpu.r.pc == 0x201,
            "Halted execution repeats a read and idle pair without incrementing PC");
    }
  }
}

void HaltedWaitClasses() {
  constexpr std::array<unsigned, 4> durations{1, 2, 5, 10};
  constexpr std::array<unsigned, 4> timerTicks{2, 4, 8, 16};
  for (uint8_t opcode : {uint8_t(0xef), uint8_t(0xff)}) {
    for (unsigned external = 0; external < 4; ++external) {
      for (unsigned internal = 0; internal < 4; ++internal) {
        Smp smp;
        smp.IoState().iplRomEnable = false;
        smp.IoState().externalWaitStates = uint8_t(external);
        smp.IoState().internalWaitStates = uint8_t(internal);
        smp.r.pc = 0x200;
        smp.Ram()[0x200] = opcode;
        smp.Step();
        Check(smp.CycleCount() == 2 * durations[external] + durations[internal],
              "Halt entry applies external read waits and internal idle waits independently");
        const auto phase = smp.GetTimer0().stage0 + 128u * smp.GetTimer0().stage1;
        Check(phase == 2 * timerTicks[external] + timerTicks[internal],
              "Timer dividers retain their ratios during halted read and idle waits");
        const auto before = smp.CycleCount();
        smp.Step();
        Check(smp.CycleCount() - before == durations[external] + durations[internal] &&
                  smp.r.pc == 0x201,
              "A halted sound processor retains both selected wait classes");
      }
    }
  }
}

void ControlTransitions() {
  for (unsigned oldMask = 0; oldMask < 8; ++oldMask) {
    for (unsigned newMask = 0; newMask < 8; ++newMask) {
      for (uint8_t clear : {0, 0x10, 0x20, 0x30}) {
        for (bool ipl : {false, true}) {
          for (bool edge : {false, true}) {
            Smp smp;
            for (uint8_t port = 0; port < 4; ++port) {
              smp.PortWrite(port, uint8_t(0x11 * (port + 1)));
              smp.Write(0xf4 + port, uint8_t(0xa0 + port));
            }
            auto seed = [&](auto &timer, unsigned number, unsigned frequency) {
              timer.enable = (oldMask & (1u << number)) != 0;
              timer.stage0 = uint8_t(edge ? frequency - 2 : 0);
              timer.stage1 = uint8_t(edge);
              timer.line = edge;
              timer.stage2 = 0x42;
              timer.stage3 = 9;
              timer.target = 0x43;
            };
            seed(smp.GetTimer0(), 0, 128);
            seed(smp.GetTimer1(), 1, 128);
            seed(smp.GetTimer2(), 2, 16);
            smp.r.p.p = (oldMask & 1) != 0; // CONTROL applies in either direct-page mode.
            const auto value = uint8_t(newMask | clear | (ipl ? 0x80 : 0));
            smp.Write(0xf1, value);
            auto verify = [&](const auto &timer, unsigned number) {
              const bool wasEnabled = (oldMask & (1u << number)) != 0;
              const bool enabled = (newMask & (1u << number)) != 0;
              const bool rising = !wasEnabled && enabled;
              const bool reset = number == 2 ? !rising : rising;
              const unsigned expectedCounter = reset || (wasEnabled && edge) ? 0 : 0x42;
              const unsigned expectedOutput = reset ? 0 : wasEnabled && edge ? 10 : 9;
              Check(timer.enable == enabled && timer.stage2 == expectedCounter &&
                        timer.stage3 == expectedOutput,
                    "CONTROL applies each timer's reset rule after the current divider edge");
              Check(timer.stage0 == (edge ? 0 : 2) && timer.stage1 == 0,
                    "CONTROL preserves the free-running timer divider");
            };
            verify(smp.GetTimer0(), 0);
            verify(smp.GetTimer1(), 1);
            verify(smp.GetTimer2(), 2);
            for (uint8_t port = 0; port < 4; ++port) {
              const bool cleared = (clear & (port < 2 ? 0x10 : 0x20)) != 0;
              Check(smp.ApuInput(port) == (cleared ? 0 : 0x11 * (port + 1)) &&
                        smp.PortRead(port) == 0xa0 + port,
                    "CONTROL clears input pairs while preserving the output latches");
            }
            Check(smp.IoState().iplRomEnable == ipl && smp.Ram()[0xf1] == value,
                  "CONTROL updates the IPL overlay and its backing RAM on the final write edge");
          }
        }
      }
    }
  }
}
} // namespace

int main() {
  unsigned failures = 0;
  for (auto test : {PortSamplingEdges, HardwareClockPhase, HalfCycleDeadlinesAndReset,
                    ConsoleAudioRate, UploadedPortEdges, HaltedBusSequence, HaltedWaitClasses,
                    ControlTransitions, CommunicationHandoffs}) {
    try {
      test();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s\n", error.what());
      ++failures;
    }
  }
  if (failures)
    return 1;
  std::puts(
      "Sound hardware-clock edges, port holds, halted cycles, and CONTROL transitions passed");
}
