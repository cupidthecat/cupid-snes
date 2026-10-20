#pragma once

#include "snes/core/Emulator.hpp"

namespace snes::tests {

inline core::Emulator::Config DeterministicConsoleConfig() {
  core::Emulator::Config config;
  config.entropyMode = core::EntropyMode::None;
  config.entropySeed = 0;
  config.ppu.model = core::Ppu::RenderModel::Progressive;
  config.dspMode = core::Smp::DspMode::PerCycle;
  return config;
}

} // namespace snes::tests
