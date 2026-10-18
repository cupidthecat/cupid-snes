#include "snes/core/gameboy/AudioMixer.hpp"

#include "snes/core/AudioTiming.hpp"

#include <algorithm>
#include <utility>

namespace snes::core::gameboy {

void AudioMixer::Stream::Reset(uint32_t inputRate) {
  history = {};
  ready.clear();
  cursor = 0;
  next = 0;
  interval = double(inputRate) / AudioTiming::SampleRate;
}

void AudioMixer::Stream::Write(Frame frame) {
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

void AudioMixer::SetOutput(Output output) {
  if (!output_ && output && handheldRate_)
    Reset(handheldRate_);
  output_ = std::move(output);
}

void AudioMixer::Reset(uint32_t handheldRate) {
  handheldRate_ = handheldRate;
  handheld_.Reset(handheldRate);
  console_.Reset(AudioTiming::SampleRate);
}

void AudioMixer::SetHandheldRate(uint32_t rate) {
  handheldRate_ = rate;
  handheld_.Reset(rate);
}

void AudioMixer::WriteHandheld(int16_t left, int16_t right) {
  if (!output_ || !handheldRate_)
    return;
  handheld_.Write({double(left) / 32768, double(right) / 32768});
  MixReady();
}

void AudioMixer::WriteConsole(int16_t left, int16_t right) {
  if (!output_ || !handheldRate_)
    return;
  console_.Write({double(left) / 32768, double(right) / 32768});
  MixReady();
}

void AudioMixer::MixReady() {
  while (!handheld_.ready.empty() && !console_.ready.empty()) {
    const auto handheld = handheld_.ready.front();
    const auto console = console_.ready.front();
    handheld_.ready.pop_front();
    console_.ready.pop_front();
    output_(float(std::clamp(handheld[0] + console[0], -1.0, 1.0)),
            float(std::clamp(handheld[1] + console[1], -1.0, 1.0)));
  }
}

} // namespace snes::core::gameboy
