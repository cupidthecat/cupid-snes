#include "snes/core/gameboy/Audio.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace snes::core::gameboy {
namespace {
constexpr uint32_t SampleUnit = 1u << 28;
using Impulses = std::array<std::array<int32_t, 64>, 256>;

const Impulses &Coefficients() {
  static const Impulses table = [] {
    std::array<double, 16385> integral{};
    double total = 0;
    for (unsigned i = 0; i < 16384; ++i) {
      const double x = double(i) / 256 - 32;
      const double sinc =
          x == 0 ? 15.0 / 16 : std::sin(std::numbers::pi * 15 / 16 * x) / (std::numbers::pi * x);
      const double window = (7938 + 9240 * std::cos(std::numbers::pi * x / 32) +
                             1430 * std::cos(std::numbers::pi * x / 16)) /
                            18608;
      total += sinc * window;
      integral[i + 1] = total;
    }
    Impulses result{};
    for (unsigned phase = 0; phase < result.size(); ++phase) {
      int sum = 0;
      unsigned peak = 0;
      for (unsigned i = 0; i < 64; ++i) {
        const unsigned first = i * 256 >= phase ? i * 256 - phase : 0;
        const unsigned last = (i + 1) * 256 - phase;
        result[phase][i] = int32_t((integral[last] - integral[first]) / total * 65536);
        sum += result[phase][i];
        if (result[phase][i] > result[phase][peak])
          peak = i;
      }
      result[phase][peak] += 65536 - sum;
    }
    return result;
  }();
  return table;
}
} // namespace

void Audio::Reset(bool secondModel) {
  filters_.fill({});
  capacitor_.fill(0);
  clock_ = renderClock_ = samples_ = phaseClock_ = 0;
  phase_ = resumeClocks_ = renderedClocks_ = 0;
  stopped_ = false;
  sampleCycles_ = 0;
  sampleRate_ = secondModel ? 32768 : 33558;
  clockRate_ = secondModel ? 4194304 : 4295454;
  batchClocks_ = 2 * ((clockRate_ + 2 * sampleRate_ - 1) / (2 * sampleRate_));
  phaseStep_ = uint32_t(std::llround(double(sampleRate_) * 2 * SampleUnit / clockRate_));
  highpass_ = std::pow(0.999958, double(clockRate_) / sampleRate_);
  volume_ = routing_ = 0;
  channels_.Reset(secondModel);
}

uint8_t Audio::Read(uint16_t address) {
  FlushSound(clock_);
  return channels_.Read(address);
}

void Audio::Write(uint16_t address, uint8_t value) {
  FlushSound(clock_);
  channels_.Write(address, value);
}

void Audio::Dac(uint64_t clock, unsigned channel, SoundChannels::Signal signal, int phaseOffset) {
  const auto previous = filters_[channel].signal;
  filters_[channel].signal = signal;
  if (previous.level != signal.level || previous.channelEnabled != signal.channelEnabled)
    Change(clock, channel, signal.level, phaseOffset);
}

void Audio::Mixer(uint64_t clock, uint8_t volume, uint8_t routing) {
  volume_ = volume;
  routing_ = routing;
  for (unsigned channel = 0; channel < filters_.size(); ++channel)
    Change(clock, channel,
           filters_[channel].signal.channelEnabled ? filters_[channel].signal.level : 16, 0);
}

void Audio::Power(uint64_t clock, bool enabled) {
  if (!enabled)
    for (unsigned channel = 0; channel < filters_.size(); ++channel)
      Change(clock, channel, 0, 0);
  // Clearing mixer registers leaves the previous input latched until an update.
  volume_ = routing_ = 0;
}

void Audio::Change(uint64_t clock, unsigned channel, unsigned code, int phaseOffset) {
  auto &filter = filters_[channel];
  AdvancePhase(clock);
  // The phase accumulator wraps within sixteen samples. Keeping its whole-sample
  // part preserves delayed transitions without changing an already emitted sample.
  const uint32_t phase =
      phase_ + (uint32_t(clock / 2 - phaseClock_ / 2) + uint32_t(phaseOffset / 2)) * phaseStep_;
  const unsigned fraction = (phase >> 20) & 255;
  const uint64_t first = samples_ + (phase >> 28) + 1;
  const auto &impulse = Coefficients()[fraction];
  const int level = 15 - 2 * int(code);
  for (unsigned side = 0; side < 2; ++side) {
    const unsigned shift = side ? 0 : 4;
    const bool routed = (routing_ & (1u << (channel + shift))) != 0;
    const int value = routed ? level * (1 + ((volume_ >> shift) & 7)) : 0;
    const int delta = value - filter.input[side];
    filter.input[side] = value;
    if (delta == 0)
      continue;
    for (unsigned i = 0; i < impulse.size(); ++i)
      filter.changes[side][(first + i) & 127] += int64_t(delta) * impulse[i];
  }
}

void Audio::AdvancePhase(uint64_t clock) {
  if (clock <= phaseClock_)
    return;
  if (!stopped_ && resumeClocks_ == 0)
    phase_ += uint32_t(clock / 2 - phaseClock_ / 2) * phaseStep_;
  phaseClock_ = clock;
}

void Audio::FlushSound(uint64_t clock) {
  if (clock > phaseClock_ && !stopped_ && resumeClocks_ == 0)
    renderedClocks_ += unsigned(clock - phaseClock_);
  channels_.AdvanceTo(clock);
  AdvancePhase(clock);
}

void Audio::Stop(bool stopped) noexcept {
  if (stopped_ && !stopped)
    resumeClocks_ = 4;
  stopped_ = stopped;
  channels_.Stop(stopped || resumeClocks_ != 0);
}

void Audio::Render() {
  renderedClocks_ = 0;
  ++samples_;
  std::array<int, 2> mixed{};
  for (auto &filter : filters_) {
    const double change = (filter.signal.dacEnabled ? 1 : -1) * 20000.0 / sampleRate_;
    filter.charge = std::clamp(filter.charge + change, 0.0, 1.0);
    const double curve = filter.charge * filter.charge * (3 - 2 * filter.charge);
    const int gain = int(34 * curve);
    for (unsigned side = 0; side < 2; ++side) {
      auto &delta = filter.changes[side][samples_ & 127];
      filter.value[side] += delta;
      delta = 0;
      mixed[side] += int(std::floor(double(filter.value[side]) * gain / 65536));
    }
  }
  std::array<int16_t, 2> sample{};
  for (unsigned side = 0; side < 2; ++side) {
    sample[side] = int16_t(std::clamp(mixed[side] - int(capacitor_[side]), -32768, 32767));
    capacitor_[side] = mixed[side] - (mixed[side] - capacitor_[side]) * highpass_;
  }
  phase_ = phase_ >= SampleUnit ? phase_ - SampleUnit : 0;
  output_.Sample(renderClock_, sample[0], sample[1]);
}

void Audio::Advance(unsigned clocks) {
  const auto target = clock_ + clocks;
  while (clock_ + 4 < target)
    AdvanceTo(clock_ + 4);
  AdvanceTo(target);
}

void Audio::AdvanceTo(uint64_t clock) {
  clock_ = std::max(clock_, clock);
  uint64_t batchEnd = std::min(clock_ & ~uint64_t(3), renderClock_ + 16);
  while (renderClock_ + 4 <= clock_) {
    renderClock_ += 4;
    if (stopped_ || resumeClocks_) {
      channels_.Suspend(4);
      phaseClock_ += 4;
    }
    if (resumeClocks_) {
      resumeClocks_ -= 4;
      if (resumeClocks_ == 0)
        channels_.Stop(stopped_);
      continue;
    }
    sampleCycles_ += sampleRate_ * 4;
    if (renderClock_ < batchEnd)
      continue;
    batchEnd = std::min(clock_ & ~uint64_t(3), renderClock_ + 16);
    if (channels_.NeedsStep() || renderedClocks_ + renderClock_ - phaseClock_ >= batchClocks_ ||
        sampleCycles_ >= clockRate_)
      FlushSound(renderClock_);
    while (sampleCycles_ >= clockRate_) {
      sampleCycles_ -= clockRate_;
      Render();
    }
  }
}

void Audio::DividerEdge(bool rising, uint64_t clock) {
  if (clock > renderClock_ + 4)
    AdvanceTo((clock - 1) & ~uint64_t(3));
  FlushSound(clock > renderClock_ ? renderClock_ : clock);
  channels_.DividerEdge(rising, clock);
  AdvancePhase(clock);
  AdvanceTo(clock);
}

} // namespace snes::core::gameboy
