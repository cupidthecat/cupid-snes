#include "ConsoleConfig.hpp"
#include "audio/MsuFixture.hpp"
#include "snes/core/SnesCpu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace snes::core;

namespace {
using PcmFrame = std::array<int16_t, 2>;

void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

class Fixture {
public:
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      auto candidate = std::filesystem::current_path() /
                       ("msu1-console-" + std::to_string(stamp) + "-" + std::to_string(attempt));
      if (std::filesystem::create_directory(candidate)) {
        directory_ = std::move(candidate);
        return;
      }
    }
    throw std::runtime_error("Cannot create MSU-1 console fixture");
  }

  ~Fixture() {
    std::error_code error;
    for (const auto &path : files_)
      std::filesystem::remove(path, error);
    std::filesystem::remove(directory_, error);
  }

  std::filesystem::path Write(const char *name, std::span<const uint8_t> bytes) {
    const auto path = directory_ / name;
    files_.push_back(path);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    Check(bool(output), "Write console fixture");
    return path;
  }

  void Pack(unsigned frameCount = 6000) {
    const std::vector<PcmFrame> frames(frameCount, PcmFrame{10000, -7000});
    Pack(frames);
  }

  void Pack(std::span<const PcmFrame> frames, uint32_t loopFrame = 0) {
    std::array<uint8_t, 256> data{};
    for (unsigned i = 0; i < data.size(); ++i)
      data[i] = static_cast<uint8_t>(i ^ 0x59);
    Write("game.msu", data);
    std::vector<uint8_t> pcm{'M', 'S', 'U', '1', 0, 0, 0, 0};
    for (unsigned byte = 0; byte < 4; ++byte)
      pcm[4 + byte] = static_cast<uint8_t>(loopFrame >> (8 * byte));
    for (const auto &frame : frames) {
      for (const int16_t value : frame) {
        const auto bits = static_cast<uint16_t>(value);
        pcm.push_back(static_cast<uint8_t>(bits));
        pcm.push_back(static_cast<uint8_t>(bits >> 8));
      }
    }
    Write("game-1.pcm", pcm);
  }

private:
  std::filesystem::path directory_;
  std::vector<std::filesystem::path> files_;
};

std::vector<uint8_t> Rom(std::initializer_list<uint8_t> code = {0x80, 0xfe}, bool pal = false) {
  std::vector<uint8_t> rom(0x8000, 0xea);
  std::copy(code.begin(), code.end(), rom.begin());
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0x7fc0] = 'T';
  rom[0x7fd5] = 0x20;
  rom[0x7fd6] = 0;
  rom[0x7fd7] = 5;
  rom[0x7fd8] = 0;
  rom[0x7fd9] = pal ? 2 : 1;
  rom[0x7fdc] = 0xff;
  rom[0x7fdd] = 0xff;
  rom[0x7fde] = 0;
  rom[0x7fdf] = 0;
  rom[0x7ffc] = 0;
  rom[0x7ffd] = 0x80;
  return rom;
}

std::unique_ptr<Emulator> Load(const std::filesystem::path &path) {
  auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  std::string error;
  if (!machine->LoadCartridgeFromFile(path.string(), &error))
    throw std::runtime_error(error);
  return machine;
}

struct Audio final : IAudioOutput {
  std::vector<float> samples;
  void Submit(const AudioBuffer &buffer) override {
    Check(buffer.sampleRate == AudioTiming::SampleRate, "Mixed PCM follows the core output rate");
    samples.insert(samples.end(), buffer.interleavedStereo.begin(), buffer.interleavedStereo.end());
  }
};

void MappingAndLifecycle() {
  Fixture fixture;
  fixture.Pack();
  const auto rom = Rom();
  const auto path = fixture.Write("game.sfc", rom);
  auto machine = Load(path);
  auto &bus = machine->GetBus();
  for (uint32_t bank = 0; bank <= 0xbf; ++bank) {
    if (bank & 0x40)
      continue;
    Check(bus.Read((bank << 16) | 0x2000) == 0x0a,
          "MSU-1 status maps across both system-bank ranges");
    Check(bus.Read((bank << 16) | 0x2002) == 'S' && bus.Read((bank << 16) | 0x2007) == '1',
          "MSU-1 signature has mirrored bus decoding");
  }
  bus.Write(0x7e2002, 0xa5);
  Check(bus.Read(0x7e2002) == 0xa5, "MSU-1 leaves full WRAM banks intact");
  Check(bus.Read(0x2008) == 0xa5, "MSU-1 leaves adjacent addresses as open bus");

  // A fixed A-bus source still advances the data stream on every DMA read.
  bus.Write(0x2181, 0);
  bus.Write(0x2182, 0);
  bus.Write(0x2183, 0);
  bus.Write(0x4300, 8);
  bus.Write(0x4301, 0x80);
  bus.Write(0x4302, 1);
  bus.Write(0x4303, 0x20);
  bus.Write(0x4304, 0x95);
  bus.Write(0x4305, 16);
  bus.Write(0x4306, 0);
  machine->GetDma().EnableDma(1);
  machine->GetDma().RunDma();
  for (unsigned i = 0; i < 16; ++i)
    Check(bus.Read(0x7e0000 + i) == (i ^ 0x59),
          "DMA reads advance the MSU-1 stream through a mirror");

  const std::array<uint8_t, 1> invalid{0};
  Check(!machine->LoadCartridge(invalid), "Invalid ROM is rejected");
  Check(bus.Read(0x2002) == 'S', "Failed ROM loads preserve the active MSU-1 mapping");
  Check(machine->LoadCartridge(rom), "Plain in-memory ROM loads after an MSU-1 game");
  bus.Write(0x7e0000, 0xa5);
  Check(bus.Read(0x2002) == 0xa5, "A new plain cartridge removes old MSU-1 handlers");

  Check(machine->LoadCartridgeFromFile(path.string()), "MSU-1 game can be reloaded");
  Check(bus.Read(0x2000) == 0x0a && bus.Read(0x2001) == 0x59,
        "Reloading resets playback registers and the data position");
  const auto plain = fixture.Write("plain.sfc", rom);
  Check(machine->LoadCartridgeFromFile(plain.string()),
        "Missing optional sidecar permits ordinary ROM loading");
  bus.Write(0x7e0000, 0xa5);
  Check(bus.Read(0x2002) == 0xa5, "A ROM without a sidecar keeps normal expansion-bus behavior");
}

void TrackFile(Fixture &fixture, uint16_t track, std::span<const uint8_t> payload,
               uint32_t loop = 0) {
  std::vector<uint8_t> bytes{'M', 'S', 'U', '1'};
  for (unsigned byte = 0; byte < 4; ++byte)
    bytes.push_back(uint8_t(loop >> (8 * byte)));
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  fixture.Write(("game-" + std::to_string(track) + ".pcm").c_str(), bytes);
}

std::vector<uint8_t> Waveform(unsigned count, unsigned seed = 0) {
  std::vector<uint8_t> bytes;
  for (unsigned i = 0; i < count; ++i)
    for (int value :
         {int((i * 137 + seed) % 60001) - 30000, int((i * 911 + 77 + seed) % 56003) - 28001}) {
      bytes.push_back(uint8_t(value));
      bytes.push_back(uint8_t(value >> 8));
    }
  return bytes;
}

void SceneAssets(Fixture &fixture, unsigned scene) {
  fixture.Write("game.msu", std::array<uint8_t, 2>{0x5a, 0xa5});
  const bool shortTrack = scene == 0 || scene == 3;
  TrackFile(fixture, 1, Waveform(shortTrack ? 3 : 257), shortTrack ? 0 : 23);
  TrackFile(fixture, 2, Waveform(73, 99), 7);
  TrackFile(fixture, 3, {});
  TrackFile(fixture, 4, std::array<uint8_t, 1>{0x12});
  TrackFile(fixture, 5, std::array<uint8_t, 3>{0x34, 0x92, 0x56});
  TrackFile(fixture, 6, Waveform(3), 3);
  TrackFile(fixture, 7, Waveform(3), 4);
  TrackFile(fixture, 8, Waveform(3), 0xffffffff);
  TrackFile(fixture, 65535, Waveform(19), 5);
  if (scene == 6)
    TrackFile(fixture, 0, Waveform(7), 2);
}

uint64_t Digest(std::span<const float> samples) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (float sample : samples) {
    Check(std::isfinite(sample) && sample >= -1 && sample <= 1,
          "The mixed stream contains finite, clipped stereo samples");
    const uint16_t value = uint16_t(std::lrint(double(sample) * 32768));
    for (unsigned byte = 0; byte < 2; ++byte) {
      hash ^= uint8_t(value >> (byte * 8));
      hash *= 0x100000001b3ull;
    }
  }
  return hash;
}

void ClockedPrograms() {
  // Cold and warm PCM signatures include stream priming and the complete tail.
  constexpr std::array<std::array<std::array<uint64_t, 2>, 8>, 2> expected{
      {{{{0x4e3ef690b7abd1d5ull, 0x4e3ef690b7abd1d5ull},
         {0xed86cdecf0b03fbdull, 0xed86cdecf0b03fbdull},
         {0xde635dd03ebf9c56ull, 0xde635dd03ebf9c56ull},
         {0x36f6be7442a981d3ull, 0x36f6be7442a981d3ull},
         {0xa2ec8fe52ab5d223ull, 0xa2ec8fe52ab5d223ull},
         {0xb3ee972dccfe248cull, 0xb3ee972dccfe248cull},
         {0x0f71b37a16736cc4ull, 0x0f71b37a16736cc4ull},
         {0xeafa5768144e47cfull, 0xeafa5768144e47cfull}}},
       {{{0x0fc64e1d41598775ull, 0x0fc64e1d41598775ull},
         {0xc587c1ec1c5eef74ull, 0xc587c1ec1c5eef74ull},
         {0x8964ccf40ddf34eaull, 0x8964ccf40ddf34eaull},
         {0x036b612ea35ec0a1ull, 0x036b612ea35ec0a1ull},
         {0xdcf9869be6406a6aull, 0xdcf9869be6406a6aull},
         {0x47209800efaf218cull, 0x47209800efaf218cull},
         {0xea20e3dd37c94f12ull, 0xea20e3dd37c94f12ull},
         {0x14139294d95950bfull, 0x14139294d95950bfull}}}}};
  const std::array<std::vector<uint8_t>, 8> statuses{
      {{10, 2},
       {10, 2, 2, 2, 2, 2},
       {10, 2, 2, 2, 2, 2},
       {10, 2, 2, 2},
       {10, 2, 2, 50, 2, 50, 2, 50, 2, 50, 2, 50, 2, 50, 2, 50},
       {10, 2, 2, 2, 2},
       {2, 50},
       {0, 0}}};
  for (unsigned pal = 0; pal < 2; ++pal) {
    for (unsigned scene = 0; scene < 8; ++scene) {
      Fixture fixture;
      SceneAssets(fixture, scene);
      const auto path = fixture.Write("game.sfc", msu_test::Rom(scene, pal != 0));
      auto machine = Load(path);
      Audio audio;
      machine->AttachAudioOutput(&audio);
      size_t hiddenPrefix = 0;
      std::vector<float> cold;
      for (unsigned warm = 0; warm < 2; ++warm) {
        audio.samples.clear();
        if (warm)
          Check(machine->Reset(), "A mounted MSU pack survives console reset");
        for (unsigned frame = 0; frame < 16; ++frame) {
          machine->StepFrame();
          if (!warm && frame == 7)
            hiddenPrefix = audio.samples.size();
        }
        Check(audio.samples.size() == (pal ? 10073u : 8455u) * 2,
              "The independent input clocks deliver the complete regional stream");
        if (Digest(audio.samples) != expected[pal][scene][warm])
          throw std::runtime_error("MSU PCM signature differs for scene " + std::to_string(scene) +
                                   ", region " + std::to_string(pal) + ", reset " +
                                   std::to_string(warm));
        for (unsigned i = 0; i < statuses[scene].size(); ++i)
          Check(machine->GetBus().Read(0x7e0100 + i) == statuses[scene][i],
                "Program and DMA status bytes retain their expected values");
        Check(machine->GetBus().Read(0x7e01ff) == 0xa5,
              "The console program completes sound upload, controls and DMA");
        if (!warm)
          cold = audio.samples;
      }
      // Detaching the host sink must not pause either oscillator or replay its queue.
      auto headless = Load(path);
      for (unsigned frame = 0; frame < 8; ++frame)
        headless->StepFrame();
      Audio resumed;
      headless->AttachAudioOutput(&resumed);
      for (unsigned frame = 0; frame < 8; ++frame)
        headless->StepFrame();
      Check(resumed.samples.size() == cold.size() - hiddenPrefix,
            "Attaching a sink delivers only newly mixed samples");
      for (size_t i = 0; i < resumed.samples.size(); ++i)
        Check(resumed.samples[i] == cold[hiddenPrefix + i],
              "Headless playback preserves interpolation and file position");
    }
  }
}

void ThreeStreamClipping() {
  audio::StreamMixer mixer;
  using Input = audio::StreamMixer::Input;
  std::vector<float> output;
  mixer.SetOutput([&](float left, float right) {
    output.push_back(left);
    output.push_back(right);
  });
  for (const auto input : {Input::Console, Input::Handheld, Input::Expansion})
    mixer.SetRate(input, AudioTiming::SampleRate);
  for (unsigned i = 0; i < 8; ++i) {
    mixer.Write(Input::Console, 0.75, -0.75);
    mixer.Write(Input::Handheld, 0.75, -0.75);
    mixer.Write(Input::Expansion, -0.75, 0.75);
  }
  Check(output.size() == 18 && output[output.size() - 2] == 0.75f && output.back() == -0.75f,
        "All three streams add before clipping, including cancellation after two loud inputs");
}

void AdapterPackFileLoading() {
  Fixture fixture;
  SceneAssets(fixture, 2);
  auto bios = msu_test::Rom(2, false);
  // Enable the handheld before uploading the console sound program.
  const std::array<uint8_t, 5> enable{0xa9, 0x81, 0x8d, 3, 0x60};
  std::copy_backward(bios.begin(), bios.begin() + 0x380, bios.begin() + 0x380 + enable.size());
  std::copy(enable.begin(), enable.end(), bios.begin());
  bios.resize(0x80000);
  bios[0x7fd6] = 0xe3;
  bios[0x7fd7] = 9;
  const auto path = fixture.Write("game.sfc", bios);
  std::array<uint8_t, 256> boot{};
  boot[0] = 0xc3;
  boot[1] = 0xfc;
  boot[0xfc] = 0x3e;
  boot[0xfd] = 1;
  boot[0xfe] = 0xe0;
  boot[0xff] = 0x50;
  const auto bootPath = fixture.Write("boot.bin", boot);
  std::vector<uint8_t> game(32768);
  std::vector<uint8_t> sound;
  for (const auto [reg, value] : std::initializer_list<std::pair<uint8_t, uint8_t>>{{0x26, 0x80},
                                                                                    {0x24, 0x77},
                                                                                    {0x25, 0xff},
                                                                                    {0x11, 0x40},
                                                                                    {0x12, 0xf3},
                                                                                    {0x13, 0x87},
                                                                                    {0x14, 0x83}})
    sound.insert(sound.end(), {0x3e, value, 0xe0, reg});
  sound.insert(sound.end(), {0x18, 0xfe});
  std::copy(sound.begin(), sound.end(), game.begin() + 0x100);
  const auto gamePath = fixture.Write("handheld.gb", game);
  auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Check(machine->LoadSuperGameBoyFromFiles(path.string(), gamePath.string(), bootPath.string()),
        "The adapter file loader accepts a pack beside its console BIOS");
  Check(machine->GetBus().Read(0x2002) == 'S', "The adapter exposes the external audio registers");
  size_t handheldFrames = 0, audibleHandheld = 0;
  const_cast<Cartridge *>(machine->LoadedCartridge())
      ->SetGameBoyAudioOutput([&](int16_t left, int16_t right, uint32_t) {
        ++handheldFrames;
        audibleHandheld += left != 0 || right != 0;
      });
  Audio audio;
  machine->AttachAudioOutput(&audio);
  for (unsigned frame = 0; frame < 16; ++frame)
    machine->StepFrame();
  Check(machine->GetBus().Read(0x7e01ff) == 0xa5 && audio.samples.size() > 16000,
        "The three input clocks deliver audio while both processors execute");
  Check(handheldFrames > 8000 && audibleHandheld > 1000 &&
            std::any_of(audio.samples.begin(), audio.samples.end(),
                        [](float value) { return std::abs(value) > 0.01f; }),
        "The adapter pack produces audible mixed output");
  Check(machine->Reset(), "The adapter and mounted pack reset together");
  audio.samples.clear();
  for (unsigned frame = 0; frame < 16; ++frame)
    machine->StepFrame();
  Check(machine->GetBus().Read(0x7e01ff) == 0xa5 && audio.samples.size() > 16000,
        "All three streams restart after reset without stale queues");
}

} // namespace

int main() {
  try {
    MappingAndLifecycle();
    ClockedPrograms();
    ThreeStreamClipping();
    AdapterPackFileLoading();
    std::puts("MSU-1 console clock, mute, loop, mixing and reset tests passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
