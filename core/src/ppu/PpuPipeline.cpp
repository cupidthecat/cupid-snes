#include "snes/core/PpuBackground.hpp"
#include "snes/core/PpuObject.hpp"
#include "snes/core/PpuPixelMixer.hpp"

#include <algorithm>

namespace snes::core {
void Ppu::SynchronizePipeline(uint16_t clock) {
  if (ScanlineModel()) {
    if (pendingScanline_ && clock > config_.renderCycle) {
      const auto line = pendingScanline_;
      pendingScanline_ = 0;
      CacheScanline(line);
    }
    return;
  }
  const uint16_t end = std::min<uint16_t>(clock, 1368);
  if (currentLine_ > 240) {
    pipelineClock_ = end;
    return;
  }
  // Fine alignment includes writes at line start, before the first fetch edge.
  if (pipelineClock_ == 0 && end > 0)
    background_->BeginLine(currentLine_, fieldId_, io_);
  while (pipelineClock_ < end) {
    const auto h = pipelineClock_;
    pipelineClock_ += 2;
    if ((h <= 1016 && !(h & 7)) || h >= 1080)
      object_->Clock(h, fieldId_, io_, latch_, oam_,
                     PpuObject::Vram{vram_.get(), config_.vramBytes / 2});
    if (h < 1080 &&
        background_->Clock(h, io_, PpuBackground::Vram{vram_.get(), config_.vramBytes / 2}) &&
        currentLine_ > 0)
      StorePipelinePixel((h - 58) / 4);
  }
}

void Ppu::StorePipelinePixel(unsigned x) {
  const bool hires = io_.pseudoHires || io_.bgMode == 5 || io_.bgMode == 6;
  const bool retained =
      currentLine_ > 0 && currentLine_ < MaxVisibleLines && currentLine_ < VDisp();
  if (retained)
    lines_[currentLine_].sampledHires |= hires;
  uint16_t sub = 0, main = 0;
  if (!io_.displayDisable && (io_.overscan || currentLine_ < 225)) {
    const auto pixel = MixPpuPixel(io_, cgram_, *background_, *object_, x);
    // The main-screen lookup follows the sub-screen lookup on the palette bus.
    if (pixel.paletteAddress >= 0)
      latch_.cgramAddress = uint8_t(pixel.paletteAddress);
    if (retained) {
      auto &line = lines_[currentLine_];
      line.sampledAbove[x] = pixel.above;
      line.sampledBelow[x] = pixel.below;
    }
    sub = hires ? CompositeHiresSubPixel(io_, pixel.below, pipelineMath_) : 0;
    main = CompositePixel(io_, pixel.above, pixel.below,
                          PpuColorWindow(io_, io_.col.window.aboveMask, x),
                          PpuColorWindow(io_, io_.col.window.belowMask, x), pipelineMath_);
  }
  // Each lookup precedes its stores: the first dot can change the second lookup.
  const uint16_t first = ProgressiveColor(hires ? sub : main);
  StoreDisplayFeedback(x * 2, first);
  const uint16_t second = ProgressiveColor(main);
  StoreDisplayFeedback(x * 2 + 1, second);
}

uint16_t Ppu::ProgressiveColor(uint16_t color) const {
  if (io_.displayBrightness == 0 && color < displayFeedback_->size())
    return (*displayFeedback_)[color];
  return lightTable_[io_.displayBrightness][color];
}

void Ppu::StoreDisplayFeedback(unsigned x, uint16_t color) {
  const unsigned row = currentLine_ + (feedbackOverscan_ ? 0 : 7);
  unsigned index = row * 1024 + x;
  if (displayInterlace_ && fieldId_)
    index += 512;
  const auto store = [&](unsigned position) {
    constexpr unsigned words = OutputWidth * OutputHeight;
    if (position < words) {
      progressiveOutput_[position] = uint16_t(color | (io_.displayDisable ? 0x8000 : 0));
    } else if (position - words < displayFeedback_->size()) {
      (*displayFeedback_)[position - words] = color;
    }
  };
  if (!displayInterlace_)
    store(index + 512);
  store(index);
}

} // namespace snes::core
