// TPP1 banking, clock commands, rumble levels, and battery clock data.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace snes::core::gameboy {

class Tpp1 {
public:
  void Reset(uint64_t timestamp) noexcept;
  unsigned RomBank() const noexcept {
    return bankWritten_ ? romBank_ : 1;
  }
  uint8_t RumbleStrength() const noexcept {
    return rumble_;
  }
  std::optional<uint8_t> Read(uint16_t address, std::span<const uint8_t> ram) noexcept;
  void Write(uint16_t address, uint8_t value, std::span<uint8_t> ram) noexcept;
  // Return true when the clock reaches a host polling boundary.
  bool Advance(unsigned clocks, uint32_t clocksPerSecond) noexcept;
  void Synchronize(uint64_t timestamp) noexcept;
  std::array<uint8_t, 20> Save(uint64_t now) const noexcept;
  void Load(std::span<const uint8_t> bytes, uint64_t now) noexcept;

private:
  std::array<uint8_t, 5> clock_{}, latched_{};
  uint64_t timestamp_ = 0, phase_ = 0;
  uint16_t romBank_ = 0;
  uint8_t ramBank_ = 0, mode_ = 0, flags_ = 0, rumble_ = 0;
  bool bankWritten_ = false;

  void Command(uint8_t value) noexcept;
  void AdvanceDays(uint64_t days) noexcept;
};

} // namespace snes::core::gameboy
