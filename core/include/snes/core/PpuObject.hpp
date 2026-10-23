#pragma once

#include "snes/core/Ppu.hpp"

namespace snes::core {

class PpuObject {
public:
  using Oam = std::span<const uint8_t, Ppu::OamSize>;
  using Vram = std::span<const uint16_t>;
  struct Sample {
    uint8_t palette = 0;
    uint8_t priority = 0; // Object priority selector, before the live mode table.
  };

  void Reset();
  void SerializeState(detail::StateArchive &archive);
  // Repeated calls for the same line leave its latched rotation unchanged.
  void BeginLine(uint16_t line, const Ppu::ObjectIO &io);
  // Consume eight-clock evaluation edges through H=1016 and successive
  // two-clock edges from H=1080. IO changes occur between consumed edges.
  void Clock(uint16_t clock, bool field, Ppu::IO &io, Ppu::Latch &latch, Oam oam, Vram vram);
  Sample Pixel(unsigned x) const noexcept {
    return row_[x];
  }

private:
  struct Attributes {
    unsigned x, y, character, palette, priority;
    bool large, alternate, flipX, flipY;
  };
  struct Tile {
    uint32_t planes = 0;
    uint16_t x = 0;
    uint8_t palette = 0, priority = 0;
    bool flip = false;
  };
  struct Bank {
    std::array<Tile, 34> tiles{};
    unsigned count = 0;
  };
  enum class Fetch { Idle, Object, Tile, HighWord, Delay, Done };

  static Attributes Read(Oam oam, unsigned index);
  static unsigned Width(const Attributes &object, const Ppu::ObjectIO &io);
  static unsigned Height(const Attributes &object, const Ppu::ObjectIO &io);
  void Evaluate(unsigned position, Ppu::IO &io, Ppu::Latch &latch, Oam oam);
  void FetchEdge(bool field, Ppu::IO &io, Ppu::Latch &latch, Oam oam, Vram vram);
  void DecodeRow(const Bank &bank);

  std::array<Bank, 2> banks_{};
  std::array<Sample, 256> row_{};
  std::array<uint8_t, 32> selected_{};
  uint16_t line_ = 0xffff;
  unsigned writing_ = 0, first_ = 0, selectedCount_ = 0, tileCount_ = 0;
  Fetch phase_ = Fetch::Idle;
  unsigned nextFetch_ = 1080;
  int item_ = -1;
  unsigned index_ = 0, column_ = 0, columns_ = 0, x_ = 0, y_ = 0;
  uint16_t base_ = 0, address_ = 0;
  unsigned characterX_ = 0;
};

} // namespace snes::core
