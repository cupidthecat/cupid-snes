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
  const auto pixel = MixPpuPixel(io_, cgram_, *background_, *object_, x);
  // The main-screen lookup follows the sub-screen lookup on the palette bus.
  if (pixel.paletteAddress >= 0)
    latch_.cgramAddress = uint8_t(pixel.paletteAddress);
  if (retained) {
    auto &line = lines_[currentLine_];
    line.sampledAbove[x] = pixel.above;
    line.sampledBelow[x] = pixel.below;
  }
}

} // namespace snes::core
