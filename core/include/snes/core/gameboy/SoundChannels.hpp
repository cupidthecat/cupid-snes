// Clocked monochrome sound channels and their register interface.
#pragma once

#include <array>
#include <cstdint>

namespace snes::core::gameboy {

class SoundChannels {
public:
  struct Signal {
    uint8_t level = 0;
    bool dacEnabled = false, channelEnabled = false;
  };
  struct Output {
    virtual ~Output() = default;
    // Offsets select a filter phase in CPU clocks; delivery clocks stay ordered.
    virtual void Dac(uint64_t clock, unsigned channel, Signal signal, int phaseOffset) = 0;
    virtual void Mixer(uint64_t clock, uint8_t volume, uint8_t routing) = 0;
    virtual void Power(uint64_t clock, bool enabled) = 0;
  };
  struct State {
    std::array<uint8_t, 4> amplitudes{}, volumes{};
    std::array<uint16_t, 4> frequencies{};
  };

  explicit SoundChannels(Output &output) : output_(output) {}
  void Reset(bool secondModel = false, bool notifyOutput = true);
  uint8_t Read(uint16_t address) const;
  void Write(uint16_t address, uint8_t value);
  void Advance(unsigned clocks);
  void AdvanceTo(uint64_t clock);
  void DividerEdge(bool rising, uint64_t clock);
  void Stop(bool stopped) noexcept {
    stopped_ = stopped;
  }
  // Move wall-clock deadlines while retaining unprocessed oscillator clocks.
  void Suspend(unsigned clocks) noexcept;
  // Wave fetches and sweep pipelines need service before the next sample.
  bool NeedsStep() const noexcept;
  // Delayed triggers split a sound run at their hardware clock.
  uint64_t PendingTriggerClock() const noexcept {
    return noisePending_ ? noiseStart_ : 0;
  }
  State GetState() const noexcept;

private:
  struct Channel {
    int timer = 0;
    unsigned length = 0;
    uint16_t frequency = 0;
    uint8_t position = 0, volume = 0, envelope = 0, amplitude = 0;
    bool enabled = false, envelopeRunning = false, high = false, squareStarted = false;
    bool envelopeClock = false, envelopeLimit = false, squareReloaded = false;
  };

  Output &output_;
  std::array<uint8_t, 23> registers_{};
  std::array<uint8_t, 16> wave_{};
  std::array<Channel, 4> channels_{};
  std::array<uint8_t, 4> emitted_{};
  uint64_t clocks_ = 0, audioClock_ = 0, noiseStart_ = 0, waveRead_ = 0, sweepStop_ = 0,
           dividerEvent_ = 0, secondaryEvent_ = 0, sweepTrigger_ = 0;
  uint16_t noise_ = 0x7fff, noiseCounter_ = 0, sweepShadow_ = 0;
  int noiseCountdown_ = 0, noiseReloadDelta_ = 0;
  uint8_t phase_ = 0, waveShift_ = 0, waveLatch_ = 0, emittedDacs_ = 0, emittedChannels_ = 0,
          sweepCounter_ = 0, noiseAlignment_ = 0, startupEdges_ = 0;
  bool powered_ = false, dividerHigh_ = false, sweepEnabled_ = false;
  bool sweepNegated_ = false, noisePending_ = false, waveAccess_ = false;
  bool secondModel_ = false;
  bool noiseReloaded_ = false;
  bool sweepStarting_ = false;
  bool stopped_ = false;

  void Power(bool enabled);
  void Tick();
  void Trigger(unsigned channel);
  void SetControl(unsigned channel, uint8_t previous, uint8_t value);
  void ClockFrame(int phaseOffset = 0);
  void Lengths();
  void Envelopes(bool rising);
  void Sweep();
  int SweepResult();
  void Emit(uint64_t clock, int phaseOffset = 0);
  void Disable(unsigned channel, bool clearSignal = true);
  bool DacEnabled(unsigned channel) const noexcept;
  bool LengthEnabled(unsigned channel) const noexcept;
  uint16_t Frequency(unsigned channel) const noexcept;
  void ClockNoise();
  void PulseNoise(bool narrow);
  uint16_t NoiseTaps() const noexcept;
  bool Duty(unsigned channel) const noexcept;
  void UpdateAmplitude(unsigned channel);
};

} // namespace snes::core::gameboy
