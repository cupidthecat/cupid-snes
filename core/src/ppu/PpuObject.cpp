#include "snes/core/PpuObject.hpp"

#include <algorithm>

namespace snes::core {

void PpuObject::Reset() {
  banks_ = {};
  row_ = {};
  selected_ = {};
  line_ = 0xffff;
  writing_ = first_ = selectedCount_ = tileCount_ = 0;
  phase_ = Fetch::Idle;
  nextFetch_ = 1080;
  item_ = -1;
}

PpuObject::Attributes PpuObject::Read(Oam oam, unsigned index) {
  const unsigned low = index * 4;
  const unsigned high = (oam[512 + index / 4] >> (index % 4 * 2)) & 3;
  const unsigned flags = oam[low + 3];
  return {oam[low] | ((high & 1) << 8),
          oam[low + 1],
          oam[low + 2],
          (flags >> 1) & 7,
          (flags >> 4) & 3,
          bool(high & 2),
          bool(flags & 1),
          bool(flags & 64),
          bool(flags & 128)};
}

unsigned PpuObject::Width(const Attributes &object, const Ppu::ObjectIO &io) {
  return object.large ? Ppu::kObjLargeWidth[io.baseSize] : Ppu::kObjSmallWidth[io.baseSize];
}

unsigned PpuObject::Height(const Attributes &object, const Ppu::ObjectIO &io) {
  if (!object.large && io.interlace && io.baseSize >= 6)
    return 16;
  return object.large ? Ppu::kObjLargeHeight[io.baseSize] : Ppu::kObjSmallHeight[io.baseSize];
}

void PpuObject::DecodeRow(const Bank &bank) {
  row_ = {};
  for (unsigned i = 0; i < bank.count; ++i) {
    const auto &tile = bank.tiles[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      const unsigned x = (tile.x + bit) & 511;
      if (x >= 256)
        continue;
      const unsigned shifted = tile.planes >> (tile.flip ? bit : 7 - bit);
      const unsigned color =
          (shifted & 1) | ((shifted >> 7) & 2) | ((shifted >> 14) & 4) | ((shifted >> 21) & 8);
      if (color)
        row_[x] = {uint8_t(tile.palette + color), tile.priority};
    }
  }
}

void PpuObject::BeginLine(uint16_t line, const Ppu::ObjectIO &io) {
  if (line_ == line)
    return;
  line_ = line;
  first_ = io.first;
  writing_ ^= 1;
  DecodeRow(banks_[writing_ ^ 1]);
  banks_[writing_].count = 0;
  selectedCount_ = tileCount_ = 0;
  phase_ = Fetch::Idle;
  nextFetch_ = 1080;
}

void PpuObject::Evaluate(unsigned position, Ppu::IO &io, Ppu::Latch &latch, Oam oam) {
  if (io.displayDisable || selectedCount_ > 32)
    return;
  const unsigned index = (first_ + position) & 127;
  const auto object = Read(oam, index);
  const unsigned width = Width(object, io.obj);
  if (object.x > 256 && object.x + width <= 512)
    return;
  const unsigned height = Height(object, io.obj) >> unsigned(io.obj.interlace);
  const unsigned bottom = object.y + height;
  const bool inside =
      (line_ >= object.y && line_ < bottom) || (bottom >= 256 && line_ < (bottom & 255));
  if (!inside)
    return;
  latch.oamAddress = uint16_t(index);
  if (selectedCount_ < selected_.size())
    selected_[selectedCount_] = uint8_t(index);
  ++selectedCount_;
}

void PpuObject::FetchEdge(bool field, Ppu::IO &io, Ppu::Latch &latch, Oam oam, Vram vram) {
  switch (phase_) {
  case Fetch::Idle:
    item_ = int(std::min<unsigned>(selectedCount_, 32)) - 1;
    phase_ = Fetch::Object;
    break;
  case Fetch::Object: {
    if (item_ < 0) {
      io.obj.rangeOver |= selectedCount_ > 32;
      io.obj.timeOver |= tileCount_ > 34;
      phase_ = Fetch::Done;
      return;
    }
    if (io.displayDisable || line_ >= (io.overscan ? 239u : 224u)) {
      --item_;
      nextFetch_ += 8;
      phase_ = Fetch::Delay;
      return;
    }
    index_ = selected_[unsigned(item_)];
    latch.oamAddress = uint16_t(512 + index_ / 4);
    const auto object = Read(oam, index_);
    const unsigned width = Width(object, io.obj), height = Height(object, io.obj);
    columns_ = width / 8;
    x_ = object.x;
    int y = int((line_ - object.y) & 255);
    if (io.obj.interlace)
      y *= 2;
    if (object.flipY) {
      const unsigned block = width == height || y < int(width) ? 0 : width;
      y = int(block * 2 + (width == height ? height : width)) - 1 - y;
    }
    if (io.obj.interlace)
      y += object.flipY ? -int(field) : int(field);
    y_ = unsigned(y) & 255;
    const unsigned alternate = object.alternate ? (1u + io.obj.nameselect) << 12 : 0;
    const unsigned characterY = ((object.character >> 4) + y_ / 8) & 15;
    base_ = uint16_t(io.obj.tiledataAddress + alternate + characterY * 256);
    characterX_ = object.character & 15;
    column_ = 0;
    phase_ = Fetch::Tile;
    break;
  }
  case Fetch::Tile: {
    if (column_ == columns_) {
      --item_;
      phase_ = Fetch::Object;
      break;
    }
    const unsigned x = (x_ + column_ * 8) & 511;
    if (x_ != 256 && x >= 256 && x + 7 < 512) {
      ++column_;
      break;
    }
    if (tileCount_++ >= 34) {
      --item_;
      phase_ = Fetch::Object;
      break;
    }
    auto &bank = banks_[writing_];
    bank.count = tileCount_;
    auto &tile = bank.tiles[tileCount_ - 1];
    const auto attributes = Read(oam, index_);
    tile.x = uint16_t(x);
    tile.palette = uint8_t(128 + attributes.palette * 16);
    tile.priority = uint8_t(attributes.priority);
    tile.flip = attributes.flipX;
    const unsigned column = tile.flip ? columns_ - 1 - column_ : column_;
    const uint16_t character = uint16_t(base_ + ((characterX_ + column) & 15) * 16);
    address_ = uint16_t((character & 0xfff0) | (y_ & 7));
    if (!io.displayDisable)
      tile.planes = vram[address_ & 0x7fff];
    phase_ = Fetch::HighWord;
    nextFetch_ += 4;
    break;
  }
  case Fetch::HighWord:
    if (!io.displayDisable)
      banks_[writing_].tiles[tileCount_ - 1].planes |= uint32_t(vram[(address_ + 8) & 0x7fff])
                                                       << 16;
    ++column_;
    phase_ = Fetch::Tile;
    nextFetch_ += 4;
    break;
  case Fetch::Delay:
    phase_ = Fetch::Object;
    break;
  case Fetch::Done:
    break;
  }
}

void PpuObject::Clock(uint16_t clock, bool field, Ppu::IO &io, Ppu::Latch &latch, Oam oam,
                      Vram vram) {
  if (clock <= 1016 && !(clock & 7))
    Evaluate(clock / 8, io, latch, oam);
  while (clock >= nextFetch_ && phase_ != Fetch::Done)
    FetchEdge(field, io, latch, oam, vram);
}

} // namespace snes::core
