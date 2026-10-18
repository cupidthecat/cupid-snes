// Monochrome LCD timing, tile fetching, and object composition.
#pragma once

#include <array>
#include <cstdint>

namespace snes::core::gameboy {

class Video {
public:
  struct Output {
    virtual ~Output() = default;
    virtual void BeginFrame() = 0;
    virtual void BeginLine() = 0;
    virtual void Pixel(uint8_t shade) = 0;
  };

  explicit Video(Output &output) : output_(output) {}
  void Reset(bool secondModel = false);
  uint8_t Read(uint16_t address, bool dma = false) const;
  uint8_t Write(uint16_t address, uint8_t value, bool dma = false);
  uint8_t Advance(unsigned clocks);
  void Stop(bool stopped) noexcept {
    stopped_ = stopped;
  }
  void WindowWrite(bool active) noexcept {
    windowWrite_ = active;
    if (!active)
      windowChanged_ = false;
  }
  uint8_t Blocked() const noexcept {
    return blocked_;
  }

private:
  struct Object {
    uint8_t index = 0, x = 0, y = 0, flags = 0;
    uint16_t address = 0;
    uint8_t low = 0, high = 0;
    std::array<uint8_t, 8> pixels{};
    bool fetched = false;
  };
  Output &output_;
  std::array<uint8_t, 8192> vram_{};
  std::array<uint8_t, 160> oam_{};
  std::array<uint8_t, 12> registers_{};
  std::array<uint8_t, 16> fifo_{};
  std::array<Object, 10> objects_{};
  uint16_t dot_ = 0, transferClocks_ = 0, hblankDot_ = 0;
  uint8_t line_ = 0, ly_ = 0, mode_ = 0, irqMode_ = 0xff, blocked_ = 0;
  int comparisonLine_ = -1;
  uint8_t fifoHead_ = 0, fifoSize_ = 0, fetchPhase_ = 0, fetchX_ = 0;
  uint8_t tile_ = 0, low_ = 0, high_ = 0, x_ = 0;
  uint16_t fine_ = 0, scrollDiscard_ = 0;
  bool scrollLatched_ = false;
  uint16_t windowFine_ = 0;
  bool leftWindowScan_ = false, leftWindowExtra_ = false, leftWindowPending_ = false;
  uint8_t scrollFine_ = 0;
  uint8_t objectCount_ = 0, objectStall_ = 0, fetchingObject_ = 0, backgroundResume_ = 0;
  uint8_t windowLine_ = 0, windowStall_ = 0;
  int lastObjectX_ = -1;
  bool warm_ = true, coincidence_ = false, irqLine_ = false, stopped_ = false;
  bool secondModel_ = false, windowReached_ = false, windowActive_ = false;
  bool windowUsed_ = false, lateWindow_ = false;
  bool windowWrite_ = false, windowChanged_ = false;

  uint8_t Tick();
  uint8_t UpdateInterrupt();
  void CompareLine(int line);
  void EndLine();
  void BeginTransfer();
  void Transfer();
  void FetchBackground();
  uint16_t TileAddress() const;
  bool StartWindow(int fine = -1);
  bool StartObject();
  void FetchObject();
  uint8_t Shade(uint8_t background) const;
};

} // namespace snes::core::gameboy
