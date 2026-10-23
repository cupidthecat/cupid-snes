#include "snes/core/Ppu.hpp"
#include "snes/core/PpuBackground.hpp"
#include "snes/core/PpuObject.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
namespace {
using detail::StateArchive;

template <class Window> void WindowSelection(StateArchive &archive, Window &window) {
  archive.Number(window.oneEnable);
  archive.Number(window.oneInvert);
  archive.Number(window.twoEnable);
  archive.Number(window.twoInvert);
  archive.Bounded(window.mask, uint8_t{0}, uint8_t{3});
}
void WindowLayer(StateArchive &archive, Ppu::WindowLayer &window) {
  WindowSelection(archive, window);
  archive.Number(window.aboveEnable);
  archive.Number(window.belowEnable);
}
void Registers(StateArchive &archive, Ppu::IO &io) {
  archive.Number(io.displayDisable);
  archive.Bounded(io.displayBrightness, uint8_t{0}, uint8_t{15});
  archive.Bounded(io.oamBaseAddress, uint16_t{0}, uint16_t{1023});
  archive.Bounded(io.oamAddress, uint16_t{0}, uint16_t{1023});
  archive.Number(io.oamPriority);
  archive.Bounded(io.bgMode, uint8_t{0}, uint8_t{7});
  archive.Number(io.bgPriority);
  archive.Number(io.vramIncrementMode);
  archive.Bounded(io.vramMapping, uint8_t{0}, uint8_t{3});
  const auto increment = archive.Number(io.vramIncrementSize);
  archive.Validate(increment == 0 || increment == 1 || increment == 32 || increment == 128,
                   "Invalid PPU memory increment");
  archive.Number(io.vramAddress);
  archive.Number(io.cgramAddress);
  archive.Number(io.cgramAddressLatch);
  archive.Number(io.interlace);
  archive.Number(io.overscan);
  archive.Number(io.pseudoHires);
  archive.Number(io.extbg);
  archive.Bounded(io.hcounter, uint16_t{0}, uint16_t{511});
  archive.Bounded(io.vcounter, uint16_t{0}, uint16_t{511});
  archive.Bounded(io.mosaic.size, uint8_t{1}, uint8_t{16});
  archive.Bounded(io.mosaic.counter, uint8_t{0}, uint8_t{16});
  auto &mode = io.mode7;
  archive.Number(mode.hflip);
  archive.Number(mode.vflip);
  archive.Bounded(mode.repeat, uint8_t{0}, uint8_t{3});
  archive.Number(mode.a);
  archive.Number(mode.b);
  archive.Number(mode.c);
  archive.Number(mode.d);
  archive.Number(mode.x);
  archive.Number(mode.y);
  archive.Number(mode.hoffset);
  archive.Number(mode.voffset);
  archive.Number(io.window.oneLeft);
  archive.Number(io.window.oneRight);
  archive.Number(io.window.twoLeft);
  archive.Number(io.window.twoRight);
  for (auto *bg : {&io.bg1, &io.bg2, &io.bg3, &io.bg4}) {
    WindowLayer(archive, bg->window);
    archive.Number(bg->aboveEnable);
    archive.Number(bg->belowEnable);
    archive.Number(bg->mosaicEnable);
    archive.Number(bg->tiledataAddress);
    archive.Number(bg->screenAddress);
    archive.Bounded(bg->screenSize, uint8_t{0}, uint8_t{3});
    archive.Number(bg->tileSize);
    archive.Number(bg->hoffset);
    archive.Number(bg->voffset);
    archive.Enumeration(bg->tileMode, Ppu::TileMode::BPP2, Ppu::TileMode::Inactive);
    for (auto &priority : bg->priority)
      archive.Number(priority);
  }
  auto &obj = io.obj;
  WindowLayer(archive, obj.window);
  archive.Number(obj.aboveEnable);
  archive.Number(obj.belowEnable);
  archive.Number(obj.interlace);
  archive.Bounded(obj.baseSize, uint8_t{0}, uint8_t{7});
  archive.Bounded(obj.nameselect, uint8_t{0}, uint8_t{3});
  archive.Number(obj.tiledataAddress);
  archive.Bounded(obj.first, uint8_t{0}, uint8_t{127});
  archive.Number(obj.rangeOver);
  archive.Number(obj.timeOver);
  for (auto &priority : obj.priority)
    archive.Number(priority);
  auto &col = io.col;
  WindowSelection(archive, col.window);
  archive.Bounded(col.window.aboveMask, uint8_t{0}, uint8_t{3});
  archive.Bounded(col.window.belowMask, uint8_t{0}, uint8_t{3});
  for (auto &enable : col.enable)
    archive.Number(enable);
  archive.Number(col.directColor);
  archive.Number(col.blendMode);
  archive.Number(col.halve);
  archive.Number(col.mathMode);
  archive.Bounded(col.fixedColor, uint16_t{0}, uint16_t{0x7fff});
}
void PixelFields(StateArchive &archive, Ppu::Pixel &pixel) {
  archive.Bounded(pixel.source, uint8_t{0}, uint8_t{Ppu::SourceCount - 1});
  archive.Number(pixel.priority);
  archive.Bounded(pixel.color, uint16_t{0}, uint16_t{0x7fff});
}
template <class Colors> void Palette(StateArchive &archive, Colors &colors) {
  for (auto &color : colors)
    archive.Bounded(color, uint16_t{0}, uint16_t{0x7fff});
}
void BackgroundSample(StateArchive &archive, PpuBackground::Sample &sample) {
  archive.Number(sample.palette);
  archive.Bounded(sample.group, uint8_t{0}, uint8_t{7});
  archive.Number(sample.priority);
}
} // namespace

void PpuBackground::SerializeState(detail::StateArchive &archive) {
  archive.Object(*this, [](auto &fields, auto &next) {
    for (auto &layer : next.layers_) {
      for (auto &column : layer.columns) {
        fields.Number(column.pixels);
        fields.Number(column.address);
        fields.Number(column.palette);
        fields.Bounded(column.group, uint8_t{0}, uint8_t{7});
        fields.Number(column.priority);
        fields.Number(column.flip);
      }
      fields.Bounded(layer.column, 0u, 65u);
      fields.Bounded(layer.position, 0u, 7u);
      fields.Bounded(layer.mosaicClock, 0u, 16u);
      fields.Bounded(layer.mosaicX, 0u, 1024u);
      BackgroundSample(fields, layer.mosaicSample);
    }
    for (auto &sample : next.output_.above)
      BackgroundSample(fields, sample);
    for (auto &sample : next.output_.below)
      BackgroundSample(fields, sample);
    fields.Bounded(next.line_, uint16_t{0}, uint16_t{511});
    fields.Number(next.horizontalOffset_);
    fields.Number(next.verticalOffset_);
    fields.Number(next.previousHorizontalOffset_);
    fields.Number(next.previousVerticalOffset_);
    fields.Number(next.field_);
    fields.Enumeration(next.model_, Ppu::RenderModel::Progressive, Ppu::RenderModel::Scanline);
  });
}

void PpuObject::SerializeState(detail::StateArchive &archive) {
  archive.Object(*this, [](auto &fields, auto &next) {
    for (auto &bank : next.banks_) {
      for (auto &tile : bank.tiles) {
        fields.Number(tile.planes);
        fields.Bounded(tile.x, uint16_t{0}, uint16_t{511});
        fields.Number(tile.palette);
        fields.Bounded(tile.priority, uint8_t{0}, uint8_t{3});
        fields.Number(tile.flip);
      }
      fields.Bounded(bank.count, 0u, 34u);
    }
    for (auto &sample : next.row_) {
      fields.Number(sample.palette);
      fields.Bounded(sample.priority, uint8_t{0}, uint8_t{3});
    }
    for (auto &index : next.selected_)
      fields.Bounded(index, uint8_t{0}, uint8_t{127});
    fields.Number(next.line_);
    fields.Validate(next.line_ <= 511 || next.line_ == 0xffff, "Invalid sprite scanline");
    fields.Bounded(next.writing_, 0u, 1u);
    fields.Bounded(next.first_, 0u, 127u);
    fields.Bounded(next.selectedCount_, 0u, 33u);
    fields.Bounded(next.tileCount_, 0u, 66u);
    fields.Enumeration(next.phase_, Fetch::Idle, Fetch::Done);
    fields.Bounded(next.nextFetch_, 1080u, 2048u);
    fields.Bounded(next.item_, -1, 31);
    fields.Bounded(next.index_, 0u, 127u);
    fields.Bounded(next.column_, 0u, 8u);
    fields.Bounded(next.columns_, 0u, 8u);
    fields.Bounded(next.x_, 0u, 511u);
    fields.Bounded(next.y_, 0u, 255u);
    fields.Number(next.base_);
    fields.Number(next.address_);
    fields.Bounded(next.characterX_, 0u, 15u);
    fields.Validate(next.phase_ != Fetch::HighWord ||
                        (next.tileCount_ > 0 && next.tileCount_ <= 34 &&
                         next.banks_[next.writing_].count == next.tileCount_),
                    "Invalid suspended sprite tile fetch");
  });
}

void Ppu::SerializeState(detail::StateArchive &archive) {
  archive.Expect(config_.revision2);
  archive.Expect(config_.vramBytes);
  archive.Expect(uint32_t(config_.model));
  archive.Expect(config_.deinterlace);
  archive.Expect(config_.renderCycle);
  archive.Expect(isPal_);
  const auto clock = archive.Bounded(pipelineClock_, uint16_t{0}, uint16_t{1368});
  archive.Validate((clock & 1) == 0, "Invalid PPU pipeline clock");
  archive.Object(pipelineMath_, [](auto &fields, auto &next) {
    fields.Bounded(next.mainColor, uint16_t{0}, uint16_t{0x7fff});
    fields.Number(next.mainVisible);
    fields.Number(next.mathEnabled);
    fields.Number(next.useSubscreen);
    fields.Number(next.halve);
  });
  archive.Object(io_, Registers);
  archive.Object(latch_, [](auto &fields, auto &next) {
    fields.Number(next.vram);
    fields.Number(next.oam);
    fields.Number(next.cgram);
    fields.Bounded(next.oamAddress, uint16_t{0}, uint16_t{1023});
    fields.Number(next.cgramAddress);
    fields.Number(next.mode7);
    fields.Number(next.counters);
    fields.Number(next.hcounter);
    fields.Number(next.vcounter);
    fields.Number(next.ppu1.mdr);
    fields.Number(next.ppu1.bgofs);
    fields.Number(next.ppu2.mdr);
    fields.Number(next.ppu2.bgofs);
  });
  archive.Fixed(std::span<uint16_t>(vram_.get(), VramStorageWords));
  archive.Array(oam_);
  archive.Object(cgram_, [](auto &fields, auto &next) { Palette(fields, next); });
  archive.Array(objects_, [](auto &fields, auto &next) {
    fields.Bounded(next.x, uint16_t{0}, uint16_t{511});
    fields.Number(next.y);
    fields.Number(next.character);
    fields.Number(next.nameselect);
    fields.Number(next.vflip);
    fields.Number(next.hflip);
    fields.Bounded(next.priority, uint8_t{0}, uint8_t{3});
    fields.Bounded(next.palette, uint8_t{0}, uint8_t{7});
    fields.Number(next.size);
  });
  const auto start = archive.Bounded(lineStart_, 0, MaxVisibleLines - 1);
  const auto count = archive.Bounded(lineCount_, 0, MaxVisibleLines - 1);
  archive.Validate(count == 0 || (start > 0 && start + count <= MaxVisibleLines),
                   "Invalid PPU scanline cache range");
  archive.Bounded(pendingScanline_, uint16_t{0}, uint16_t{239});
  archive.Number(scanlineHires_);
  for (int index = 0; index < MaxVisibleLines; ++index)
    archive.Object(lines_[index], [=](auto &fields, auto &line) {
      fields.Bounded(line.y, uint16_t{0}, uint16_t{239});
      fields.Validate(count == 0 || index < start || index >= start + count || line.y == index,
                      "Invalid retained PPU scanline");
      fields.Number(line.fieldID);
      Registers(fields, line.io);
      Palette(fields, line.palette);
      for (auto &pixel : line.sampledAbove)
        PixelFields(fields, pixel);
      for (auto &pixel : line.sampledBelow)
        PixelFields(fields, pixel);
      fields.Number(line.sampledHires);
      for (auto &pixel : line.above)
        PixelFields(fields, pixel);
      for (auto &pixel : line.below)
        PixelFields(fields, pixel);
      for (auto &visible : line.windowAbove)
        fields.Number(visible);
      for (auto &visible : line.windowBelow)
        fields.Number(visible);
    });
  for (auto &events : rasterEvents_) {
    uint16_t previous = 0;
    archive.Vector(
        events, detail::StateArchive::MaximumBytes / sizeof(RasterEvent),
        [&](auto &fields, auto &event) {
          fields.Bounded(event.hclock, uint16_t{58}, uint16_t{1078});
          fields.Bounded(event.x, uint16_t{1}, uint16_t{255});
          fields.Validate(event.x >= previous && event.x == (event.hclock - 58 + 3) / 4,
                          "Invalid PPU raster event order");
          previous = event.x;
          fields.Enumeration(event.type, RasterEventType::Display, RasterEventType::Composition);
          fields.Number(event.index);
          fields.Validate(event.type == RasterEventType::Composition
                              ? event.index >= 0x26 && event.index <= 0x32
                          : event.type == RasterEventType::WindowSelect ? event.index <= 2
                                                                        : event.index == 0,
                          "Invalid PPU raster register");
          Registers(fields, event.before);
          Registers(fields, event.after);
        });
  }
  const auto mosaicLine = archive.Number(mosaicCounterLine_);
  archive.Validate(mosaicLine <= 511 || mosaicLine == 0xffff, "Invalid PPU mosaic scanline");
  archive.Fixed(std::span<uint32_t>(output_.get(), OutputWidth * OutputHeight));
  archive.Fixed(std::span<uint16_t>(progressiveOutput_.get(), OutputWidth * OutputHeight));
  // Brightness tables are constant; lower-border display feedback is retained.
  archive.Object(*displayFeedback_, [](auto &fields, auto &next) { Palette(fields, next); });
  archive.Number(feedbackOverscan_);
  archive.Bounded(currentLine_, uint16_t{0}, uint16_t{511});
  archive.Bounded(currentHClock_, uint16_t{0}, uint16_t{1368});
  archive.Number(cpuPio_);
  archive.Number(fieldId_);
  archive.Number(frameOverscan_);
  archive.Number(displayInterlace_);
  const auto width = archive.Number(frameWidth_);
  const auto height = archive.Number(frameHeight_);
  archive.Validate((width == 256 || width == 512) &&
                       (height == 224 || height == 239 || height == 448 || height == 478),
                   "Invalid PPU output dimensions");
  background_->SerializeState(archive);
  object_->SerializeState(archive);
  archive.Number(frameInterlace_);
}
} // namespace snes::core
