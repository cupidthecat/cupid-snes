#include "snes/core/PpuBackground.hpp"

namespace snes::core {
namespace {
constexpr auto SpreadPixels() {
  std::array<uint64_t, 256> table{};
  for (unsigned value = 0; value < 256; ++value)
    for (unsigned pixel = 0; pixel < 8; ++pixel)
      table[value] |= uint64_t((value >> (7 - pixel)) & 1) << (pixel * 8);
  return table;
}
constexpr auto Spread = SpreadPixels();
unsigned Reverse(unsigned value) {
  value = ((value & 0x55) << 1) | ((value >> 1) & 0x55);
  value = ((value & 0x33) << 2) | ((value >> 2) & 0x33);
  return ((value << 4) | (value >> 4)) & 255;
}
bool Hires(const Ppu::IO &io) {
  return io.bgMode == 5 || io.bgMode == 6;
}
} // namespace

void PpuBackground::Reset() {
  output_ = {};
  line_ = horizontalOffset_ = verticalOffset_ = 0;
  field_ = false;
  for (auto &layer : layers_) {
    layer.column = layer.position = layer.mosaicClock = layer.mosaicX = 0;
    layer.mosaicSample = {};
  }
}

const Ppu::Background &PpuBackground::Registers(const Ppu::IO &io, unsigned layer) {
  switch (layer) {
  case 0:
    return io.bg1;
  case 1:
    return io.bg2;
  case 2:
    return io.bg3;
  default:
    return io.bg4;
  }
}

void PpuBackground::BeginLine(uint16_t line, bool field, const Ppu::IO &io) {
  line_ = line;
  field_ = field;
  horizontalOffset_ = verticalOffset_ = 0;
  for (unsigned i = 0; i < 4; ++i) {
    auto &layer = layers_[i];
    layer.column = 0;
    layer.position = ((Registers(io, i).hoffset & 7) << unsigned(Hires(io))) & 7;
    layer.mosaicClock = io.mosaic.size;
    layer.mosaicX = 0;
  }
}

uint16_t PpuBackground::MapAddress(const Ppu::Background &bg, unsigned x, unsigned y) {
  unsigned offset = (x & 31) | ((y & 31) << 5);
  if ((x & 32) && (bg.screenSize & 1))
    offset += 0x400;
  if ((y & 32) && (bg.screenSize & 2))
    offset += (bg.screenSize & 1) ? 0x800 : 0x400;
  return uint16_t(bg.screenAddress + offset) & 0x7fff;
}

void PpuBackground::Name(unsigned id, unsigned clock, const Ppu::IO &io, Vram vram) {
  const auto &bg = Registers(io, id);
  const bool hires = Hires(io);
  const unsigned depthMode = unsigned(bg.tileMode);
  unsigned y = line_;
  if (hires && io.interlace)
    y = (y << 1) | unsigned(field_ && !bg.mosaicEnable);
  if (bg.mosaicEnable)
    y = unsigned(int(y) -
                 (int(io.mosaic.size) - io.mosaic.counter) * (hires && io.interlace ? 2 : 1));
  for (unsigned half = 0; half <= unsigned(hires); ++half) {
    const unsigned screenX = ((clock & ~31u) / 4 << unsigned(hires)) + half * 8;
    const unsigned scrollX = unsigned(bg.hoffset) << unsigned(hires);
    unsigned x = screenX + scrollX, row = y + bg.voffset;
    if (io.bgMode == 2 || io.bgMode == 4 || io.bgMode == 6) {
      const unsigned select = 0x2000u << id;
      if (io.bgMode == 4) {
        if (horizontalOffset_ & select) {
          if (horizontalOffset_ & 0x8000)
            row = y + horizontalOffset_;
          else
            x = screenX + (horizontalOffset_ & ~7u) + (scrollX & 7);
        }
      } else {
        if (horizontalOffset_ & select)
          x = screenX + (horizontalOffset_ & ~7u) + (scrollX & 7);
        if (verticalOffset_ & select)
          row = y + verticalOffset_;
      }
    }
    const unsigned width = 256u << unsigned(hires);
    x &= (width << unsigned(bg.tileSize) << (bg.screenSize & 1)) - 1;
    row &= (width << unsigned(bg.tileSize) << ((bg.screenSize >> 1) & 1)) - 1;
    const unsigned xShift = hires ? 4 : 3 + unsigned(bg.tileSize);
    const unsigned yShift = 3 + unsigned(bg.tileSize);
    const uint16_t entry = vram[MapAddress(bg, x >> xShift, row >> yShift)];
    unsigned character = entry & 1023;
    if (xShift == 4 && bool(x & 8) != bool(entry & 0x4000))
      ++character;
    if (yShift == 4 && bool(row & 8) != bool(entry & 0x8000))
      character += 16;
    if (entry & 0x8000)
      row ^= 7;
    auto &column = layers_[id].columns[(clock / 32 << unsigned(hires)) + half];
    const unsigned shift = 3 + depthMode;
    const unsigned tile = ((character & 1023) + (bg.tiledataAddress >> shift)) & (0x7fff >> shift);
    column.address = uint16_t((tile << shift) + (row & 7));
    column.group = (entry >> 10) & 7;
    column.palette = uint8_t((io.bgMode == 0 ? id * 32 : 0) +
                             (unsigned(column.group) << ((2u << depthMode) & 31)));
    column.priority = bg.priority[(entry >> 13) & 1];
    column.flip = entry & 0x4000;
  }
}

void PpuBackground::Pattern(unsigned id, unsigned pair, unsigned half, unsigned clock,
                            const Ppu::IO &io, Vram vram) {
  auto &column = layers_[id].columns[(clock / 32 << unsigned(Hires(io))) + half];
  const uint16_t word = vram[(column.address + pair * 8) & 0x7fff];
  unsigned low = word & 255, high = word >> 8;
  if (column.flip) {
    low = Reverse(low);
    high = Reverse(high);
  }
  const uint64_t mask = 0x0303030303030303ull << (pair * 2);
  const uint64_t pixels = (Spread[low] | (Spread[high] << 1)) << (pair * 2);
  column.pixels = (column.pixels & ~mask) | pixels;
}

void PpuBackground::Offset(bool vertical, unsigned clock, const Ppu::IO &io, Vram vram) {
  const auto &bg = io.bg3;
  const unsigned x = clock / 32 * 8 + (bg.hoffset & ~7u);
  const unsigned y = bg.voffset + (vertical ? 8 : 0);
  const unsigned xShift = Hires(io) ? 3 : 3 + unsigned(bg.tileSize);
  const uint16_t word = vram[MapAddress(bg, x >> xShift, y >> (3 + unsigned(bg.tileSize)))];
  (vertical ? verticalOffset_ : horizontalOffset_) = word;
}

void PpuBackground::Fetch(uint16_t clock, const Ppu::IO &io, Vram vram) {
  // Each entry identifies the bus owner and which part of its column is fetched.
  // Names are 0-3, offset words are 4-5, and pattern words encode layer/pair/half.
  constexpr unsigned pattern = 0x10;
  constexpr unsigned schedule[7][8] = {
      {3, 2, 1, 0, 0x28, 0x20, 0x18, 0x10},    {2, 1, 0, 0x20, 0x18, 0x1a, 0x10, 0x12},
      {1, 0, 4, 5, 0x18, 0x1a, 0x10, 0x12},    {1, 0, 0x18, 0x1a, 0x10, 0x12, 0x14, 0x16},
      {1, 0, 4, 0x18, 0x10, 0x12, 0x14, 0x16}, {1, 0, 0x18, 0x19, 0x10, 0x12, 0x11, 0x13},
      {1, 0, 4, 5, 0x10, 0x12, 0x11, 0x13}};
  if (io.bgMode >= 7)
    return;
  const unsigned operation = schedule[io.bgMode][(clock / 4) & 7];
  if (operation < 4)
    Name(operation, clock, io, vram);
  else if (operation < pattern)
    Offset(operation == 5, clock, io, vram);
  else
    Pattern((operation - pattern) / 8, (operation / 2) & 3, operation & 1, clock, io, vram);
}

PpuBackground::Sample PpuBackground::Mode7(unsigned id, const Ppu::IO &io, Vram vram) {
  auto &layer = layers_[id];
  const auto &bg = Registers(io, id);
  unsigned x = layer.mosaicX;
  if (!bg.mosaicEnable)
    ++layer.mosaicX;
  else if (--layer.mosaicClock == 0) {
    layer.mosaicClock = io.mosaic.size;
    layer.mosaicX += io.mosaic.size;
  }
  int y = line_;
  if (io.bg1.mosaicEnable)
    y -= io.mosaic.size - io.mosaic.counter;
  if (io.mode7.hflip)
    x = 255 - x;
  if (io.mode7.vflip)
    y = 255 - y;
  const auto signed13 = [](uint16_t n) { return int(n & 0x1fff) - ((n & 0x1000) ? 0x2000 : 0); };
  const auto clip = [](int n) { return (n & 0x2000) ? (n | ~1023) : (n & 1023); };
  const int a = int16_t(io.mode7.a), b = int16_t(io.mode7.b);
  const int c = int16_t(io.mode7.c), d = int16_t(io.mode7.d);
  const int centerX = signed13(io.mode7.x), centerY = signed13(io.mode7.y);
  const int h = clip(signed13(io.mode7.hoffset) - centerX);
  const int v = clip(signed13(io.mode7.voffset) - centerY);
  const int pixelX =
      ((a * h & ~63) + (b * v & ~63) + (b * y & ~63) + centerX * 256 + a * int(x)) >> 8;
  const int pixelY =
      ((c * h & ~63) + (d * v & ~63) + (d * y & ~63) + centerY * 256 + c * int(x)) >> 8;
  const bool outside = (pixelX | pixelY) & ~1023;
  if (outside && io.mode7.repeat == 2)
    return {};
  const unsigned map = (((pixelY >> 3) & 127) << 7) | ((pixelX >> 3) & 127);
  const unsigned tile = outside && io.mode7.repeat == 3 ? 0 : vram[map] & 255;
  const unsigned address = tile * 64 + ((pixelY & 7) << 3) + (pixelX & 7);
  unsigned palette = vram[address] >> 8;
  const unsigned priority = bg.priority[id == 0 ? 0 : palette >> 7];
  if (id != 0)
    palette &= 127;
  return palette ? Sample{uint8_t(palette), 0, uint8_t(priority)} : Sample{};
}

void PpuBackground::Shift(bool below, unsigned clock, const Ppu::IO &io, Vram vram) {
  const bool hires = Hires(io);
  if (below)
    output_ = {};
  if (below && !hires)
    return;
  const auto shiftLayer = [&]<unsigned id>() {
    auto &layer = layers_[id];
    const auto &bg = Registers(io, id);
    Sample sample;
    if (bg.tileMode == Ppu::TileMode::Mode7)
      sample = Mode7(id, io, vram);
    else {
      auto &column = layer.columns[layer.column];
      const unsigned bits =
          bg.tileMode == Ppu::TileMode::Inactive ? 8 : 2u << unsigned(bg.tileMode);
      const unsigned color = unsigned(column.pixels & ((1u << bits) - 1));
      column.pixels >>= 8;
      sample = {color ? uint8_t(column.palette + color) : uint8_t{0}, column.group,
                column.priority};
      if (++layer.position == 8) {
        layer.position = 0;
        ++layer.column;
      }
      const bool first = clock < 60;
      if ((!hires || below) && (first || --layer.mosaicClock == 0)) {
        layer.mosaicClock = io.mosaic.size;
        layer.mosaicSample = sample;
      } else if (bg.mosaicEnable)
        sample = layer.mosaicSample;
    }
    if (!sample.palette)
      return;
    if ((!hires || !below) && bg.aboveEnable)
      output_.above[id] = sample;
    if ((!hires || below) && bg.belowEnable)
      output_.below[id] = sample;
  };
  shiftLayer.operator()<0>();
  shiftLayer.operator()<1>();
  shiftLayer.operator()<2>();
  shiftLayer.operator()<3>();
}

bool PpuBackground::Clock(uint16_t clock, const Ppu::IO &io, Vram vram) {
  if (clock == 56)
    for (auto &layer : layers_)
      layer.columns[0].pixels >>= layer.position * 8;
  if (!line_)
    return false;
  if (clock <= 1054 && !(clock & 3))
    Fetch(clock, io, vram);
  if (clock >= 56 && clock <= 1078)
    Shift(!(clock & 2), clock, io, vram);
  return clock >= 58 && clock <= 1078 && (clock & 3) == 2;
}

} // namespace snes::core
