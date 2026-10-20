#pragma once

#include "snes/core/Ppu.hpp"

namespace snes::core {

class PpuBackground;
class PpuObject;

struct PpuMixedPixel {
  Ppu::Pixel above, below;
  int paletteAddress = -1;
};

// Resolve layer priority, windows, palette lookup and direct color for one dot.
PpuMixedPixel MixPpuPixel(const Ppu::IO &io, std::span<const uint16_t, Ppu::CgramColors> palette,
                          const PpuBackground &background, const PpuObject &objects, unsigned x);

} // namespace snes::core
