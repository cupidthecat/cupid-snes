// Handheld power-on noise and model-specific memory patterns.
#pragma once

#include <array>
#include <cstdint>

namespace snes::core::gameboy {

class StartupState {
public:
  struct Memory {
    std::array<uint8_t, 8192> work{};
    std::array<uint8_t, 127> high{};
    std::array<uint8_t, 160> objects{};
    std::array<uint8_t, 16> wave{};
  };

  void Configure(bool enabled, uint64_t seed) noexcept;
  static uint64_t SeedFromTime(uint64_t timestamp) noexcept;
  Memory Power(bool secondModel) noexcept;
  uint8_t NoiseByte() noexcept;

private:
  uint64_t state_ = 0;
  bool enabled_ = false;

  uint8_t Combine(unsigned count, bool mostlySet) noexcept;
};

} // namespace snes::core::gameboy
