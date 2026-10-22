#pragma once

#include "snes/core/Ppu.hpp"

namespace snes::core {

class PpuBackground {
public:
  using Vram = std::span<const uint16_t>;
  struct Sample {
    uint8_t palette = 0;
    uint8_t group = 0;
    uint8_t priority = 0;
  };
  struct Output {
    std::array<Sample, 4> above{}, below{};
  };

  // Reset control and mosaic state while retaining fetched column data.
  void Reset();
  void BeginLine(uint16_t line, bool field, const Ppu::IO &io,
                 Ppu::RenderModel model = Ppu::RenderModel::Progressive);
  // Consume successive two-master-clock edges. True marks a complete output dot.
  bool Clock(uint16_t clock, const Ppu::IO &io, Vram vram);
  const Output &Samples() const noexcept {
    return output_;
  }

private:
  struct Column {
    uint64_t pixels = 0;
    uint16_t address = 0;
    uint8_t palette = 0, group = 0, priority = 0;
    bool flip = false;
  };
  struct Layer {
    std::array<Column, 66> columns{};
    unsigned column = 0, position = 0;
    unsigned mosaicClock = 1, mosaicX = 0;
    Sample mosaicSample{};
  };
  static const Ppu::Background &Registers(const Ppu::IO &io, unsigned layer);
  static uint16_t MapAddress(const Ppu::Background &bg, unsigned x, unsigned y);
  void Fetch(uint16_t clock, const Ppu::IO &io, Vram vram);
  void Name(unsigned layer, unsigned clock, const Ppu::IO &io, Vram vram);
  void Pattern(unsigned layer, unsigned pair, unsigned half, unsigned clock, const Ppu::IO &io,
               Vram vram);
  void Offset(bool vertical, unsigned clock, const Ppu::IO &io, Vram vram);
  void Shift(bool below, unsigned clock, const Ppu::IO &io, Vram vram);
  Sample Mode7(unsigned layer, const Ppu::IO &io, Vram vram);

  std::array<Layer, 4> layers_{};
  Output output_{};
  uint16_t line_ = 0;
  uint16_t horizontalOffset_ = 0, verticalOffset_ = 0;
  bool field_ = false;
  Ppu::RenderModel model_ = Ppu::RenderModel::Progressive;
};

} // namespace snes::core
