#include "audio/DspBatchProgram.hpp"
#include "snes/core/Emulator.hpp"
#include "snes/core/SnesCpu.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

using namespace snes::core;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct Hash {
  uint64_t value = 0xcbf29ce484222325ull;
  void Byte(uint8_t byte) {
    value = (value ^ byte) * 0x100000001b3ull;
  }
  void Word(uint64_t word) {
    for (unsigned byte = 0; byte < 8; ++byte)
      Byte(uint8_t(word >> (byte * 8)));
  }
  void Bytes(std::span<const uint8_t> bytes) {
    for (uint8_t byte : bytes)
      Byte(byte);
  }
};

struct Audio final : IAudioOutput {
  Hash hash;
  uint64_t frames = 0, audible = 0;
  void Submit(const AudioBuffer &buffer) override {
    Check(buffer.sampleRate == 32040, "Both DSP modes keep the normal output rate");
    frames += buffer.interleavedStereo.size() / 2;
    for (float sample : buffer.interleavedStereo) {
      Check(std::isfinite(sample) && sample >= -1 && sample <= 1,
            "DSP output remains finite and clipped");
      const auto word = uint16_t(std::lrint(double(sample) * 32768));
      hash.Byte(uint8_t(word));
      hash.Byte(uint8_t(word >> 8));
      audible += word != 0;
    }
  }
};

void Snapshot(Emulator &machine, const Audio &audio, Hash &hash) {
  const auto &cpu = machine.GetCpu()->regs();
  const std::array<uint64_t, 15> registers{uint64_t(cpu.pb) << 16 | cpu.pc,
                                           cpu.a,
                                           cpu.x,
                                           cpu.y,
                                           cpu.s,
                                           cpu.d,
                                           cpu.db,
                                           cpu.p,
                                           cpu.e,
                                           machine.LoadedCartridge()->CpuIrqPending(),
                                           cpu.wai,
                                           cpu.stp,
                                           cpu.mar,
                                           cpu.mdr,
                                           cpu.vector};
  for (auto value : registers)
    hash.Word(value);
  const auto &smp = machine.GetSmp().r;
  for (auto value : std::array<uint64_t, 8>{smp.pc, smp.a, smp.x, smp.y, smp.s, smp.p.Pack(),
                                            smp.wait, smp.stop})
    hash.Word(value);
  for (auto value :
       {machine.GetTiming().MasterClocksElapsed(), machine.GetSmp().HardwareClockCount(),
        uint64_t(machine.GetDsp().Phase()), audio.frames, audio.hash.value})
    hash.Word(value);
  hash.Bytes({machine.GetBus().WramData(), MemoryBus::WramSize});
  hash.Bytes({machine.GetSmp().Ram(), Smp::RamSize});
  hash.Bytes(machine.GetDsp().Regs());
}

void BusBoundaries() {
  Smp smp;
  Dsp dsp;
  std::array<int16_t, 16> samples{};
  smp.SetDsp(dsp);
  smp.SetDspMode(Smp::DspMode::Batch32);
  dsp.SetRam(smp.Ram());
  dsp.SetOutput(samples.data(), int(samples.size() / 2));
  for (unsigned cycle = 0; cycle < 32; ++cycle)
    smp.Idle();
  Check(dsp.SamplesWritten() == 0, "Idle cycles retain DSP debt until a memory access");
  smp.Read(0x200);
  Check(dsp.SamplesWritten() == 2 && dsp.Phase() == 0,
        "A memory access repays idle debt in complete 32-clock batches");
  for (unsigned cycle = 0; cycle < 31; ++cycle)
    smp.Idle();
  Check(dsp.SamplesWritten() == 2, "Idle cycles consume the retained DSP clock lead");
  smp.Read(0x200);
  Check(dsp.SamplesWritten() == 3, "The next batch begins only after exhausting that lead");
  smp.Power(true);
  dsp.SoftReset();
  dsp.ResetSamplesWritten();
  smp.Read(0x200);
  Check(smp.GetDspMode() == Smp::DspMode::Batch32 && dsp.SamplesWritten() == 1,
        "Power clears DSP clock debt while preserving the chosen mode");

  smp.Power();
  dsp.Power();
  dsp.SetOutput(samples.data(), int(samples.size() / 2));
  smp.IoState().iplRomEnable = false;
  smp.r.pc = 0x200;
  // Fifteen NOPs place a port-read midpoint at hardware clock 65.
  std::fill_n(smp.Ram() + 0x200, 15, uint8_t{0});
  smp.Ram()[0x20f] = 0xe4;
  smp.Ram()[0x210] = 0xf4;
  smp.PortWrite(0, 0x5a);
  smp.RunUntilHardwareClock(65);
  Check(dsp.SamplesWritten() == 2 && smp.r.a == 0,
        "A port-read midpoint starts the next DSP batch before the read completes");
  smp.RunUntilHardwareClock(66);
  Check(dsp.SamplesWritten() == 2 && smp.r.a == 0x5a,
        "Completing the held port read preserves the batch and sampled input");
}

void LaunchModes() {
  struct Scene {
    const char *title;
    bool pal, perCycle;
  };
  constexpr Scene scenes[]{{"CONTROL", false, false},      {"KOUSHIEN_2", false, true},
                           {"KOUSHIEN_2", true, true},     {"RENDERING RANGER R2", false, true},
                           {"BUBSY II", false, false},     {"BUBSY II", true, true},
                           {"KOUSHIEN_2 X", false, false}, {"koushien_2", false, false},
                           {"BUBSY II X", true, false}};
  for (bool hotfixes : {false, true}) {
    for (bool batch : {false, true}) {
      Emulator::Config config;
      config.entropyMode = EntropyMode::None;
      config.entropySeed = 0;
      config.gameCompatibilityFixes = hotfixes;
      if (!batch)
        config.dspMode = Smp::DspMode::PerCycle;
      auto machine = std::make_unique<Emulator>(config);
      for (const auto &scene : scenes) {
        auto rom = dsp_batch_test::Rom(scene.pal, 0, 0);
        std::fill_n(rom.begin() + 0x7fc0, 21, uint8_t{' '});
        std::copy_n(scene.title, std::char_traits<char>::length(scene.title), rom.begin() + 0x7fc0);
        Check(machine->LoadCartridge(rom), "Each DSP launch profile replaces the prior cartridge");
        const auto expected =
            scene.perCycle || !batch ? Smp::DspMode::PerCycle : Smp::DspMode::Batch32;
        Check(machine->GetSmp().GetDspMode() == expected,
              "Exact title and region select DSP execution independently of optional hotfixes");
        machine->StepFrame({false, false, false});
        Check(machine->Reset() && machine->GetSmp().GetDspMode() == expected,
              "Warm reset retains the selected title's DSP profile");
      }
    }
  }
}

void PortBackingRam() {
  for (auto mode : {Smp::DspMode::PerCycle, Smp::DspMode::Batch32}) {
    Smp smp;
    smp.SetDspMode(mode);
    smp.IoState().iplRomEnable = false;
    smp.r.pc = 0x200;
    constexpr std::array<uint8_t, 8> program{0x8f, 0xa5, 0xf4, 0x8f, 0xb6, 0xf5, 0x2f, 0xf8};
    std::copy(program.begin(), program.end(), smp.Ram() + 0x200);
    const auto frequency = uint32_t(smp.HardwareClockFrequency());
    auto handoff = [&] { smp.SynchronizeCpu(smp.HardwareClockCount() + 1, frequency); };
    // MOV dp,#imm first performs a dummy read of the same port.
    handoff();
    handoff();
    Check(smp.Ram()[0xf4] == 0xa5 && smp.PortRead(0) == 0,
          "A pending communication write reaches backing RAM before changing its output latch");
    handoff();
    handoff();
    Check(smp.PortRead(0) == 0xa5 && smp.Ram()[0xf5] == 0xb6 && smp.PortRead(1) == 0,
          "Resuming commits the first latch before the next write's RAM and console handoff");
  }
}

void ConsolePrograms() {
  // Each trace includes every field's complete work RAM, audio RAM, DSP
  // registers, processor registers, clocks, output count and PCM digest.
  constexpr std::array<uint64_t, 32> expected{
      0xa7247bf7811240f4ull, 0x2e7043200e1c18abull, 0x85d0c08595ea5cf4ull, 0x512d4d2cf5d2c962ull,
      0x6e879a50a7d597dfull, 0x59dc78c6425eaafdull, 0xb2a10f2148076d52ull, 0x1285dcae2e8c37e2ull,
      0x894b8851845f338cull, 0xd2c3c2d0fc42ce65ull, 0x4ff6858e326290bfull, 0x718595f441f54ed2ull,
      0x4e2f4bcd2c805903ull, 0x33d473c2a63d16fbull, 0xbbb00f6efe869dcfull, 0xb1eec5d77a9b56f2ull,
      0x63d06fb65624e3deull, 0xd398abb6bed2df22ull, 0xf9848011cd1b28caull, 0xd20537a8e63659d0ull,
      0xeaabe5f4bde5ae8dull, 0x5b57e37db6eaa55eull, 0x81ebc9a6f2fc8d7cull, 0xcfa4091fb5fca586ull,
      0xeacda708357a2e6full, 0xda9213f13ab96809ull, 0x6231fd3a367cdbebull, 0x1d19b5150e223016ull,
      0x5f846bc3491b570bull, 0x96c1f69a3b70fd07ull, 0xc73eb28b9e4d156bull, 0x45e72c4ac2325bc9ull};
  unsigned index = 0;
  for (bool batch : {false, true}) {
    for (bool pal : {false, true}) {
      for (unsigned phase : {0, 15}) {
        for (unsigned waits : {0, 17, 34, 51}) {
          Emulator::Config config;
          config.entropyMode = EntropyMode::None;
          config.entropySeed = 0;
          Check(config.dspMode == Smp::DspMode::Batch32,
                "Console defaults to 32-clock DSP batches");
          if (!batch)
            config.dspMode = Smp::DspMode::PerCycle;
          auto machine = std::make_unique<Emulator>(config);
          Audio audio;
          machine->AttachAudioOutput(&audio);
          Check(machine->LoadCartridge(dsp_batch_test::Rom(pal, phase, waits)),
                "The DSP fixture loads through the normal cartridge interface");
          Hash trace;
          for (bool warm : {false, true}) {
            if (warm)
              Check(machine->Reset(), "The sound program restarts through console reset");
            Check(machine->GetSmp().GetDspMode() == config.dspMode,
                  "Cold power and warm reset retain the selected DSP mode");
            for (unsigned field = 0; field < 24; ++field) {
              machine->StepFrame({false, false, false});
              Snapshot(*machine, audio, trace);
            }
            Check(machine->GetBus().WramData()[0x1ff] == 0xa5 && audio.audible > 1000,
                  "Both processors finish IPL upload and produce audible output");
          }
          if (trace.value != expected[index]) {
            std::fprintf(stderr, "DSP trace batch=%u PAL=%u phase=%u waits=%u: %016llx\n", batch,
                         pal, phase, waits, static_cast<unsigned long long>(trace.value));
            throw std::runtime_error(
                "DSP execution, register reads, shared RAM and PCM match the recorded trace");
          }
          ++index;
        }
      }
    }
  }
}
} // namespace

int main() {
  try {
    BusBoundaries();
    PortBackingRam();
    LaunchModes();
    ConsolePrograms();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("DSP batch and per-cycle console traces passed");
}
