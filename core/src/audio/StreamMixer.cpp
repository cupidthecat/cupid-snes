#include "snes/core/audio/StreamMixer.hpp"

#include <algorithm>
#include <utility>

namespace snes::core::audio {

void StreamMixer::Stream::Reset(uint32_t inputRate) {
  history = {};
  ready.assign(inputRate ? kQueueFrames : 0, {});
  readIndex = writeIndex = 0;
  queued = 0;
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
    Frame interpolated{};
    for (unsigned side = 0; side < 2; ++side) {
      const double oldest = history[cursor][side];
      const double older = history[(cursor + 1) & 3][side];
      const double newer = history[(cursor + 2) & 3][side];
      const double newest = history[(cursor + 3) & 3][side];
      const double linear = newer - oldest;
      double cubic = newest - newer;
      cubic -= oldest;
      cubic += older;
      const double quadratic = oldest - older - cubic;
      // Keep the polynomial evaluation order stable at silence transitions.
      double value = cubic * next * next * next;
      value += quadratic * next * next;
      value += linear * next;
      value += older;
      interpolated[side] = value;
    }
    // A producer can wrap its twenty-millisecond buffer before other clocks catch up.
    ready[writeIndex] = interpolated;
    writeIndex = (writeIndex + 1) % kQueueFrames;
    ++queued;
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
      if (!stream.queued)
        return;
    }
    if (!active)
      return;
    Frame mixed{};
    for (auto &stream : streams_) {
      if (!stream.rate)
        continue;
      for (unsigned side = 0; side < 2; ++side)
        mixed[side] += stream.ready[stream.readIndex][side];
      stream.readIndex = (stream.readIndex + 1) % Stream::kQueueFrames;
      --stream.queued;
    }
    output_(float(std::clamp(mixed[0], -1.0, 1.0)), float(std::clamp(mixed[1], -1.0, 1.0)));
  }
}

} // namespace snes::core::audio
