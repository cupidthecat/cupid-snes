#include "snes/core/Emulator.hpp"
#include "snes/core/SnesCpu.hpp"

#include <algorithm>
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

std::vector<uint8_t> Rom(bool pal = false) {
  std::vector<uint8_t> rom(0x8000);
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0x7fc0] = 'T';
  rom[0x7fd5] = 0x20;
  rom[0x7fd7] = 5;
  rom[0x7fd9] = pal ? 2 : 1;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffd] = 0x80;
  rom[0] = 0xdb; // STP keeps the fixture independent of instruction throughput.
  return rom;
}

std::unique_ptr<Emulator> Machine(bool pal = false) {
  auto machine = std::make_unique<Emulator>();
  std::string error;
  Check(machine->LoadCartridge(Rom(pal), &error), "The frame fixture loads");
  return machine;
}

struct Output final : IVideoOutput, IAudioOutput {
  unsigned videoCalls = 0, audioCalls = 0;
  uint64_t samples = 0;
  void Present(const VideoFrame &) override {
    ++videoCalls;
  }
  void Submit(const AudioBuffer &buffer) override {
    ++audioCalls;
    Check(buffer.sampleRate == AudioTiming::SampleRate, "Output retains the native audio rate");
    Check(buffer.interleavedStereo.size() < 1400, "One field contains one field of audio");
    samples += buffer.interleavedStereo.size() / 2;
  }
};

void BoundaryDelivery() {
  for (bool pal : {false, true}) {
    for (uint8_t setting : {0, 1, 4, 5}) {
      auto machine = Machine(pal);
      machine->GetBus().Write(0x2133, setting);
      Output output;
      machine->AttachVideoOutput(&output);
      machine->AttachAudioOutput(&output);
      for (unsigned frame = 0; frame < 4; ++frame) {
        const auto result = machine->StepFrame();
        const auto &timing = machine->GetTiming();
        Check(timing.VCounter() == timing.VDisp() && timing.HCounter() == 0,
              "StepFrame returns at the display boundary inside the current CPU cycle");
        Check(timing.FrameCount() == frame && result.frameIndex == frame + 1,
              "Host frames count display events, including the first partial field");
        Check(output.videoCalls == frame + 1 && output.audioCalls == frame + 1,
              "Every field delivers exactly one video frame and one audio block");
        const auto hardwareClocks =
            result.masterCycles * Smp::kHardwareClockFrequency / timing.MasterClockHz();
        Check(machine->GetSmp().CycleCount() == hardwareClocks / 2 &&
                  output.samples == (hardwareClocks / 2 + 4) / 32,
              "Sound reaches the exact display deadline before delivery");
      }
    }
  }
}

void StartLongDma(Emulator &machine, unsigned &writes) {
  machine.GetBus().Write(0x7e0100, 0xa5);
  machine.GetBus().Map(
      0, 0, 0x21e0, 0x21e0, [](uint32_t, uint8_t value) { return value; },
      [&](uint32_t, uint8_t value) {
        Check(value == 0xa5, "A suspended DMA retains its sampled byte");
        ++writes;
      });
  for (unsigned index = 0; index < 8; ++index) {
    auto &channel = machine.GetDma().Channel(index);
    channel.writeControl(8);
    channel.sourceBank = 0x7e;
    channel.sourceAddress = 0x100;
    channel.targetAddress = 0xe0;
    channel.transferSize = 0; // Each channel transfers 65536 bytes.
  }
  machine.GetDma().EnableDma(0xff);
}

void DmaAcrossFields() {
  for (bool pal : {false, true}) {
    auto machine = Machine(pal);
    unsigned writes = 0;
    StartLongDma(*machine, writes);
    Output output;
    machine->AttachVideoOutput(&output);
    machine->AttachAudioOutput(&output);
    for (unsigned frame = 0; frame < 15; ++frame) {
      machine->StepFrame();
      Check(machine->GetTiming().FrameCount() == frame,
            "A DMA spanning several fields never skips a display event");
      Check(output.videoCalls == frame + 1 && output.audioCalls == frame + 1,
            "Long DMA delivers video and audio on every field");
      Check(output.samples == (machine->GetSmp().CycleCount() + 4) / 32,
            "Resuming DMA neither repeats nor drops audio samples");
      if (frame == 0)
        Check(writes > 0 && writes < 65536 && machine->GetDma().InDma(),
              "The first field suspends inside the first DMA channel");
    }
    Check(writes == 8 * 65536 && !machine->GetDma().AnyDmaEnabled() &&
              machine->GetCpu()->regs().stp,
          "All DMA bytes execute once before CPU reset and the program complete");
  }
}

void ReloadAndDebuggerOperations() {
  unsigned writes = 0;
  auto machine = Machine();
  StartLongDma(*machine, writes);
  machine->StepFrame();
  const auto firstWrites = writes;
  std::string error;
  Check(!machine->LoadCartridge({}, &error), "An invalid replacement image is rejected");
  machine->StepFrame();
  Check(writes > firstWrites && machine->GetTiming().FrameCount() == 1,
        "A failed reload leaves the suspended operation resumable");
  const auto before = machine->GetCpu()->Cycles();
  const auto remaining = machine->GetCpu()->Step();
  Check(writes == 8 * 65536 && !machine->GetDma().InDma() && remaining > 0 &&
            remaining == machine->GetCpu()->Cycles() - before,
        "A debugger step completes the retained CPU operation and DMA exactly once");
  machine->GetCpu()->Step();
  Check(machine->GetCpu()->regs().stp, "The following debugger step executes the next instruction");
  machine->StepFrame();
  Check(machine->GetTiming().HCounter() == 0 &&
            machine->GetTiming().VCounter() == machine->GetTiming().VDisp(),
        "Normal frame execution resumes after debugger instruction stepping");

  writes = 0;
  machine = Machine();
  StartLongDma(*machine, writes);
  machine->StepFrame();
  const auto oldWrites = writes;
  Check(machine->LoadCartridge(Rom(true), &error), "A valid replacement cartridge loads");
  machine->StepFrame();
  Check(writes == oldWrites && machine->CurrentFrame() == 1 &&
            machine->GetTiming().FrameCount() == 0 && !machine->GetDma().InDma(),
        "Reload unwinds old DMA before replacing its mapped devices");

  writes = 0;
  machine = Machine();
  StartLongDma(*machine, writes);
  machine->StepFrame();
  machine->GetCpu()->Reset();
  const auto resetWrites = writes;
  Check(!machine->GetDma().InDma(), "Reset unwinds the suspended DMA guard");
  machine->GetDma().EnableDma(0);
  machine->StepFrame();
  Check(writes == resetWrites && machine->GetCpu()->regs().stp,
        "Reset replaces the retained CPU stack without replaying its pending bus phase");

  writes = 0;
  machine = Machine();
  StartLongDma(*machine, writes);
  machine->StepFrame();
  const auto finalWrites = writes;
  machine.reset();
  Check(writes == finalWrites, "Destruction cancels a paused DMA without further bus writes");
}

void ManualClocksAtDisplayEvent() {
  for (bool reset : {false, true}) {
    auto machine = Machine();
    machine->StepFrame();
    auto &timing = machine->GetTiming();
    unsigned nmiPoints = 0;
    timing.onNmiPoint = [&]() { ++nmiPoints; };
    const auto poll = timing.onIrqPoll;
    uint64_t previousPoll = 0;
    timing.onIrqPoll = [&](uint16_t h, uint16_t v, uint16_t vdisp, uint16_t period) {
      Check(timing.MasterClocksElapsed() > previousPoll,
            "Resuming a timing callback never repeats an interrupt sampling edge");
      previousPoll = timing.MasterClocksElapsed();
      poll(h, v, vdisp, period);
    };
    if (reset)
      machine->GetCpu()->Reset();
    machine->GetCpu()->idle();
    Check(nmiPoints == 1, "Manual clocks immediately after a frame retain the VBlank NMI edge");
    machine->StepFrame();
    Check(nmiPoints == 1, "The following frame ends before its next NMI edge");
  }
}
} // namespace

int main() {
  try {
    BoundaryDelivery();
    DmaAcrossFields();
    ReloadAndDebuggerOperations();
    ManualClocksAtDisplayEvent();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("Frame boundary and suspended DMA checks passed");
}
