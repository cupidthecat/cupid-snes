#pragma once

#include "snes/core/Emulator.hpp"

namespace snes::tests {

inline core::Emulator::Config DeterministicConsoleConfig() {
  core::Emulator::Config config;
  config.entropyMode = core::EntropyMode::None;
  config.entropySeed = 0;
  config.ppu.model = core::Ppu::RenderModel::Progressive;
  return config;
}

} // namespace snes::tests
