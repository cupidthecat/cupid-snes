#include "snes/core/gameboy/Audio.hpp"
#include "snes/core/gameboy/ClockedIo.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

using snes::core::gameboy::Audio;
using snes::core::gameboy::ClockedIo;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct System final : Audio::Output, ClockedIo::Bus {
  struct Sample {
    uint64_t clock;
    int16_t left, right;
    bool operator==(const Sample &) const = default;
  };
  std::vector<Sample> samples;
  uint64_t clock = 0;
  Audio audio{*this};
  ClockedIo divider{*this};
  explicit System(bool secondModel = false) {
    Reset(secondModel);
  }
  void Reset(bool secondModel) {
    clock = 0;
    samples.clear();
    audio.Reset(secondModel);
    divider.Reset();
  }
  void Sample(uint64_t time, int16_t left, int16_t right) override {
    Check(samples.empty() || time > samples.back().clock,
          "Stereo samples keep strictly ordered clocks");
    samples.push_back({time, left, right});
  }
  void RequestInterrupt(uint8_t) override {}
  void AudioDividerEdge(bool rising) override {
    audio.DividerEdge(rising, clock);
  }
  void Step(unsigned clocks) {
    while (clocks--) {
      ++clock;
      divider.Advance(1);
      audio.AdvanceTo(clock);
    }
  }
  void StepBatched(unsigned clocks) {
    while (clocks) {
      const auto count = std::min(clocks, 64u);
      for (unsigned i = 0; i < count; ++i) {
        ++clock;
        divider.Advance(1);
      }
      audio.AdvanceTo(clock);
      clocks -= count;
    }
  }
  void Voices() {
    audio.Write(0xff26, 0x80);
    audio.Write(0xff24, 0x77);
    audio.Write(0xff25, 0xff);
    audio.Write(0xff11, 0x40);
    audio.Write(0xff12, 0xb2);
    audio.Write(0xff13, 0x87);
    audio.Write(0xff14, 0x85);
    audio.Write(0xff16, 0xc0);
    audio.Write(0xff17, 0x81);
    audio.Write(0xff18, 0xa2);
    audio.Write(0xff19, 0x86);
    for (unsigned i = 0; i < 16; ++i)
      audio.Write(uint16_t(0xff30 + i), uint8_t(i * 17));
    audio.Write(0xff1a, 0x80);
    audio.Write(0xff1c, 0x20);
    audio.Write(0xff1d, 0x23);
    audio.Write(0xff1e, 0x87);
    audio.Write(0xff21, 0x72);
    audio.Write(0xff22, 0x19);
    audio.Write(0xff23, 0x80);
  }
  uint64_t Digest() const {
    uint64_t hash = 14695981039346656037ull;
    for (const auto &sample : samples)
      for (int16_t value : {sample.left, sample.right})
        for (unsigned shift : {0u, 8u}) {
          hash ^= (uint16_t(value) >> shift) & 255;
          hash *= 1099511628211ull;
        }
    return hash;
  }
};

void NativeRatesAndSilence() {
  for (bool secondModel : {false, true}) {
    System system(secondModel);
    Check(system.audio.SampleRate() == (secondModel ? 32768u : 33558u),
          "Each adapter uses its native integer sample rate");
    system.Step(2097152);
    Check(system.samples.size() == (secondModel ? 16384u : 16383u) &&
              system.samples.front().clock == (secondModel ? 128u : 132u) &&
              system.samples.back().clock == (secondModel ? 2097152u : 2097040u),
          "Sample clocks retain the fractional period of the first adapter");
    for (const auto &sample : system.samples)
      Check(sample.left == 0 && sample.right == 0, "Disconnected DACs produce silence");
  }
}

void FilterAndStereoRouting() {
  System system(true);
  system.audio.Write(0xff26, 0x80);
  system.audio.Write(0xff24, 0x77);
  system.audio.Write(0xff25, 0x10);
  system.audio.Write(0xff12, 0xf0);
  system.Step(128000);
  constexpr std::array<int16_t, 40> expected = {
      0,   -1,   0,   -1,    0,     -1,    0,     0,     -1,    0,     -2,    3,    -5, 6,
      -9,  10,   -14, 15,    -17,   17,    -17,   14,    -10,   2,     7,     -24,  44, -75,
      121, -203, 403, -2305, -5016, -4386, -4683, -4465, -4558, -4468, -4474, -4446};
  for (unsigned i = 0; i < expected.size(); ++i)
    Check(system.samples[i].left == expected[i] && system.samples[i].right == 0,
          "The reconstruction filter retains its impulse shape and independent stereo routes");
  Check(system.samples[63].left == -3904 && system.samples[199].left == -1880 &&
            system.samples[999].left == -26,
        "The high-pass capacitor removes the connected DAC's DC offset over time");
}

void MixedVoices() {
  constexpr std::array<uint64_t, 2> expected = {0x016a57de6a59f989ull, 0x238eed136a6a373dull};
  for (unsigned model = 0; model < 2; ++model) {
    System system(model != 0);
    system.Voices();
    system.Step(131072);
    Check(system.Digest() == expected[model],
          "Both clocks preserve the mixed square, wave, noise and envelope waveform");
  }
}

void DacDischargeAndRetrigger() {
  constexpr std::array<uint64_t, 2> expected = {0x07138d2646f72611ull, 0x00f9e59fb81f1765ull};
  for (unsigned model = 0; model < 2; ++model) {
    System system(model != 0);
    system.Voices();
    system.Step(65528);
    system.audio.Write(0xff12, 0);
    system.Step(132);
    system.audio.Write(0xff12, 0xf0);
    system.Step(12);
    system.audio.Write(0xff14, 0x80);
    system.Step(32768);
    Check(
        system.samples.size() == 769 && system.Digest() == expected[model],
        "DAC discharge uses its quantized charge curve and preserves the disconnected input latch");
  }
}

void PowerCycleAndNoisePhase() {
  constexpr std::array<uint64_t, 2> expected = {0x7c7eb063f5322f51ull, 0x9dec6eb55b1f39f1ull};
  for (unsigned model = 0; model < 2; ++model) {
    System system(model != 0);
    system.Voices();
    system.Step(128);
    system.audio.Write(0xff26, 0);
    system.Step(4);
    system.audio.Write(0xff26, 0x80);
    system.Step(12);
    system.Voices();
    Check(system.audio.Read(0xff26) == 0xf7, "Power cycling restarts the noise prescaler phase");
    system.Step(11);
    Check(system.audio.Read(0xff26) == 0xf7,
          "The restarted noise trigger keeps its activation delay");
    system.Step(1);
    Check(system.audio.Read(0xff26) == 0xff,
          "The delayed noise channel activates on its scheduled tick");
    system.Step(32768 - 12);
    Check(system.Digest() == expected[model],
          "Power-off retains mixer input latches while clearing mixer registers");
  }
}

void WrappedPhaseAndReset() {
  System system(true);
  system.Step(112);
  constexpr std::array<std::array<uint16_t, 2>, 8> writes = {{{0xff26, 0x80},
                                                              {0xff24, 0x77},
                                                              {0xff25, 0x11},
                                                              {0xff10, 0x12},
                                                              {0xff11, 0xc0},
                                                              {0xff12, 0xf0},
                                                              {0xff13, 0xff},
                                                              {0xff14, 0x87}}};
  for (const auto &write : writes)
    system.audio.Write(write[0], uint8_t(write[1]));
  system.Step(8192);
  Check(system.Digest() == 0x5236a58c995f7eadull,
        "A pipelined sweep transition wraps its phase into the bounded future sample buffer");
  system.Reset(true);
  system.Step(8192);
  for (const auto &sample : system.samples)
    Check(sample.left == 0 && sample.right == 0,
          "Reset clears delayed impulses and capacitor history");
}

void BatchedAdvancement() {
  System stepped(true), batched(true);
  stepped.Voices();
  batched.Voices();
  stepped.Step(4096);
  batched.audio.Advance(4096);
  Check(stepped.samples == batched.samples,
        "Batched sound advancement preserves sample values and clocks");
}

void MixerBeforeTrigger() {
  System system(true);
  system.Step(76);
  system.audio.Write(0xff26, 0x80);
  system.Step(8);
  system.audio.Write(0xff11, 0x80);
  system.Step(20);
  system.audio.Write(0xff12, 0xf3);
  system.Step(8);
  system.audio.Write(0xff25, 0xf3);
  system.Step(16);
  system.audio.Write(0xff24, 0x77);
  system.Step(4096);
  Check(system.samples.size() == 33 && system.Digest() == 0x055095df870b4641ull,
        "Mixer writes retain the initial DAC code before a channel is triggered");
  system.Reset(true);
  for (const auto [address, value] : std::array<std::array<uint8_t, 2>, 5>{
           {{0x26, 0x80}, {0x24, 0x77}, {0x25, 0x11}, {0x12, 0xf0}, {0x24, 0x77}}})
    system.audio.Write(uint16_t(0xff00 | address), value);
  system.Step(4096);
  Check(system.samples.size() == 32 && system.Digest() == 0x067ac992ed9c084dull,
        "A disconnected mixer code stays latched until a DAC level update");
}

void ProcessorResetKeepsOutput() {
  constexpr std::array<unsigned, 8> phases{0, 4, 8, 12, 16, 28, 60, 124};
  constexpr std::array<uint64_t, 8> firstModel{
      0x98ddb5c3f74aaa8dull, 0x041944d22a0f3fa1ull, 0x144d2c78bdd61065ull, 0x1eea756bdb5a7ea5ull,
      0xcb1cfe2f0c6d4c59ull, 0xeb1166914925141dull, 0xafe33b23d70a6851ull, 0xafe33b23d70a6851ull};
  for (bool secondModel : {false, true})
    for (unsigned i = 0; i < phases.size(); ++i) {
      System system(secondModel);
      for (const auto [address, value] : std::array<std::array<uint8_t, 2>, 7>{{{0x26, 0x80},
                                                                                {0x24, 0x77},
                                                                                {0x25, 0xff},
                                                                                {0x11, 0x40},
                                                                                {0x12, 0xb2},
                                                                                {0x13, 0x87},
                                                                                {0x14, 0x85}}})
        system.audio.Write(uint16_t(0xff00 | address), value);
      system.StepBatched(2048 + phases[i]);
      system.audio.Reset(secondModel, true);
      system.divider.Reset();
      system.clock = 0;
      system.samples.clear();
      for (const auto [address, value] : std::array<std::array<uint8_t, 2>, 5>{
               {{0x26, 0x80}, {0x24, 0x77}, {0x25, 0x11}, {0x12, 0xf0}, {0x24, 0x77}}})
        system.audio.Write(uint16_t(0xff00 | address), value);
      system.StepBatched(4096);
      Check(system.samples.size() == 32 &&
                system.Digest() == (secondModel ? 0xbd2bf48fc917354dull : firstModel[i]),
            "Processor resets preserve filter history and the pending native sample phase");
    }
}
} // namespace

int main() {
  try {
    NativeRatesAndSilence();
    FilterAndStereoRouting();
    MixedVoices();
    DacDischargeAndRetrigger();
    PowerCycleAndNoisePhase();
    WrappedPhaseAndReset();
    BatchedAdvancement();
    MixerBeforeTrigger();
    ProcessorResetKeepsOutput();
    std::puts("Handheld stereo rendering, filtering and clock checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
