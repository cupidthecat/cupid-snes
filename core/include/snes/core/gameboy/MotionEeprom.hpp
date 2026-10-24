// MBC7 motion registers and serial EEPROM pins.
#pragma once

#include <cstdint>
#include <span>

namespace snes::core::detail {
class StateArchive;
}

namespace snes::core::gameboy {
class MotionEeprom {
  public:
    void SerializeState(detail::StateArchive& archive);
    void Reset() noexcept;
    void SetMotion(double x, double y) noexcept;
    uint8_t Read(uint16_t address) const noexcept;
    void Write(uint16_t address, uint8_t value, std::span<uint8_t> memory) noexcept;

  private:
    double motionX_ = 0, motionY_ = 0;
    uint16_t x_ = 0x8000, y_ = 0x8000, command_ = 0, output_ = 0xffff;
    uint8_t dataBits_ = 0;
    bool selected_ = false, clock_ = false, input_ = false, dataOut_ = true;
    bool writeEnabled_ = false;
    void RisingEdge(std::span<uint8_t> memory) noexcept;
    void Command(std::span<uint8_t> memory) noexcept;
};
} // namespace snes::core::gameboy
