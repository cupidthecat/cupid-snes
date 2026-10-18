#include "snes/core/gameboy/ClockedIo.hpp"
#include "snes/core/gameboy/SoundChannels.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

using snes::core::gameboy::ClockedIo;
using snes::core::gameboy::SoundChannels;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct System final : SoundChannels::Output, ClockedIo::Bus {
  struct Edge {
    uint64_t clock;
    unsigned channel;
    uint8_t level;
    bool dacEnabled, channelEnabled;
    int phaseOffset;
    bool operator==(const Edge &) const = default;
  };
  SoundChannels sound{*this};
  ClockedIo divider{*this};
  std::vector<Edge> edges;
  uint64_t clock = 0;
  uint8_t volume = 0, routing = 0;
  explicit System(bool secondModel = false) {
    sound.Reset(secondModel);
    divider.Reset();
  }
  void Dac(uint64_t time, unsigned channel, SoundChannels::Signal signal,
           int phaseOffset) override {
    Check(edges.empty() || time >= edges.back().clock, "DAC events retain clock order");
    edges.push_back(
        {time, channel, signal.level, signal.dacEnabled, signal.channelEnabled, phaseOffset});
  }
  void Mixer(uint64_t, uint8_t value, uint8_t outputs) override {
    volume = value;
    routing = outputs;
  }
  void Power(uint64_t, bool) override {
    volume = routing = 0;
  }
  void RequestInterrupt(uint8_t) override {}
  void AudioDividerEdge(bool rising) override {
    sound.DividerEdge(rising, clock);
  }
  void Step(unsigned clocks) {
    while (clocks--) {
      ++clock;
      divider.Advance(1);
      sound.AdvanceTo(clock);
    }
  }
  void Noise(uint8_t polynomial, bool trigger = true) {
    sound.Write(0xff26, 0x80);
    sound.Write(0xff21, 0x72);
    sound.Write(0xff22, polynomial);
    if (trigger)
      sound.Write(0xff23, 0x80);
  }
  void Wave(uint16_t frequency) {
    sound.Write(0xff26, 0x80);
    for (unsigned i = 0; i < 16; ++i)
      sound.Write(uint16_t(0xff30 + i), uint8_t(i * 29 + 0x35));
    sound.Write(0xff1a, 0x80);
    sound.Write(0xff1c, 0x20);
    sound.Write(0xff1d, uint8_t(frequency));
    sound.Write(0xff1e, uint8_t(0x80 | (frequency >> 8)));
  }
};

void PowerAndRegisters() {
  System system;
  Check(system.sound.Read(0xff26) == 0x70 && system.sound.Read(0xff10) == 0x80 &&
            system.sound.Read(0xff15) == 0xff,
        "Powered-off sound exposes the register read masks");
  system.sound.Write(0xff30, 0xa5);
  system.sound.Write(0xff12, 0xf0);
  system.sound.Write(0xff11, 0x3f);
  system.sound.Write(0xff26, 0x80);
  Check(system.sound.Read(0xff12) == 0 && system.sound.Read(0xff30) == 0xa5,
        "Power-on preserves wave RAM and ignores earlier envelope writes");
  system.sound.Write(0xff12, 0xf0);
  system.sound.Write(0xff14, 0xc0);
  Check(system.edges.back().dacEnabled && system.edges.back().channelEnabled &&
            system.edges.back().level == 0,
        "A muted trigger still emits its channel activation");
  system.Step(8183);
  Check(system.sound.Read(0xff26) == 0xf1, "A length loaded while off survives power-on");
  system.Step(1);
  Check(system.sound.Read(0xff26) == 0xf0, "The first divider edge expires a one-step length");
  system.sound.Write(0xff24, 0x76);
  system.sound.Write(0xff25, 0xa5);
  Check(system.volume == 0x76 && system.routing == 0xa5, "Mixer writes reach the output interface");
  system.sound.Write(0xff26, 0);
  Check(system.sound.Read(0xff26) == 0x70 && system.volume == 0 && system.routing == 0 &&
            system.sound.Read(0xff30) == 0xa5,
        "Power-off clears channels and routing while retaining wave RAM");
}

void SquarePhasesAndEnvelopeWrites() {
  System system;
  system.sound.Write(0xff26, 0x80);
  system.sound.Write(0xff12, 0xf0);
  system.sound.Write(0xff13, 0xff);
  system.sound.Write(0xff14, 0x87);
  system.Step(35);
  Check(system.sound.GetState().amplitudes[0] == 0, "The first square duty cycle starts muted");
  system.Step(1);
  Check(system.sound.GetState().amplitudes[0] == 15,
        "Duty zero raises the final phase of its cycle");
  system.Step(4);
  Check(system.sound.GetState().amplitudes[0] == 0, "The square period reloads on its next phase");

  System live;
  live.sound.Write(0xff26, 0x80);
  live.sound.Write(0xff12, 0xb2);
  live.sound.Write(0xff14, 0x80);
  live.sound.Write(0xff12, 0xff);
  Check(live.sound.GetState().volumes[0] == 3,
        "Changing a running envelope direction transforms the current volume");
  live.sound.Write(0xff12, 0);
  Check(live.sound.Read(0xff26) == 0xf0 && live.sound.GetState().volumes[0] == 3,
        "Disabling a DAC stops its channel without resetting the envelope volume");
}

void DividerLengthAndEnvelope() {
  System system;
  system.sound.Write(0xff26, 0x80);
  system.sound.Write(0xff17, 0x81);
  system.sound.Write(0xff19, 0x80);
  system.Step(65527);
  Check(system.sound.GetState().volumes[1] == 8, "The envelope waits for sequencer step seven");
  system.Step(1);
  Check(system.sound.GetState().volumes[1] == 7,
        "Divider edges clock a period-one envelope at 64 Hz");
  system.sound.Write(0xff16, 0x3f);
  system.sound.Write(0xff19, 0x40);
  system.Step(8192);
  Check((system.sound.Read(0xff26) & 2) == 0, "The next length step silences a one-step channel");
}

void SweepOverflowAndLiveWrites() {
  System system;
  system.sound.Write(0xff26, 0x80);
  system.sound.Write(0xff10, 0x11);
  system.sound.Write(0xff12, 0xf3);
  system.sound.Write(0xff13, 0xa7);
  system.sound.Write(0xff14, 0x85);
  system.Step(15);
  Check(system.sound.Read(0xff26) == 0xf1, "Sweep overflow waits for its calculation delay");
  system.Step(1);
  Check(system.sound.Read(0xff26) == 0xf0,
        "Sweep overflow silences the channel after sixteen clocks");
  Check(system.edges.back().clock == 16 && system.edges.back().dacEnabled &&
            !system.edges.back().channelEnabled,
        "Sweep shutdown retains the DAC connection and reports the exact calculation clock");
  system.Step(24568 - 16);
  Check(system.sound.GetState().frequencies[0] == 122,
        "The sweep frequency latch continues calculating after channel shutdown");

  System dormant;
  dormant.sound.Write(0xff26, 0x80);
  dormant.sound.Write(0xff12, 0xf0);
  dormant.sound.Write(0xff13, 0x87);
  dormant.sound.Write(0xff14, 0x85);
  dormant.sound.Write(0xff10, 0x3f);
  Check(dormant.sound.GetState().frequencies[0] == 1,
        "Enabling a dormant sweep exposes the initial calculator latch");

  System unshifted;
  unshifted.sound.Write(0xff26, 0x80);
  unshifted.sound.Write(0xff12, 0xf0);
  unshifted.sound.Write(0xff13, 0x87);
  unshifted.sound.Write(0xff14, 0x85);
  unshifted.Step(4);
  unshifted.sound.Write(0xff10, 0x10);
  unshifted.Step(7);
  Check(unshifted.sound.Read(0xff26) == 0xf1, "An unshifted sweep retains its overflow delay");
  unshifted.Step(1);
  Check(unshifted.sound.Read(0xff26) == 0xf0, "The live unshifted calculation detects overflow");
}

void NoiseClocksAndRetriggers() {
  for (bool secondModel : {false, true}) {
    System system(secondModel);
    system.Noise(0x19);
    system.Step(215);
    Check(system.sound.GetState().amplitudes[3] == 0, "The initial noise sequence starts low");
    system.Step(1);
    Check(system.sound.GetState().amplitudes[3] == 7,
          "The noise prescaler produces its first high sample");

    System retrigger(secondModel);
    retrigger.Noise(0x19);
    retrigger.Step(32);
    retrigger.sound.Write(0xff23, 0x80);
    retrigger.Step(223);
    Check(retrigger.sound.GetState().amplitudes[3] == 0,
          "Retrigger retains the running noise counter");
    retrigger.Step(1);
    Check(retrigger.sound.GetState().amplitudes[3] == 7,
          "The retained counter sets the retrigger phase");

    System inactive(secondModel);
    inactive.Noise(8, false);
    inactive.Step(128);
    inactive.sound.Write(0xff23, 0x80);
    Check(inactive.sound.GetState().amplitudes[3] == 0,
          "An inactive channel clocks its prescaler without advancing its output latch");
    inactive.Step(56);
    Check(inactive.sound.GetState().amplitudes[3] == 7,
          "The inactive prescaler retains its clock phase");

    System live(secondModel);
    live.Noise(0x19);
    live.Step(24568);
    Check(live.sound.GetState().amplitudes[3] == 0, "The selected noise phase has a low output");
    live.sound.Write(0xff22, 0x80);
    Check(live.sound.GetState().amplitudes[3] == 7,
          "A polynomial write can clock coupled counter taps into the output latch");
  }
}

void WaveAccessAndRetrigger() {
  for (bool secondModel : {false, true}) {
    System system(secondModel);
    system.Wave(2047);
    system.Step(8);
    Check(system.sound.GetState().amplitudes[2] == 5 && system.sound.Read(0xff3f) == 0x35,
          "The wave access window redirects CPU reads to the current byte");
    system.Step(3);
    Check(system.sound.Read(0xff30) == 0x35,
          "A wave read window lasts until the next audio update");
    system.sound.Write(0xff3f, 0xa5);
    Check(system.sound.Read(0xff30) == 0xa5 && system.sound.GetState().amplitudes[2] == 5,
          "An active wave write updates RAM while preserving the fetched sample");
    system.sound.Write(0xff1a, 0);
    Check(system.sound.Read(0xff30) == 0xa5 && system.sound.Read(0xff3f) == 0xe8,
          "Disabling the wave DAC restores direct RAM addressing");

    System blocked(secondModel);
    blocked.Wave(2046);
    blocked.Step(12);
    blocked.sound.Write(0xff30, 0xa5);
    Check(blocked.sound.Read(0xff30) == 0xff,
          "CPU wave writes outside the access window are ignored");
    blocked.sound.Write(0xff1a, 0);
    Check(blocked.sound.Read(0xff30) == 0x35, "A blocked wave write preserves physical RAM");

    System retrigger(secondModel);
    retrigger.Wave(2047);
    retrigger.Step(20);
    retrigger.sound.Write(0xff1e, 0x87);
    Check(retrigger.sound.GetState().amplitudes[2] == 10,
          "Retrigger can immediately expose the high nibble of the replacement wave byte");
    retrigger.sound.Write(0xff1a, 0);
    constexpr std::array<uint8_t, 4> expected = {0xa9, 0xc6, 0xe3, 0};
    for (unsigned i = 0; i < expected.size(); ++i)
      Check(retrigger.sound.Read(uint16_t(0xff30 + i)) == expected[i],
            "Retrigger during a fetch copies its aligned four-byte group into wave RAM");
  }
}

void DutyChangesWaitForAnAmplitudeUpdate() {
  for (bool secondModel : {false, true}) {
    for (unsigned channel : {0u, 1u}) {
      const auto base = uint16_t(0xff10 + channel * 5);
      for (bool becomesHigh : {false, true}) {
        for (bool retrigger : {false, true}) {
          System system(secondModel);
          system.sound.Write(0xff26, 0x80);
          system.sound.Write(uint16_t(base + 1), becomesHigh ? 0x80 : 0xc0);
          system.sound.Write(uint16_t(base + 2), 0xc8);
          system.sound.Write(uint16_t(base + 3), 0xff);
          system.sound.Write(uint16_t(base + 4), 0x87);
          system.Step(12);
          const auto before = uint8_t(becomesHigh ? 0 : 12);
          Check(system.sound.GetState().amplitudes[channel] == before,
                "The first oscillator edge selects the original duty level");
          system.sound.Write(uint16_t(base + 1), becomesHigh ? 0xc0 : 0x80);
          Check(system.sound.GetState().amplitudes[channel] == before,
                "A duty write retains the previous output until an amplitude update");
          system.sound.Write(uint16_t(base + (retrigger ? 4 : 2)), retrigger ? 0x87 : 0xc8);
          const auto volume = uint8_t(retrigger ? 12 : 13);
          Check(system.sound.GetState().volumes[channel] == volume &&
                    system.sound.GetState().amplitudes[channel] == (becomesHigh ? volume : 0),
                "Envelope writes and retriggers apply the current duty at the retained phase");
        }
      }
    }
  }
}

void BatchedOutput() {
  System individual, batched;
  individual.Noise(0x19);
  batched.Noise(0x19);
  for (System *system : {&individual, &batched}) {
    system->sound.Write(0xff17, 0x80);
    system->sound.Write(0xff18, 0xff);
    system->sound.Write(0xff19, 0x87);
  }
  individual.Step(2048);
  batched.sound.Advance(2048);
  Check(individual.edges == batched.edges,
        "Batched advancement preserves every DAC event and timestamp");
}

void RetriggerBetweenEnvelopeEdges() {
  for (bool secondModel : {false, true})
    for (unsigned channel : {0u, 1u, 3u}) {
      System system(secondModel);
      system.divider.Write(0xff04, 0);
      system.sound.Write(0xff26, 0x80);
      const uint16_t envelope = uint16_t(0xff12 + channel * 5);
      const uint16_t trigger = uint16_t(0xff14 + channel * 5);
      system.sound.Write(envelope, 0x61);
      system.sound.Write(trigger, 0x80);
      system.Step(57344);
      system.sound.Write(trigger, 0x80);
      system.Step(8192);
      Check(system.sound.GetState().volumes[channel] == 6,
            "Retriggering between envelope edges postpones the next volume step");
    }
}

void NoiseRetriggerOutputLatch() {
  for (bool secondModel : {false, true}) {
    System system(secondModel);
    system.divider.Write(0xff04, 0);
    system.sound.Write(0xff26, 0x80);
    system.sound.Write(0xff21, 0x61);
    system.sound.Write(0xff22, 0);
    system.sound.Write(0xff23, 0x80);
    system.Step(128);
    Check(system.sound.GetState().amplitudes[3] == 6,
          "The selected noise phase has a high output before retriggering");
    system.sound.Write(0xff23, 0x80);
    Check(system.sound.GetState().amplitudes[3] == 6,
          "Retrigger updates the output using the previous noise latch");
    system.sound.Write(0xff21, 0x51);
    Check(system.sound.GetState().amplitudes[3] == 0,
          "A later envelope write uses the cleared noise latch after retriggering");
  }
}

void PowerOnDuringHighDivider() {
  for (bool secondModel : {false, true})
    for (unsigned channel : {0u, 1u, 3u}) {
      System system(secondModel);
      system.divider.Write(0xff04, 0);
      system.Step(4096);
      system.sound.Write(0xff26, 0x80);
      system.sound.Write(uint16_t(0xff12 + channel * 5), 0x61);
      system.sound.Write(uint16_t(0xff14 + channel * 5), 0x80);
      system.Step(65536);
      Check(system.sound.GetState().volumes[channel] == 6,
            "Power-on while DIV is high delays the first envelope step");
      system.Step(4096);
      Check(system.sound.GetState().volumes[channel] == 5,
            "The envelope advances after the skipped startup divider edge");
    }
}

void DelayedNoiseTriggerAcrossDivider() {
  for (bool secondModel : {false, true}) {
    System system(secondModel);
    system.divider.Write(0xff04, 0);
    system.sound.Write(0xff26, 0x80);
    system.sound.Write(0xff21, 0x61);
    system.sound.Write(0xff23, 0x80);
    system.Step(65532);
    system.sound.Write(0xff23, 0x80);
    system.Step(4);
    Check(system.sound.GetState().volumes[3] == 6,
          "A delayed noise trigger clears the pending envelope clock immediately");
    system.Step(8);
    Check(system.sound.GetState().volumes[3] == 6,
          "The delayed trigger restarts noise without an intervening volume step");
  }
}

void SquareFrequencyWriteOnReload() {
  for (bool secondModel : {false, true})
    for (unsigned channel : {0u, 1u})
      for (bool highByte : {false, true}) {
        System system(secondModel);
        const uint16_t base = uint16_t(0xff10 + channel * 5);
        system.sound.Write(0xff26, 0x80);
        system.sound.Write(uint16_t(base + 1), 0xc0);
        system.sound.Write(uint16_t(base + 2), 0xf0);
        system.sound.Write(uint16_t(base + 3), 0xc6);
        system.sound.Write(uint16_t(base + 4), 0x87);
        system.Step(240);
        system.sound.Write(uint16_t(base + (highByte ? 4 : 3)), highByte ? 0 : 0x57);
        system.Step(highByte ? 44400 : 3700);
        Check(system.sound.GetState().amplitudes[channel] == 15,
              "A frequency write on the reload edge replaces the newly loaded square countdown");
        system.Step(highByte ? 7400 : 356);
        Check(system.sound.GetState().amplitudes[channel] == 0,
              "The square duty phase advances after the updated countdown expires");
      }
}
} // namespace

int main() {
  try {
    PowerAndRegisters();
    SquarePhasesAndEnvelopeWrites();
    DividerLengthAndEnvelope();
    SweepOverflowAndLiveWrites();
    NoiseClocksAndRetriggers();
    WaveAccessAndRetrigger();
    DutyChangesWaitForAnAmplitudeUpdate();
    BatchedOutput();
    RetriggerBetweenEnvelopeEdges();
    NoiseRetriggerOutputLatch();
    PowerOnDuringHighDivider();
    DelayedNoiseTriggerAcrossDivider();
    SquareFrequencyWriteOnReload();
    std::puts("Handheld sound register, oscillator and divider checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
