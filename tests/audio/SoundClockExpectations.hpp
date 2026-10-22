#pragma once

#include <cstdint>

namespace sound_clock_test {

constexpr uint64_t DspClocks(uint64_t hardwareClocks) {
  return hardwareClocks / 2 + (hardwareClocks & 1);
}

// Phase 27 publishes a stereo frame. The first input into a unity-rate cubic
// stream also publishes its priming frame; subsequent inputs publish one each.
constexpr uint64_t UnityStreamFrames(uint64_t hardwareClocks) {
  const auto frames = (DspClocks(hardwareClocks) + 4) / 32;
  return frames ? frames + 1 : 0;
}

} // namespace sound_clock_test
