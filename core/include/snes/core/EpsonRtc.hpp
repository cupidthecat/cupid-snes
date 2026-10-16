#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace snes::core {
class EpsonRtc {
public:
  using Clock = std::function<int64_t()>;
  explicit EpsonRtc(Clock clock = {});
  void Reset();
  void Advance(uint32_t masterClocks);
  void SetPal(bool pal);
  uint8_t Read(unsigned port, uint8_t openBus);
  void Write(unsigned port, uint8_t value);
  std::vector<uint8_t> Save();
  bool Load(std::span<const uint8_t> data);

private:
  enum class Phase { Mode, Address, Read, Write };
  Clock clock_;
  std::array<uint8_t, 16> digits_{};
  uint32_t masterFrequency_ = 21'477'272, ticks_ = 0, seconds_ = 0;
  int64_t balance_ = 0;
  uint8_t selected_ = 0, position_ = 0, last_ = 0, wait_ = 0;
  bool ready_ = false, resync_ = false, irq_ = false, heldTick_ = false;
  Phase phase_ = Phase::Mode;
  void Ticks(uint64_t count);
  void Periodic(unsigned period);
  void Round();
  void Second();
  void Minute();
  void Hour();
  void Day();
  void Month();
  void Year();
  uint8_t Digit(unsigned index);
  void Put(unsigned index, uint8_t value);
};
} // namespace snes::core
