#include "snes/core/PpuBackground.hpp"

#include <algorithm>

namespace snes::core {
namespace {
bool Blocked(const Ppu::WindowLayer &layer, const Ppu::WindowPos &window, unsigned x) {
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
} // namespace

void Ppu::SynchronizeBackground(uint16_t clock) {
  const uint16_t end = std::min<uint16_t>(clock, 1080);
  if (currentLine_ > 240) {
    backgroundClock_ = end;
    return;
  }
  while (backgroundClock_ < end) {
    const auto h = backgroundClock_;
    backgroundClock_ += 2;
    if (background_->Clock(h, io_, PpuBackground::Vram{vram_.get(), VramWords}) &&
        currentLine_ > 0 && currentLine_ < VDisp() && currentLine_ < MaxVisibleLines)
      StoreBackgroundPixel((h - 58) / 4);
  }
}

void Ppu::StoreBackgroundPixel(unsigned x) {
  auto &line = lines_[currentLine_];
  const bool hires = io_.pseudoHires || io_.bgMode == 5 || io_.bgMode == 6;
  line.backgroundHires |= hires;
  if (io_.displayDisable)
    return;
  Pixel above{COL, 0, cgram_[0]}, below{COL, 0, hires ? cgram_[0] : io_.col.fixedColor};
  const std::array<const Background *, 4> registers{&io_.bg1, &io_.bg2, &io_.bg3, &io_.bg4};
  const auto &samples = background_->Samples();
  for (unsigned id = 0; id < 4; ++id) {
    if (!samples.above[id].priority && !samples.below[id].priority)
      continue;
    const auto &bg = *registers[id];
    const bool blocked =
        (bg.window.oneEnable || bg.window.twoEnable) && Blocked(bg.window, io_.window, x);
    for (bool sub : {false, true}) {
      const auto &pixel = (sub ? samples.below : samples.above)[id];
      auto &winner = sub ? below : above;
      if (pixel.priority <= winner.priority ||
          (blocked && (sub ? bg.window.belowEnable : bg.window.aboveEnable)))
        continue;
      const bool direct =
          id == 0 && io_.col.directColor && (io_.bgMode == 3 || io_.bgMode == 4 || io_.bgMode == 7);
      winner = {uint8_t(id), pixel.priority,
                direct ? DirectColor(pixel.group, pixel.palette) : cgram_[pixel.palette]};
    }
  }
  line.backgroundAbove[x] = above;
  line.backgroundBelow[x] = below;
}

} // namespace snes::core
