#include "ConsoleConfig.hpp"
#include "audio/MsuFixture.hpp"

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
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<uint8_t> Rom(std::string_view title, bool pal = false) {
  msu_test::Program p;
  p.Store(0x01ff, 0);
  const auto sound = p.Sound(true);
  p.Delay(4);
  p.Store(0x01ff, 0xa5);
  p.Emit({0xdb});
  std::vector<uint8_t> image(32768, 0xea);
  std::copy(p.code.begin(), p.code.end(), image.begin());
  std::copy(sound.begin(), sound.end(), image.begin() + 0x400);
  std::fill_n(image.begin() + 0x7fc0, 21, ' ');
  std::copy(title.begin(), title.end(), image.begin() + 0x7fc0);
  const std::array<uint8_t, 11> header{0x20, 0, 5, 0, uint8_t(pal ? 2 : 1), 0, 0, 255, 255, 0, 0};
  std::copy(header.begin(), header.end(), image.begin() + 0x7fd5);
  image[0x7ffc] = 0;
  image[0x7ffd] = 0x80;
  return image;
}

struct Audio final : IAudioOutput {
  std::vector<float> samples;
  void Submit(const AudioBuffer &buffer) override {
    Check(buffer.sampleRate == 32040, "Output stays fixed when the input oscillator changes");
    samples.insert(samples.end(), buffer.interleavedStereo.begin(), buffer.interleavedStereo.end());
  }
};

uint64_t Digest(std::span<const float> samples) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (float sample : samples) {
    Check(std::isfinite(sample) && sample >= -1 && sample <= 1,
          "Converted audio stays finite and clipped");
    const auto word = uint16_t(std::lrint(double(sample) * 32768));
    for (unsigned byte = 0; byte < 2; ++byte) {
      hash ^= uint8_t(word >> (8 * byte));
      hash *= 0x100000001b3ull;
    }
  }
  return hash;
}

void StartupAndSound() {
  struct Expected {
    size_t frames;
    uint64_t hash, hardwareClocks;
    uint8_t phase;
  };
  constexpr std::array<Expected, 32> expected{{{8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {8456u, 0x28df44ab97f1fc21ull, 541111ull, 28u},
                                               {8456u, 0x28df44ab97f1fc21ull, 541111ull, 28u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {10073u, 0x94525e55ed8e6d55ull, 644659ull, 26u},
                                               {10073u, 0x94525e55ed8e6d55ull, 644659ull, 26u},
                                               {8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {8456u, 0x56f6ad989aec9125ull, 541111ull, 28u},
                                               {8455u, 0xa747d0e81ad973cdull, 540431ull, 8u},
                                               {8455u, 0xa747d0e81ad973cdull, 540431ull, 8u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {10073u, 0x24bb7ff9159cdd9dull, 644659ull, 26u},
                                               {10073u, 0x94ef736afb1bec51ull, 643861ull, 11u},
                                               {10073u, 0x94ef736afb1bec51ull, 643861ull, 11u}}};
  unsigned index = 0;
  for (std::string_view title : {"CONTROL", "DIRT RACER", "MAGICAL DROP", "RENDERING RANGER R2"})
    for (bool pal : {false, true})
      for (bool enabled : {false, true}) {
        auto config = snes::tests::DeterministicConsoleConfig();
        config.gameCompatibilityFixes = enabled;
        auto machine = std::make_unique<Emulator>(config);
        Check(machine->LoadCartridge(Rom(title, pal)), "The generated compatibility program loads");
        Audio audio;
        machine->AttachAudioOutput(&audio);
        std::array<uint8_t, MemoryBus::WramSize> retained{};
        for (bool warm : {false, true}) {
          if (warm) {
            std::copy_n(machine->GetBus().WramData(), retained.size(), retained.begin());
            Check(machine->Reset(), "Compatibility rules also apply during warm reset");
          }
          const bool dirt = enabled && title == "DIRT RACER";
          const bool magical = enabled && title == "MAGICAL DROP";
          const uint32_t rate = enabled && title == "RENDERING RANGER R2" ? 32000 : 32040;
          Check(machine->GetSmp().SampleRate() == rate &&
                    machine->GetSmp().HardwareClockFrequency() == uint64_t(rate) * 64,
                "The cartridge selects the physical audio oscillator");
          for (size_t byte = 0; byte < retained.size(); ++byte)
            Check(machine->GetBus().WramData()[byte] == (dirt   ? 0xff
                                                         : warm ? retained[byte]
                                                                : 0),
                  "The startup rule fills all WRAM while ordinary warm reset retains it");
          if (!warm || magical)
            for (unsigned reg = 0; reg < 128; ++reg)
              Check(machine->GetDsp().Read(uint8_t(reg)) == (magical ? (reg == 0x7c ? 0 : 0xff)
                                                             : reg == 0x6c ? 0xe0
                                                                           : 0),
                    "DSP startup writes retain ENDX clearing and the selected register values");
          audio.samples.clear();
          for (unsigned field = 0; field < 16; ++field)
            machine->StepFrame();
          const auto &snapshot = expected[index++];
          Check(machine->GetBus().Read(0x7e01ff) == 0xa5,
                "Both processors complete the upload handshake");
          Check(std::count_if(audio.samples.begin(), audio.samples.end(),
                              [](float value) { return std::abs(value) > 1.0f / 32768; }) > 1000,
                "The sound program produces audible output before its PCM signature is checked");
          Check(audio.samples.size() == snapshot.frames * 2 &&
                    Digest(audio.samples) == snapshot.hash,
                "Every converted stereo sample matches the complete cold or warm trace");
          Check(machine->GetSmp().HardwareClockCount() == snapshot.hardwareClocks &&
                    machine->GetDsp().Phase() == snapshot.phase,
                "Handoffs retain the exact sound clock and DSP half-cycle phase");
        }
      }
}

void SelectionAndReload() {
  auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  for (std::string_view title :
       {"DIRT RACER X", "MAGICAL DROP 2", "RENDERING RANGER R2 X", "dirt racer"}) {
    Check(machine->LoadCartridge(Rom(title)), "A similar cartridge title loads");
    Check(machine->GetSmp().SampleRate() == 32040 && machine->GetDsp().Read(0) == 0 &&
              machine->GetBus().WramData()[0] == 0,
          "Selection uses the complete title without prefix or case folding");
  }
  Check(machine->LoadCartridge(Rom("RENDERING RANGER R2")) &&
            machine->GetSmp().SampleRate() == 32000,
        "Loading the affected title selects its oscillator");
  Check(!machine->LoadCartridge(std::array<uint8_t, 1>{0}) &&
            machine->GetSmp().SampleRate() == 32000,
        "A rejected image preserves the running cartridge and oscillator");
  Check(machine->LoadCartridge(Rom("CONTROL")) && machine->GetSmp().SampleRate() == 32040,
        "Replacing the cartridge restores the normal oscillator");
}
} // namespace

int main() {
  try {
    StartupAndSound();
    SelectionAndReload();
    std::puts("Cartridge startup, audio clock and stream conversion checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
