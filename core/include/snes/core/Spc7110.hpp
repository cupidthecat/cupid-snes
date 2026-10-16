#pragma once
#include "snes/core/Spc7110Decoder.hpp"
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace snes::core {
class MemoryBus;
class EpsonRtc;
namespace detail {
class ExecutionContext;
}
class Spc7110 {
public:
  using Clock = std::function<int64_t()>;
  Spc7110(std::span<const uint8_t> rom, bool rtc, Clock clock = {});
  Spc7110(std::span<const uint8_t> rom, std::span<uint8_t> save, bool rtc, Clock clock = {});
  ~Spc7110();
  void Reset();
  void Advance(uint32_t masterClocks);
  void SetPal(bool pal);
  uint8_t Read(uint32_t address, uint8_t openBus = 0xff);
  void Write(uint32_t address, uint8_t value);
  void MapCpu(MemoryBus &bus, uint8_t slot) const;
  std::vector<uint8_t> SaveRtc();
  bool LoadRtc(std::span<const uint8_t> data);

private:
  std::span<const uint8_t> rom_, data_;
  std::span<uint8_t> save_;
  std::array<uint8_t, 64> registers_{};
  Spc7110Decoder decoder_;
  std::unique_ptr<EpsonRtc> rtc_;
  std::unique_ptr<detail::ExecutionContext> execution_;
  uint32_t masterFrequency_ = 21'477'272;
  int64_t balance_ = 0;
  bool idleCycle_ = false, decodePending_ = false, multiplyPending_ = false, dividePending_ = false;
  uint8_t mode_ = 0, tileOffset_ = 0;
  uint32_t streamAddress_ = 0, decodedRow_ = 0;
  std::array<uint8_t, 32> tile_{};
  static size_t Mirror(uint32_t address, size_t size);
  uint8_t Data(uint32_t address) const;
  uint8_t Rom(uint32_t address, uint8_t openBus) const;
  uint8_t ReadRegister(unsigned reg, uint8_t openBus);
  void WriteRegister(unsigned reg, uint8_t value);
  uint32_t Word(unsigned reg, unsigned bytes) const;
  void Store(unsigned reg, unsigned bytes, uint32_t value);
  void RefreshData();
  void AdjustData(unsigned trigger);
  uint8_t ReadStream();
  void Synchronize();
  void Cycle(unsigned clocks);
  void Work();
  void BeginStream();
  void Multiply();
  void Divide();
};
} // namespace snes::core
