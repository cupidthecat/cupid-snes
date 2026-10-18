// Nibble command interface for the cartridge clock.
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace snes::core::gameboy {

class Huc3Clock {
public:
  void Reset(uint64_t timestamp) noexcept;
  uint8_t Read() const noexcept {
    return flags_ == 2 ? 1 : read_;
  }
  void Write(uint8_t value) noexcept;
  void Synchronize(uint64_t timestamp) noexcept;
  std::array<uint8_t, 17> Save() const noexcept;
  void Load(std::span<const uint8_t> bytes, uint64_t now) noexcept;

private:
  // Minutes, days, alarm minutes, and alarm days, in command-register order.
  std::array<uint16_t, 4> words_{};
  uint64_t timestamp_ = 0;
  uint8_t cursor_ = 0, read_ = 0, flags_ = 0;
  bool alarmEnabled_ = false;
  void WriteNibble(uint8_t value) noexcept;
};

} // namespace snes::core::gameboy
