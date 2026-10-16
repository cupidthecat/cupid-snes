// snes emulator
// core/src/cartridge/Srtc.cpp
// Cartridge real-time clock commands and persistent calendar state.

#include "snes/core/Srtc.hpp"
#include "snes/core/Timing.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <utility>

namespace snes::core {
namespace {
struct DecimalPlace {
  unsigned field, divisor;
};
constexpr DecimalPlace places[]{{0, 1},  {0, 10}, {1, 1}, {1, 10}, {2, 1},   {2, 10}, {3, 1},
                                {3, 10}, {4, 1},  {5, 1}, {5, 10}, {5, 100}, {6, 1}};
constexpr unsigned monthLengths[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
bool Leap(unsigned year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
uint64_t Carries(uint32_t &value, unsigned period, uint64_t count) {
  if (!count)
    return 0;
  uint64_t result = 0;
  if (value >= period) {
    value = 0;
    --count;
    result = 1;
  }
  const uint64_t total = value + count;
  value = uint32_t(total % period);
  return result + total / period;
}
} // namespace

Srtc::Srtc(Clock clock) : clock_(std::move(clock)) {
  if (!clock_)
    clock_ = [] {
      return std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    };
}
void Srtc::Reset() {
  mode_ = Mode::Read;
  index_ = -1;
  balance_ = 0;
}
void Srtc::SetPal(bool pal) {
  masterFrequency_ = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
}
void Srtc::Advance(uint32_t masterClocks) {
  balance_ -= masterClocks;
  if (balance_ >= 0)
    return;
  const uint64_t count = (uint64_t(-balance_) + masterFrequency_ - 1) / masterFrequency_;
  balance_ += int64_t(count * masterFrequency_);
  AdvanceSeconds(count);
}
uint8_t Srtc::Digit(unsigned index) const {
  const auto place = places[index];
  unsigned value = calendar_[place.field] / place.divisor;
  if (index + 1 < std::size(places) && places[index + 1].field == place.field)
    value %= 10;
  return uint8_t(value & 15);
}
void Srtc::Put(unsigned index, uint8_t value) {
  const auto place = places[index];
  auto &field = calendar_[place.field];
  const unsigned higher = index + 1 < std::size(places) && places[index + 1].field == place.field
                              ? field / (place.divisor * 10) * (place.divisor * 10)
                              : 0;
  field = higher + (value & 15) * place.divisor + field % place.divisor;
}
void Srtc::SetWeekday() {
  const unsigned year = 1000 + calendar_[Year];
  const unsigned month = std::clamp(calendar_[Month], 1u, 12u);
  const unsigned day = std::clamp(calendar_[Day], 1u, 31u);
  const auto beforeYear = [](uint64_t value) {
    --value;
    return value * 365 + value / 4 - value / 100 + value / 400;
  };
  uint64_t days = beforeYear(year) - beforeYear(1000) + day - 1;
  for (unsigned index = 0; index + 1 < month; ++index)
    days += monthLengths[index] + (index == 1 && Leap(year));
  calendar_[Weekday] = uint32_t((days + 3) % 7);
}
void Srtc::AdvanceDays(uint64_t count) {
  while (count) {
    const auto month = calendar_[Month];
    const unsigned length = monthLengths[(month - 1) % 12] + (month == 2 && Leap(calendar_[Year]));
    const uint64_t distance = calendar_[Day] <= length ? length + 1 - calendar_[Day] : 1;
    if (count < distance) {
      calendar_[Day] += uint32_t(count);
      return;
    }
    count -= distance;
    calendar_[Day] = 1;
    if (month < 12) {
      ++calendar_[Month];
    } else {
      calendar_[Month] = 1;
      calendar_[Year] = (calendar_[Year] + 1) & 4095;
    }
  }
}
void Srtc::AdvanceHours(uint64_t count) {
  AdvanceDays(Carries(calendar_[Hour], 24, count));
}
void Srtc::AdvanceMinutes(uint64_t count) {
  AdvanceHours(Carries(calendar_[Minute], 60, count));
}
void Srtc::AdvanceSeconds(uint64_t count) {
  AdvanceMinutes(Carries(calendar_[Second], 60, count));
}
uint8_t Srtc::Read() {
  if (mode_ != Mode::Read)
    return 0;
  if (index_ < 0) {
    index_ = 0;
    return 15;
  }
  if (index_ >= 13) {
    index_ = -1;
    return 15;
  }
  return Digit(unsigned(index_++));
}
void Srtc::Write(uint8_t value) {
  value &= 15;
  if (value == 13) {
    mode_ = Mode::Read;
    index_ = -1;
  } else if (value == 14) {
    mode_ = Mode::Command;
  } else if (value != 15) {
    if (mode_ == Mode::Command) {
      mode_ = value == 0 ? Mode::Write : Mode::Idle;
      if (value == 0)
        index_ = 0;
      if (value == 4) {
        calendar_.fill(0);
        index_ = -1;
      }
    } else if (mode_ == Mode::Write && index_ >= 0 && index_ < 12) {
      Put(unsigned(index_++), value);
      if (index_ == 12)
        SetWeekday();
    }
  }
}
std::array<uint8_t, Srtc::SaveSize> Srtc::Save() {
  std::array<uint8_t, SaveSize> data{};
  for (unsigned index = 0; index < 13; ++index)
    data[index] = Digit(index);
  data[13] = 'R';
  data[14] = 'T';
  data[15] = 'C';
  const auto timestamp = uint64_t(clock_());
  for (unsigned index = 0; index < 8; ++index)
    data[16 + index] = uint8_t(timestamp >> (index * 8));
  return data;
}
bool Srtc::Load(std::span<const uint8_t> data) {
  if (data.size() != SaveSize || data[13] != 'R' || data[14] != 'T' || data[15] != 'C' ||
      std::any_of(data.begin(), data.begin() + 13, [](uint8_t value) { return value > 15; }))
    return false;
  uint64_t timestamp = 0;
  for (unsigned index = 0; index < 8; ++index)
    timestamp |= uint64_t(data[16 + index]) << (index * 8);
  if (timestamp > uint64_t(std::numeric_limits<int64_t>::max()))
    return false;
  for (unsigned index = 0; index < 13; ++index)
    Put(index, data[index]);
  const auto now = clock_();
  if (now >= 0 && uint64_t(now) >= timestamp) {
    const uint64_t elapsed = uint64_t(now) - timestamp;
    if (elapsed <= 315576000000ull) {
      AdvanceDays(elapsed / 86400);
      AdvanceHours(elapsed / 3600 % 24);
      AdvanceMinutes(elapsed / 60 % 60);
      AdvanceSeconds(elapsed % 60);
    }
  }
  Reset();
  return true;
}
} // namespace snes::core
