// Handheld ROM banking, cartridge RAM, and battery-backed clock registers.
#pragma once

#include "snes/core/gameboy/MotionEeprom.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace snes::core::gameboy {

class Cartridge {
public:
  using TimeSource = std::function<uint64_t()>;
  bool Load(std::span<const uint8_t> image, std::string *error = nullptr);
  void Reset();
  std::optional<uint8_t> Read(uint16_t address);
  void Write(uint16_t address, uint8_t value);

  void SetTimeSource(TimeSource source);
  void SetClockRate(uint32_t clocksPerSecond) noexcept;
  void AdvanceRtc(unsigned clocks);
  void SynchronizeRtc(uint64_t timestamp);
  std::vector<uint8_t> SaveBattery() const;
  void LoadBattery(std::span<const uint8_t> bytes);
  std::span<const uint8_t> Ram() const noexcept {
    return ram_;
  }
  bool Rumble() const noexcept {
    return rumble_;
  }
  void SetMotion(double x, double y) noexcept {
    motion_.SetMotion(x, y);
  }

private:
  enum class Mapper { None, Mbc1, Mbc2, Mbc3, Mbc5, Mbc7, Mmm01 };
  struct Multicart {
    uint8_t romLow = 0, romMid = 0, romHigh = 0, romMask = 0;
    uint8_t ramLow = 0, ramHigh = 0, ramMask = 3;
    bool locked = false, multiplex = false, mode = false, modeDisabled = false;
  } multicartRegisters_;
  MotionEeprom motion_;
  std::vector<uint8_t> rom_, ram_;
  Mapper mapper_ = Mapper::None;
  TimeSource timeSource_;
  std::array<uint8_t, 5> rtc_{}, latched_{};
  uint64_t lastTime_ = 0, rtcClocks_ = 0;
  uint32_t rtcClockRate_ = 4194304;
  uint8_t lowBank_ = 0, highBank_ = 0, ramBank_ = 0;
  bool ramEnabled_ = false, mode_ = false, rtcMapped_ = false;
  bool multicart_ = false, extendedMbc3_ = false;
  bool battery_ = false, clock_ = false, hasRumble_ = false, rumble_ = false;
  bool motionEnabled_ = false;

  uint64_t Now() const;
  unsigned RomBank(bool upper) const noexcept;
  std::optional<unsigned> RamBank() const noexcept;
  void AdvanceDays(uint64_t days) noexcept;
  void WriteMulticart(uint16_t address, uint8_t value) noexcept;
};

} // namespace snes::core::gameboy
