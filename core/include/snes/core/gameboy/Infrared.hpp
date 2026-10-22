// Cartridge infrared transmitter and light sensor.
#pragma once

#include <cstdint>

namespace snes::core::gameboy {

class Infrared {
public:
  void Reset() noexcept;
  void SetInput(bool on) noexcept {
    input_ = on;
  }
  void SetOutput(bool on) noexcept {
    output_ = on;
  }
  bool Output() const noexcept {
    return output_;
  }
  bool Detected() const noexcept {
    return detected_;
  }
  // Advance one CPU bus timing batch in handheld clocks.
  void Advance(unsigned clocks, bool sensing) noexcept;

private:
  uint64_t charge_ = 0;
  bool input_ = false, output_ = false, detected_ = false;
};

} // namespace snes::core::gameboy
