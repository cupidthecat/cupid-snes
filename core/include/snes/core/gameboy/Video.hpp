// Monochrome LCD timing, tile fetching, and object composition.
#pragma once

#include "snes/core/gameboy/Oam.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <utility>

namespace snes::core::detail {
class StateArchive;
}

namespace snes::core::gameboy {

class Video {
public:
  void SerializeState(detail::StateArchive &archive);
  struct Output {
    virtual ~Output() = default;
    virtual void BeginFrame() = 0;
    virtual void BeginLine() = 0;
    virtual void Pixel(uint8_t shade) = 0;
  };

  explicit Video(Output &output) : output_(output) {}
  void Reset(bool secondModel = false);
  uint8_t Read(uint16_t address, bool dma = false);
  uint8_t Write(uint16_t address, uint8_t value, bool dma = false);
  uint8_t Advance(unsigned clocks);
  void AddressBus(uint16_t address, bool incremented);
  void BlockedOamAccess(uint16_t address, bool write);
  void SetDmaSynchronizer(std::function<void()> synchronize) {
    dmaSynchronizer_ = std::move(synchronize);
  }
  void SetCpuSleeping(bool sleeping) noexcept {
    sleeping_ = sleeping;
  }
  void DmaStatus(uint16_t source, uint8_t position) noexcept {
    dmaSource_ = source;
    dmaPosition_ = position;
  }
  void Stop(bool stopped) noexcept {
    stopped_ = stopped;
  }
  void WindowWrite(bool active) noexcept {
    windowWrite_ = active;
  }
  uint8_t Blocked() const noexcept {
    return blocked_;
  }

private:
  enum : uint8_t {
    MapAddress,
    MapRead,
    LowerAddress,
    LowerRead,
    UpperAddress,
    UpperRead,
    QueueWrite
  };
  struct Object {
    uint8_t index = 0, x = 0, y = 0, flags = 0;
    uint16_t address = 0;
    uint8_t low = 0, high = 0;
    std::array<uint8_t, 8> pixels{};
    bool fetched = false;
  };
  Output &output_;
  std::array<uint8_t, 8192> vram_{};
  Oam oam_;
  std::function<void()> dmaSynchronizer_;
  std::array<uint8_t, 12> registers_{};
  std::array<uint8_t, 8> fifo_{};
  std::array<Object, 10> objects_{};
  uint16_t dot_ = 0, transferClocks_ = 0, hblankDot_ = 0;
  uint8_t line_ = 0, ly_ = 0, mode_ = 0, irqMode_ = 0xff, blocked_ = 0;
  int comparisonLine_ = -1;
  uint8_t fifoHead_ = 0, fifoSize_ = 0, fetchPhase_ = 0, fetchX_ = 0;
  uint8_t tile_ = 0, low_ = 0, high_ = 0, x_ = 0;
  int16_t position_ = -16;
  uint16_t mapAddress_ = 0, dataAddress_ = 0;
  uint8_t scrollFine_ = 0;
  uint8_t objectCount_ = 0, objectStall_ = 0, fetchingObject_ = 0;
  uint8_t windowLine_ = 0xff, windowPhase_ = 0;
  bool objectPending_ = false, objectAborted_ = false;
  bool windowStarting_ = false, insertBackground_ = false;
  bool warm_ = true, coincidence_ = false, irqLine_ = false, stopped_ = false;
  bool secondModel_ = false, windowReached_ = false, windowActive_ = false;
  bool lateWindow_ = false, windowWrite_ = false, windowCheck_ = false, windowReset_ = false;
  bool sleeping_ = false, vramRelease_ = false;
  uint16_t dmaSource_ = 0;
  uint8_t dmaPosition_ = 0xa1, scanY_ = 0, scanX_ = 0;

  uint8_t Tick();
  uint8_t ScannerRow() const noexcept;
  uint8_t ReadObject(uint8_t address) const noexcept;
  uint8_t ReadVram(uint16_t address);
  void SynchronizeDma();
  uint8_t UpdateInterrupt();
  void CompareLine(int line);
  void CompareWindow();
  void EndLine();
  void BeginTransfer();
  void Transfer();
  void FetchBackground();
  uint16_t TileAddress() const;
  bool StartWindow();
  bool StartObject();
  bool AdvanceObject();
  void FetchObject();
  uint8_t Shade(uint8_t background) const;
};

} // namespace snes::core::gameboy
