#pragma once

#include "snes/core/AudioTiming.hpp"
#include "snes/core/PowerOnState.hpp"
#include "snes/core/Ppu.hpp"

namespace snes::core {
struct RomHeader;

namespace detail {
struct CompatibilitySettings {
  bool initializeWram = false;
  bool initializeDsp = false;
  bool perCycleDsp = false;
  uint32_t sampleRate = AudioTiming::SampleRate;
  Ppu::Config video;
  EntropyMode entropy = EntropyMode::Low;
};

bool PalCountry(uint8_t country) noexcept;
CompatibilitySettings SelectCompatibility(const RomHeader &header, Ppu::Config video,
                                          EntropyMode entropy, bool hotfixes);
} // namespace detail
} // namespace snes::core
