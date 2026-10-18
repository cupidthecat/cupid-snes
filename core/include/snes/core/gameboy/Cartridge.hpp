// Handheld ROM banking, cartridge RAM, and battery-backed clock registers.
#pragma once

#include "snes/core/gameboy/Camera.hpp"
#include "snes/core/gameboy/Huc3Clock.hpp"
#include "snes/core/gameboy/Infrared.hpp"
#include "snes/core/gameboy/MotionEeprom.hpp"
#include "snes/core/gameboy/Tpp1.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
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
  void Advance(unsigned clocks, bool sleeping = false);
  // Sensor clocks precede bus transfers; infrared and RTC clocks follow them.
  void AdvanceCamera(unsigned clocks, bool sleeping);
  void AdvanceClock(unsigned clocks);
  bool SetCameraFrame(std::span<const uint8_t> pixels) {
    return camera_.SetFrame(pixels);
  }
  void SetCameraEntropy(uint64_t seed, bool enabled) noexcept {
    camera_.SetEntropy(seed, enabled);
  }
  void SetCameraNoiseSource(Camera::NoiseSource source) {
    camera_.SetNoiseSource(std::move(source));
  }
  void AdvanceRtc(unsigned clocks);
  void SetInfraredInput(bool on) noexcept {
    infrared_.SetInput(on);
  }
  bool InfraredOutput() const noexcept {
    return infrared_.Output();
  }
  void SynchronizeRtc(uint64_t timestamp);
  std::vector<uint8_t> SaveBattery() const;
  void LoadBattery(std::span<const uint8_t> bytes);
  std::span<const uint8_t> Ram() const noexcept {
    return ram_;
  }
  bool Rumble() const noexcept {
    return RumbleStrength() != 0;
  }
  uint8_t RumbleStrength() const noexcept {
    return mapper_ == Mapper::Tpp1 ? tpp1_.RumbleStrength() : uint8_t(rumble_ ? 3 : 0);
  }
  void SetMotion(double x, double y) noexcept {
    motion_.SetMotion(x, y);
  }

private:
  enum class Mapper { None, Mbc1, Mbc2, Mbc3, Mbc5, Mbc7, Mmm01, Huc1, Huc3, Camera, Tpp1 };
  struct Multicart {
    uint8_t romLow = 0, romMid = 0, romHigh = 0, romMask = 0;
    uint8_t ramLow = 0, ramHigh = 0, ramMask = 3;
    bool locked = false, multiplex = false, mode = false, modeDisabled = false;
  } multicartRegisters_;
  MotionEeprom motion_;
  Infrared infrared_;
  Camera camera_;
  Huc3Clock huc3Clock_;
  Tpp1 tpp1_;
  std::vector<uint8_t> rom_, ram_;
  Mapper mapper_ = Mapper::None;
  TimeSource timeSource_;
  std::array<uint8_t, 5> rtc_{}, latched_{};
  uint64_t lastTime_ = 0, rtcClocks_ = 0;
  uint32_t rtcClockRate_ = 4194304;
  uint8_t lowBank_ = 0, highBank_ = 0, ramBank_ = 0, huc3Mode_ = 0;
  bool ramEnabled_ = false, mode_ = false, rtcMapped_ = false;
  bool multicart_ = false, extendedMbc3_ = false;
  bool battery_ = false, clock_ = false, hasRumble_ = false, rumble_ = false;
  bool motionEnabled_ = false, bankRegistersWritten_ = false, cameraRegisters_ = false;

  uint64_t Now() const;
  unsigned RomBank(bool upper) const noexcept;
  std::optional<unsigned> RamBank() const noexcept;
  void AdvanceDays(uint64_t days) noexcept;
  void WriteMulticart(uint16_t address, uint8_t value) noexcept;
};

} // namespace snes::core::gameboy
