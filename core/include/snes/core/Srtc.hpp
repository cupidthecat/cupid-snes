// Cartridge real-time clock interface.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace snes::core {
namespace detail {
class StateArchive;
}
class Srtc {
public:
  using Clock = std::function<int64_t()>;
  static constexpr size_t SaveSize = 24;
  explicit Srtc(Clock clock = {});
  void Reset();
  void SerializeState(detail::StateArchive &archive);
  void Advance(uint32_t masterClocks);
  void SetPal(bool pal);
  uint8_t Read();
  void Write(uint8_t value);
  std::array<uint8_t, SaveSize> Save();
  // Restore calendar storage while retaining serial state and oscillator phase.
  bool Load(std::span<const uint8_t> data);

private:
  enum Field { Second, Minute, Hour, Day, Month, Year, Weekday, FieldCount };
  enum class Mode { Idle, Command, Read, Write };
  Clock clock_;
  std::array<uint32_t, FieldCount> calendar_{};
  uint32_t masterFrequency_ = 21'477'272;
  int64_t balance_ = 0;
  int index_ = -1;
  Mode mode_ = Mode::Read;
  uint8_t Digit(unsigned index) const;
  void Put(unsigned index, uint8_t value);
  void SetWeekday();
  void AdvanceSeconds(uint64_t count);
  void AdvanceMinutes(uint64_t count);
  void AdvanceHours(uint64_t count);
  void AdvanceDays(uint64_t count);
};
} // namespace snes::core
