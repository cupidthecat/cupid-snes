#include "snes/core/audio/StreamMixer.hpp"

#include <algorithm>
#include <utility>

namespace snes::core::audio {

void StreamMixer::Stream::Reset(uint32_t inputRate) {
  history = {};
  ready.clear();
  cursor = 0;
  next = 0;
  rate = inputRate;
  interval = rate ? double(rate) / AudioTiming::SampleRate : 1;
}

void StreamMixer::Stream::Write(Frame frame) {
  for (auto &sample : frame)
    sample += 1e-25; // Keep interpolation arithmetic out of the denormal range.
  history[cursor] = frame;
  cursor = (cursor + 1) & 3;
  for (; next <= 1; next += interval) {
    const double back = 1 - next;
    const double oldWeight = (1 + 2 * next) * back * back;
    const double newWeight = next * next * (3 - 2 * next);
    const double oldSlope = next * back * back;
    const double newSlope = -next * next * back;
    Frame interpolated{};
    for (unsigned side = 0; side < 2; ++side) {
      const double oldest = history[cursor][side];
      const double older = history[(cursor + 1) & 3][side];
      const double newer = history[(cursor + 2) & 3][side];
      const double newest = history[(cursor + 3) & 3][side];
      interpolated[side] = older * oldWeight + newer * newWeight + (newer - oldest) * oldSlope +
                           (newest - older) * newSlope;
    }
    ready.push_back(interpolated);
  }
  next -= 1;
}

void StreamMixer::SetOutput(Output output) {
  if (!output_ && output)
    Reset();
  output_ = std::move(output);
}

void StreamMixer::Reset() {
  for (auto &stream : streams_)
    stream.Reset(stream.rate);
}

void StreamMixer::SetRate(Input input, uint32_t rate) {
  streams_[unsigned(input)].Reset(rate);
}

void StreamMixer::Write(Input input, double left, double right) {
  auto &stream = streams_[unsigned(input)];
  if (!output_ || !stream.rate)
    return;
  stream.Write({left, right});
  MixReady();
}

void StreamMixer::MixReady() {
  for (;;) {
    bool active = false;
    for (const auto &stream : streams_) {
      if (!stream.rate)
        continue;
      active = true;
      if (stream.ready.empty())
        return;
    }
    if (!active)
      return;
    Frame mixed{};
    for (auto &stream : streams_) {
      if (!stream.rate)
        continue;
      for (unsigned side = 0; side < 2; ++side)
        mixed[side] += stream.ready.front()[side];
      stream.ready.pop_front();
    }
    output_(float(std::clamp(mixed[0], -1.0, 1.0)), float(std::clamp(mixed[1], -1.0, 1.0)));
  }
}

} // namespace snes::core::audio
