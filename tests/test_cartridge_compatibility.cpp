#include "ConsoleConfig.hpp"
#include "audio/MsuFixture.hpp"
#include "gsu/IrqProgram.hpp"
#include "gsu/IrqSignalProgram.hpp"
#include "sa1/DeferredIrqProgram.hpp"
#include "sa1/MailboxProgram.hpp"
#include "snes/core/SnesCpu.hpp"

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

void LaunchVideoProfiles() {
  struct Scene {
    std::string_view title;
    bool pal, progressive;
    uint64_t digest;
    uint16_t clock = 512;
  };
  constexpr Scene scenes[]{
      {"CONTROL", false, false, 0xf5030600487dab25ull},
      {"AIR STRIKE PATROL", false, true, 0x371f992c6ecefbc1ull},
      {"DESERT FIGHTER", false, true, 0x371f992c6ecefbc1ull},
      {"Winter olympics", true, true, 0x87bd56811dd78991ull},
      {"WORLD CUP STRIKER", true, true, 0x87bd56811dd78991ull},
      {"\xcf\xb0\xb3\xde\xaa\xd7\xbd", false, true, 0x371f992c6ecefbc1ull},
      {"SFC \xb8\xda\xd6\xdd\xbc\xdd\xc1\xac\xdd", false, true, 0x371f992c6ecefbc1ull},
      {"ADVENTURES OF FRANKEN", true, false, 0x3df8a8f4a6534b25ull, 32},
      {"ADVENTURES OF FRANKEN", false, false, 0xf5030600487dab25ull},
      {"FIREPOWER 2000", false, false, 0xb6600e37256de325ull, 32},
      {"SUPER SWIV", true, false, 0x3df8a8f4a6534b25ull, 32},
      {"NHL '94", false, false, 0xb6600e37256de325ull, 32},
      {"NHL PROHOCKEY'94", false, false, 0xb6600e37256de325ull, 32},
      {"Sugoro Quest++", false, false, 0xc3e3e8cfa3631325ull, 128},
      {"FIREPOWER 2000 X", false, false, 0xf5030600487dab25ull},
      {"firepower 2000", false, false, 0xf5030600487dab25ull},
      {"CONTROL", false, false, 0xf5030600487dab25ull},
  };
  struct Video final : IVideoOutput {
    uint64_t digest = 0;
    void Present(const VideoFrame &frame) override {
      digest = 0xcbf29ce484222325ull;
      for (auto pixel : frame.pixels) {
        const auto color =
            uint16_t((((pixel & 255) >> 3) << 10) | ((((pixel >> 8) & 255) >> 3) << 5) |
                     (((pixel >> 16) & 255) >> 3));
        for (unsigned shift : {0u, 8u})
          digest = (digest ^ ((color >> shift) & 255)) * 0x100000001b3ull;
      }
    }
  };
  for (bool fixes : {false, true}) {
    Emulator::Config config;
    config.entropyMode = EntropyMode::None;
    config.entropySeed = 0;
    config.gameCompatibilityFixes = fixes;
    auto machine = std::make_unique<Emulator>(config);
    Video video;
    machine->AttachVideoOutput(&video);
    for (const auto &scene : scenes) {
      auto image = Rom(scene.title, scene.pal);
      std::vector<uint8_t> code{0x78};
      const auto port = [&](uint16_t address, uint8_t value) {
        code.insert(code.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
      };
      port(0x2100, 0x80);
      port(0x2121, 0);
      port(0x2122, 0x1f);
      port(0x2122, 0);
      port(0x2100, 15);
      const auto loop = code.size();
      port(0x2100, 15);
      code.insert(code.end(), {0xa2, 11, 0xca, 0xd0, 0xfd});
      port(0x2100, 4);
      code.insert(code.end(), {0xa2, 9, 0xca, 0xd0, 0xfd});
      code.insert(code.end(), {0x80, uint8_t(int(loop) - int(code.size()) - 2)});
      std::copy(code.begin(), code.end(), image.begin());
      Check(machine->LoadCartridge(image), "The video compatibility cartridge loads");
      for (bool warm : {false, true}) {
        if (warm)
          Check(machine->Reset(), "Video compatibility survives a warm reset");
        for (unsigned frame = 0; frame < 3; ++frame)
          machine->StepFrame({false, false, false});
        Check(video.digest == scene.digest,
              "The cartridge selects its renderer and sampling clock on cold and warm starts");
        Check(machine->GetPpu().Configuration().model ==
                  (scene.progressive ? Ppu::RenderModel::Progressive : Ppu::RenderModel::Scanline),
              "Renderer selection follows the complete cartridge title");
        Check(machine->GetTiming().RenderCycle() == scene.clock,
              "Cold and warm starts retain the selected video event clock");
      }
    }
  }
  auto config = snes::tests::DeterministicConsoleConfig();
  auto progressive = std::make_unique<Emulator>(config);
  Check(progressive->LoadCartridge(Rom("FIREPOWER 2000")) &&
            progressive->GetPpu().Configuration().model == Ppu::RenderModel::Progressive &&
            progressive->GetTiming().RenderCycle() == 512,
        "An explicit progressive selection keeps its clocked video path");
}

void StartupEntropyProfiles() {
  for (bool fixes : {false, true}) {
    Emulator::Config config;
    config.entropyMode = EntropyMode::High;
    config.entropySeed = 0x12345678;
    config.gameCompatibilityFixes = fixes;
    auto machine = std::make_unique<Emulator>(config);
    for (std::string_view title :
         {"The Hurricanes",
          "\xc6\xc1\xcc\xde\xc2\xa5\xb1\xb0\xb9\xb0\xc4\xde\xa5\xb8\xd7\xbc\xaf\xb8\xbd",
          "CONTROL"}) {
      Check(machine->LoadCartridge(Rom(title)), "The startup compatibility cartridge loads");
      const auto *memory = machine->GetBus().WramData();
      const bool empty = std::all_of(memory, memory + MemoryBus::WramSize,
                                     [](uint8_t value) { return value == 0; });
      Check(empty == (fixes && title != "CONTROL"),
            "Selected startup fixes override entropy while other cartridges retain the request");
    }
  }
}
void Sa1DeferredDma() {
  constexpr std::array<std::array<uint8_t, 12>, 2> expected{
      {{0x5a, 0x5a, 0x5a, 0x5a, 0, 0x5a, 0x5a, 0x5a, 0, 0, 0, 0x5a},
       {0x5a, 0x5a, 0x5a, 0x5a, 0, 0, 0x5a, 0x5a, 0, 0, 0, 0x5a}}};
  for (bool contention : {false, true})
    for (bool pal : {false, true}) {
      unsigned scene = 0;
      for (uint8_t count : {1, 7, 31})
        for (unsigned delay : {0, 5, 19, 63}) {
          auto config = snes::tests::DeterministicConsoleConfig();
          config.sa1BusContention = contention;
          auto machine = std::make_unique<Emulator>(config);
          Check(machine->LoadCartridge(snes::tests::sa1::DeferredDmaProgram(count, delay, pal)),
                "The deferred shared-memory DMA cartridge loads");
          for (bool warm : {false, true}) {
            if (warm)
              Check(machine->Reset(), "Reset repeats DMA handoffs with the selected timing");
            machine->StepFrame({false, false, false});
            Check(machine->GetCpu()->regs().stp &&
                      machine->GetBus().WramData()[0x1000] == expected[contention ? 1 : 0][scene],
                  "DMA reads synchronize shared memory independently of the last CPU bus address");
          }
          ++scene;
        }
    }
}

void Sa1DeferredInterrupts() {
  constexpr std::array<std::array<std::array<uint8_t, 4>, 24>, 2> expected{
      {{{{23, 22, 3, 2}, {26, 26, 3, 2}, {24, 24, 3, 2}, {23, 22, 3, 2}, {25, 24, 1, 0},
         {23, 22, 1, 0}, {26, 26, 1, 0}, {24, 24, 1, 0}, {25, 24, 1, 0}, {23, 22, 1, 0},
         {26, 26, 1, 0}, {24, 24, 1, 0}, {24, 24, 2, 2}, {22, 22, 2, 2}, {25, 24, 2, 2},
         {24, 24, 2, 2}, {24, 24, 2, 2}, {22, 22, 2, 2}, {25, 24, 2, 2}, {24, 24, 2, 2},
         {23, 22, 3, 2}, {26, 26, 3, 2}, {24, 24, 3, 2}, {23, 22, 3, 2}}},
       {{{0, 0, 4, 4},     {3, 2, 4, 4},     {1, 0, 4, 4},     {0, 0, 4, 4},     {3, 2, 1, 0},
         {6, 6, 1, 0},     {3, 2, 1, 0},     {3, 2, 1, 0},     {30, 30, 1, 0},   {28, 28, 1, 0},
         {31, 30, 1, 0},   {27, 26, 1, 0},   {124, 124, 1, 0}, {127, 126, 1, 0}, {125, 124, 1, 0},
         {124, 124, 1, 0}, {171, 170, 2, 2}, {169, 168, 2, 2}, {167, 166, 2, 2}, {172, 172, 2, 2},
         {0, 0, 4, 4},     {82, 83, 3, 2},   {79, 79, 3, 2},   {0, 0, 4, 4}}}}};
  for (bool contention : {false, true})
    for (bool pal : {false, true}) {
      unsigned scene = 0;
      for (uint8_t count : {0, 1, 7, 31, 127, 255})
        for (unsigned delay : {0, 1, 5, 19}) {
          auto config = snes::tests::DeterministicConsoleConfig();
          config.sa1BusContention = contention;
          auto machine = std::make_unique<Emulator>(config);
          Check(machine->LoadCartridge(snes::tests::sa1::DeferredIrqProgram(count, delay, pal)),
                "The deferred interrupt cartridge loads");
          for (bool warm : {false, true}) {
            if (warm)
              Check(machine->Reset(), "Reset restarts deferred interrupt delivery");
            machine->StepFrame({false, false, false});
            const auto *memory = machine->GetBus().WramData() + 0x1000;
            const auto &counters = expected[contention ? 1 : 0][scene];
            Check(std::equal(counters.begin(), counters.end(), memory) && memory[8] == 0xa5 &&
                      machine->GetCpu()->regs().stp && !machine->LoadedCartridge()->CpuIrqPending(),
                  "Interrupt delivery preserves raster timing, acknowledgement and completion");
            for (unsigned byte = 4; byte < 16; ++byte)
              Check(memory[byte] == (byte == 8 ? 0xa5 : 0),
                    "Interrupt results preserve the complete guest output area");
          }
          ++scene;
        }
    }
}

void Sa1ConsoleMailbox() {
  constexpr std::array<std::array<uint8_t, 3>, 32> expected{
      {{1, 1, 170},  {1, 1, 174},  {1, 1, 177},  {1, 1, 181}, {2, 1, 187}, {1, 1, 174}, {1, 1, 177},
       {1, 1, 181},  {2, 1, 187},  {1, 1, 174},  {1, 1, 177}, {1, 1, 181}, {2, 1, 187}, {2, 1, 190},
       {2, 1, 194},  {1, 1, 181},  {4, 1, 220},  {3, 1, 207}, {3, 1, 210}, {3, 1, 214}, {6, 1, 253},
       {6, 1, 0},    {6, 1, 4},    {6, 1, 7},    {12, 1, 11}, {12, 1, 14}, {12, 1, 18}, {12, 1, 21},
       {23, 1, 202}, {23, 1, 206}, {23, 1, 209}, {23, 1, 213}}};
  constexpr std::array<std::array<uint8_t, 3>, 32> contended{
      {{2, 1, 187},  {1, 1, 174},  {1, 1, 177},  {1, 1, 181},  {2, 1, 187}, {1, 1, 174},
       {1, 1, 177},  {1, 1, 181},  {2, 1, 187},  {2, 1, 190},  {2, 1, 194}, {2, 1, 197},
       {3, 1, 203},  {3, 1, 207},  {3, 1, 210},  {3, 1, 214},  {6, 1, 253}, {5, 1, 240},
       {5, 1, 243},  {5, 1, 247},  {10, 1, 63},  {10, 1, 66},  {10, 1, 69}, {10, 1, 72},
       {19, 1, 126}, {19, 1, 130}, {19, 1, 143}, {19, 1, 147}, {37, 1, 92}, {37, 1, 96},
       {37, 1, 99},  {36, 1, 86}}};
  for (bool contention : {false, true}) {
    unsigned index = 0;
    for (uint8_t count : {1, 2, 4, 8, 16, 32, 64, 128})
      for (unsigned delay = 0; delay < 4; ++delay) {
        auto config = snes::tests::DeterministicConsoleConfig();
        if (contention)
          config.sa1BusContention = true;
        auto machine = std::make_unique<snes::core::Emulator>(config);
        Check(machine->LoadCartridge(snes::tests::sa1::MailboxProgram(count, delay)),
              "The generated SA-1 mailbox cartridge loads");
        for (bool warm : {false, true}) {
          if (warm)
            Check(machine->Reset(), "Console reset retains the SA-1 execution selection");
          machine->StepFrame({false, false, false});
          const auto &result = contention ? contended[index] : expected[index];
          Check(machine->GetCpu()->regs().stp &&
                    std::equal(result.begin(), result.end(), machine->GetBus().WramData() + 0x1000),
                "SA-1 mailbox polling and beam latches follow the selected contention mode");
        }
        ++index;
      }
  }
}

void SuperFxIrqHandoffs() {
  constexpr std::array<std::array<std::array<uint8_t, 5>, 28>, 2> expected{
      {{{{26, 26, 16, 16, 165}, {26, 26, 16, 16, 165}, {26, 26, 16, 16, 165},
         {26, 26, 16, 16, 165}, {26, 26, 16, 16, 165}, {26, 26, 16, 16, 165},
         {26, 26, 16, 16, 165}, {26, 26, 16, 16, 165}, {26, 26, 16, 16, 165},
         {28, 28, 17, 16, 165}, {28, 28, 17, 16, 165}, {28, 28, 17, 16, 165},
         {28, 28, 17, 16, 165}, {28, 28, 17, 16, 165}, {28, 28, 17, 16, 165},
         {28, 28, 17, 16, 165}, {13, 13, 15, 14, 0},   {13, 13, 15, 14, 0},
         {78, 79, 15, 14, 1},   {13, 13, 15, 14, 0},   {13, 13, 15, 14, 0},
         {46, 47, 15, 14, 0},   {25, 24, 16, 16, 0},   {26, 26, 16, 16, 47},
         {13, 13, 15, 14, 0},   {13, 13, 15, 14, 4},   {13, 13, 15, 14, 0},
         {26, 26, 16, 16, 53}}},
       {{{58, 58, 15, 14, 165},   {92, 92, 15, 14, 165},   {127, 126, 15, 14, 165},
         {171, 170, 15, 14, 165}, {206, 206, 15, 14, 165}, {240, 240, 15, 14, 165},
         {19, 19, 15, 14, 165},   {53, 53, 15, 14, 165},   {3, 2, 16, 16, 165},
         {37, 36, 16, 16, 165},   {72, 72, 16, 16, 165},   {106, 106, 16, 16, 165},
         {151, 150, 16, 16, 165}, {185, 184, 16, 16, 165}, {220, 220, 16, 16, 165},
         {254, 254, 16, 16, 165}, {13, 13, 15, 14, 0},     {13, 13, 15, 14, 0},
         {78, 79, 15, 14, 1},     {13, 13, 15, 14, 0},     {13, 13, 15, 14, 0},
         {46, 47, 15, 14, 0},     {47, 47, 15, 14, 234},   {13, 13, 15, 14, 47},
         {13, 13, 15, 14, 0},     {13, 13, 15, 14, 4},     {13, 13, 15, 14, 0},
         {13, 13, 15, 14, 53}}}}};
  for (bool delayed : {true, false}) {
    for (bool pal : {false, true}) {
      for (unsigned scene = 0; scene < 28; ++scene) {
        const unsigned phase = scene < 16 ? scene : 0;
        const auto probe =
            scene < 16 ? snes::tests::gsu::Probe::None : snes::tests::gsu::Probe(scene - 15);
        auto config = snes::tests::DeterministicConsoleConfig();
        if (!delayed)
          config.superFxDelayedSync = false;
        auto machine = std::make_unique<Emulator>(config);
        Check(machine->LoadCartridge(snes::tests::gsu::IrqProgram(phase, pal, probe)),
              "The generated graphics IRQ cartridge loads");
        for (bool warm : {false, true}) {
          if (warm)
            Check(machine->Reset(), "Graphics IRQ execution also follows warm reset");
          machine->StepFrame({false, false, false});
          const auto *memory = machine->GetBus().WramData();
          const std::array<uint8_t, 5> output{memory[0x1200], memory[0x1201], memory[0x1202],
                                              memory[0x1203], memory[0x1210]};
          Check(machine->GetCpu()->regs().stp && memory[0x42] == 1 &&
                    output == expected[delayed ? 0 : 1][scene],
                "Graphics IRQ delivery follows the selected synchronization and register window");
        }
      }
    }
  }
}

void SuperFxIrqOutput() {
  for (bool delayed : {true, false})
    for (bool pal : {false, true})
      for (unsigned scene = 0; scene < 9; ++scene) {
        auto config = snes::tests::DeterministicConsoleConfig();
        if (!delayed)
          config.superFxDelayedSync = false;
        auto machine = std::make_unique<Emulator>(config);
        Check(machine->LoadCartridge(snes::tests::gsu::IrqSignalProgram(scene, pal)),
              "The graphics IRQ output program loads");
        for (bool warm : {false, true}) {
          if (warm)
            Check(machine->Reset(), "Warm reset clears the previous graphics IRQ output");
          machine->StepFrame({false, false, false});
          Check(
              machine->GetCpu()->regs().stp &&
                  machine->GetBus().WramData()[0x1200] == (scene == 8 ? 1 : 0) &&
                  machine->LoadedCartridge()->CpuIrqPending() == (scene == 2 || scene == 3),
              "Graphics IRQ output follows STOP and acknowledge reads independently of SFR writes");
        }
      }
}

} // namespace

int main() {
  try {
    SuperFxIrqOutput();
    SuperFxIrqHandoffs();
    Sa1DeferredDma();
    Sa1DeferredInterrupts();
    Sa1ConsoleMailbox();
    StartupAndSound();
    SelectionAndReload();
    LaunchVideoProfiles();
    StartupEntropyProfiles();
    std::puts("Cartridge startup, audio clock and stream conversion checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
