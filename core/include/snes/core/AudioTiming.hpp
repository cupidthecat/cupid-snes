#pragma once

#include <cstdint>

namespace snes::core {
struct AudioTiming {
  static constexpr uint32_t SampleRate = 32'040;
  static constexpr unsigned DspClocksPerSample = 32;
  static constexpr uint64_t DspClockRate = uint64_t{SampleRate} * DspClocksPerSample;
  static constexpr uint64_t SmpClockRate = 2 * DspClockRate;
};
} // namespace snes::core
