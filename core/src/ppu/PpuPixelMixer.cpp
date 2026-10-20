#include "snes/core/PpuPixelMixer.hpp"

#include "snes/core/PpuBackground.hpp"
#include "snes/core/PpuObject.hpp"

namespace snes::core {
namespace {
template <typename Window>
bool Blocked(const Window &layer, const Ppu::WindowPos &window, unsigned x) {
  const bool one = (x >= window.oneLeft && x <= window.oneRight) != layer.oneInvert;
  const bool two = (x >= window.twoLeft && x <= window.twoRight) != layer.twoInvert;
  if (!layer.oneEnable)
    return layer.twoEnable && two;
  if (!layer.twoEnable)
    return one;
  switch (layer.mask) {
  case 0:
    return one || two;
  case 1:
    return one && two;
  case 2:
    return one != two;
  default:
    return one == two;
  }
}

uint16_t DirectColor(uint8_t group, uint8_t color) {
  return uint16_t(((color << 2) & 0x001c) + ((group << 1) & 0x0002) + ((color << 4) & 0x0380) +
                  ((group << 5) & 0x0040) + ((color << 7) & 0x6000) + ((group << 10) & 0x1000));
}
} // namespace

bool PpuColorWindow(const Ppu::IO &io, uint8_t mask, unsigned x) {
  if (mask == 0)
    return true;
  if (mask == 3)
    return false;
  const bool inside = Blocked(io.col.window, io.window, x);
  return mask == 1 ? inside : !inside;
}

PpuMixedPixel MixPpuPixel(const Ppu::IO &io, std::span<const uint16_t, Ppu::CgramColors> palette,
                          const PpuBackground &background, const PpuObject &objects, unsigned x) {
  const bool hires = io.pseudoHires || io.bgMode == 5 || io.bgMode == 6;
  PpuMixedPixel result{{Ppu::COL, 0, palette[0]},
                       {Ppu::COL, 0, hires ? palette[0] : io.col.fixedColor}};
  int aboveRead = -1, belowRead = -1;
  const std::array<const Ppu::Background *, 4> registers{&io.bg1, &io.bg2, &io.bg3, &io.bg4};
  const auto &samples = background.Samples();
  for (unsigned id = 0; id < 4; ++id) {
    if (!samples.above[id].priority && !samples.below[id].priority)
      continue;
    const auto &bg = *registers[id];
    const bool blocked =
        (bg.window.oneEnable || bg.window.twoEnable) && Blocked(bg.window, io.window, x);
    for (bool sub : {false, true}) {
      const auto &pixel = (sub ? samples.below : samples.above)[id];
      auto &winner = sub ? result.below : result.above;
      if (pixel.priority <= winner.priority ||
          (blocked && (sub ? bg.window.belowEnable : bg.window.aboveEnable)))
        continue;
      const bool direct =
          id == 0 && io.col.directColor && (io.bgMode == 3 || io.bgMode == 4 || io.bgMode == 7);
      winner = {uint8_t(id), pixel.priority,
                direct ? DirectColor(pixel.group, pixel.palette) : palette[pixel.palette]};
      if (!direct)
        (sub ? belowRead : aboveRead) = pixel.palette;
    }
  }
  const auto pixel = objects.Pixel(x);
  if (pixel.palette) {
    const auto &obj = io.obj;
    const auto priority = obj.priority[pixel.priority];
    const bool blocked =
        (obj.window.oneEnable || obj.window.twoEnable) && Blocked(obj.window, io.window, x);
    const Ppu::Pixel resolved{pixel.palette < 192 ? Ppu::OBJ1 : Ppu::OBJ2, priority,
                              palette[pixel.palette]};
    if (obj.aboveEnable && priority > result.above.priority &&
        !(blocked && obj.window.aboveEnable)) {
      result.above = resolved;
      aboveRead = pixel.palette;
    }
    if (obj.belowEnable && priority > result.below.priority &&
        !(blocked && obj.window.belowEnable)) {
      result.below = resolved;
      belowRead = pixel.palette;
    }
  }
  if (!result.below.priority)
    belowRead = 0;
  if (!result.above.priority)
    aboveRead = 0;
  result.paletteAddress = aboveRead >= 0 ? aboveRead : belowRead;
  return result;
}

} // namespace snes::core
