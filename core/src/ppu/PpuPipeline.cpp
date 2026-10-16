#include "snes/core/PpuBackground.hpp"
#include "snes/core/PpuObject.hpp"

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

void Ppu::SynchronizePipeline(uint16_t clock) {
  const uint16_t end = std::min<uint16_t>(clock, 1368);
  if (currentLine_ > 240) {
    pipelineClock_ = end;
    return;
  }
  while (pipelineClock_ < end) {
    const auto h = pipelineClock_;
    pipelineClock_ += 2;
    if ((h <= 1016 && !(h & 7)) || h >= 1080)
      object_->Clock(h, fieldId_, io_, latch_, oam_, PpuObject::Vram{vram_.get(), VramWords});
    if (h < 1080 && background_->Clock(h, io_, PpuBackground::Vram{vram_.get(), VramWords}) &&
        currentLine_ > 0 && (io_.overscan || currentLine_ < 225))
      StorePipelinePixel((h - 58) / 4);
  }
}

void Ppu::StorePipelinePixel(unsigned x) {
  const bool hires = io_.pseudoHires || io_.bgMode == 5 || io_.bgMode == 6;
  const bool retained =
      currentLine_ > 0 && currentLine_ < MaxVisibleLines && currentLine_ < VDisp();
  if (retained)
    lines_[currentLine_].sampledHires |= hires;
  if (io_.displayDisable || currentLine_ == 0 || currentLine_ > 240 ||
      (!io_.overscan && currentLine_ >= 225))
    return;
  Pixel above{COL, 0, cgram_[0]}, below{COL, 0, hires ? cgram_[0] : io_.col.fixedColor};
  int aboveRead = -1, belowRead = -1;
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
      if (!direct)
        (sub ? belowRead : aboveRead) = pixel.palette;
    }
  }
  const auto pixel = object_->Pixel(x);
  if (pixel.palette) {
    const auto &obj = io_.obj;
    const auto priority = obj.priority[pixel.priority];
    const bool blocked =
        (obj.window.oneEnable || obj.window.twoEnable) && Blocked(obj.window, io_.window, x);
    const Pixel resolved{pixel.palette < 192 ? OBJ1 : OBJ2, priority, cgram_[pixel.palette]};
    if (obj.aboveEnable && priority > above.priority && !(blocked && obj.window.aboveEnable)) {
      above = resolved;
      aboveRead = pixel.palette;
    }
    if (obj.belowEnable && priority > below.priority && !(blocked && obj.window.belowEnable)) {
      below = resolved;
      belowRead = pixel.palette;
    }
  }
  if (!below.priority)
    belowRead = 0;
  if (!above.priority)
    aboveRead = 0;
  // The sub-screen lookup precedes the main-screen lookup. Direct color
  // consumes no palette word and leaves the preceding bus address intact.
  if (belowRead >= 0)
    latch_.cgramAddress = uint8_t(belowRead);
  if (aboveRead >= 0)
    latch_.cgramAddress = uint8_t(aboveRead);
  if (retained) {
    auto &line = lines_[currentLine_];
    line.sampledAbove[x] = above;
    line.sampledBelow[x] = below;
  }
}

} // namespace snes::core
