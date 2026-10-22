#pragma once

#include "snes/core/Ppu.hpp"

namespace snes::core {

struct PpuScanlineStatus {
  bool rangeOver = false;
  bool timeOver = false;
};

// Render the retained register and palette state against stable graphics memory.
PpuScanlineStatus SamplePpuScanline(Ppu::Line &line, std::span<const uint16_t> vram,
                                    std::span<const uint8_t, Ppu::OamSize> oam);

} // namespace snes::core
