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

void DelayedNoiseSampleBoundaries() {
  constexpr std::array<std::array<uint64_t, 16>, 2> expected{
      {{0xc7c78768e6c12325ull, 0xcfbd06f3a00b2ee9ull, 0xa58e454cbb4f2759ull, 0x09bdb21e61f56a21ull,
        0x3f87a4ab58a88191ull, 0x513be52d4585341dull, 0x96af13ded2c8dce9ull, 0xb3f638095e5640c1ull,
        0x5be19c3f3ba1d6e5ull, 0x6cf5323538d9d549ull, 0xf020d32de341e31dull, 0x76104600d9c4eeadull,
        0x1ce713158fbde1edull, 0x4aeb184c600ef7fdull, 0x10563dc95282e779ull, 0x3abd7d591831ddc1ull},
       {0x41f58baa118ed879ull, 0xdad1258444e06fd5ull, 0xada8bb4d70b04715ull, 0x8f83787aa120c5e9ull,
        0x1c4faa7e9a721ebdull, 0xdac4b4c58ea1edf1ull, 0x572f418ddffc7319ull, 0x519ba0665fbc65e5ull,
        0x9e55908023aa0279ull, 0x37312a5a56fb99d5ull, 0x0a08c02382cb7115ull, 0xebe37d50b33befe9ull,
        0x78afaf54ac8d48bdull, 0x3724b99ba0bd17f1ull, 0xb38f4663f2179d19ull,
        0xadfba53c71d78fe5ull}}};
  for (unsigned model = 0; model < 2; ++model) {
    unsigned fixture = 0;
    for (unsigned before : {0u, 4096u})
      for (unsigned trigger : {65520u, 65524u, 65528u, 65532u})
        for (uint8_t polynomial : {uint8_t{0}, uint8_t{0x3f}}) {
          System system(model != 0);
          system.divider.Write(0xff04, 0);
          system.Step(before);
          system.audio.Write(0xff26, 0x80);
          system.audio.Write(0xff24, 0x77);
          system.audio.Write(0xff25, 0x88);
          system.audio.Write(0xff21, 0x61);
          system.audio.Write(0xff22, polynomial);
          system.audio.Write(0xff23, 0x80);
          system.Step(trigger);
          system.audio.Write(0xff23, 0x80);
          system.Step(32768);
          Check(system.samples.size() == (before ? 799u : 767u) &&
                    system.Digest() == expected[model][fixture++],
                "Delayed noise triggers retain sample boundaries near divider edges");
        }
  }
}

void NoiseActivationAtSampleBoundary() {
  constexpr std::array<std::array<uint64_t, 8>, 2> expected{
      {{0x0b3a45a4f23995ddull, 0xb3b9d93edcb98d3dull, 0x0b3a45a4f23995ddull, 0xb6c5f6c70ad59c29ull,
        0xa0bf19515d7a33d1ull, 0xc3d45afd8e82f7f5ull, 0xff4d47073d831131ull, 0xb66dd5f092b5fc9dull},
       {0xcaaf7b24306b7091ull, 0x08f18a2b10ff0b25ull, 0xcaaf7b24306b7091ull, 0x8b0d13c8d0847de9ull,
        0x8d016f5f333914a5ull, 0x9715f8ea559ee395ull, 0x36612af7deab92fdull,
        0xfb6dec34718e5d19ull}}};
  for (unsigned model = 0; model < 2; ++model)
    for (unsigned phase = 0; phase < 8; ++phase) {
      System system(model != 0);
      system.audio.Write(0xff26, 0);
      system.divider.Write(0xff04, 0);
      system.audio.Write(0xff26, 0x80);
      system.audio.Write(0xff24, 0x77);
      system.audio.Write(0xff25, 0x88);
      system.audio.Write(0xff21, 0x61);
      system.audio.Write(0xff22, 0x22);
      system.Step(996 + phase * 4);
      system.audio.Write(0xff23, 0x80);
      system.Step(16384);
      Check(system.samples.size() == (model == 1 && phase == 7 ? 136u : 135u) &&
                system.Digest() == expected[model][phase],
            "A due sample precedes delayed noise activation at a split sound boundary");
    }
}

void DividerClocksRemainInBatchBudget() {
  constexpr std::array<std::array<uint64_t, 4>, 2> expected{
      {{0x4f342472524f7919ull, 0x629b1c3da5e10e9dull, 0x7b9f7bc2a27723b5ull, 0x953d29e9d445c10dull},
       {0xe52fc243ac09ea39ull, 0xad020738f7e27f49ull, 0xe52fc243ac09ea39ull,
        0xad020738f7e27f49ull}}};
  for (unsigned model = 0; model < 2; ++model)
    for (unsigned phase = 0; phase < 2; ++phase)
      for (unsigned active = 0; active < 2; ++active) {
        System system(model != 0);
        system.audio.Write(0xff26, 0);
        system.divider.Write(0xff04, 0);
        for (const auto [address, value] : std::array<std::array<uint16_t, 2>, 6>{{{0xff26, 0x80},
                                                                                   {0xff24, 0x77},
                                                                                   {0xff25, 0x88},
                                                                                   {0xff21, 0x61},
                                                                                   {0xff22, 0x22},
                                                                                   {0xff23, 0x80}}})
          system.audio.Write(address, uint8_t(value));
        system.StepBatched(2048 + 4 * ((model ? 3 : 4) + phase));
        system.audio.Reset(model != 0, true);
        system.divider.Reset();
        system.clock = 0;
        system.samples.clear();
        system.audio.Write(0xff26, 0);
        for (const auto [address, value] : std::array<std::array<uint16_t, 2>, 5>{
                 {{0xff26, 0x80}, {0xff24, 0x77}, {0xff25, 0x88}, {0xff21, 0x61}, {0xff22, 0x22}}})
          system.audio.Write(address, uint8_t(value));
        if (active)
          system.audio.Write(0xff23, 0x80);
        system.StepBatched(131180);
        system.audio.Write(0xff21, 0x71);
        system.audio.Write(0xff22, 0x22);
        system.audio.Write(0xff23, 0x80);
        system.StepBatched(16384);
        Check(system.samples.size() == 1152 &&
                  system.Digest() == expected[model][phase * 2 + active],
              "Divider-driven clocks remain in the sound batch budget across processor resets");
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
    DelayedNoiseSampleBoundaries();
    NoiseActivationAtSampleBoundary();
    DividerClocksRemainInBatchBudget();
    std::puts("Handheld stereo rendering, filtering and clock checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
