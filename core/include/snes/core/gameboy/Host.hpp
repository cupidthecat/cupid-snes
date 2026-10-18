// Super Game Boy register, video, and joypad-pin interface.
#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace snes::core::gameboy {

class Host {
public:
  explicit Host(std::function<void()> resetHandheld = {});
  void Reset();
  uint8_t Read(uint32_t address);
  void Write(uint32_t address, uint8_t value);

  void BeginFrame() noexcept;
  void BeginLine() noexcept;
  void Pixel(uint8_t color) noexcept;
  uint8_t JoypadWrite(bool p14, bool p15) noexcept;

  uint8_t Control() const noexcept {
    return control_;
  }
  unsigned Divider() const noexcept {
    return divider_;
  }

private:
  using Packet = std::array<uint8_t, 16>;
  std::function<void()> resetHandheld_;
  std::array<uint8_t, 2048> video_{};
  std::array<Packet, 64> packets_{};
  Packet receiving_{}, command_{};
  std::array<uint8_t, 4> controllers_{};
  uint16_t readAddress_ = 0;
  uint8_t readBank_ = 0, writeBank_ = 0, x_ = 0, y_ = 0;
  uint8_t packetHead_ = 0, packetCount_ = 0, receivedBits_ = 0;
  uint8_t control_ = 0, multiplayer_ = 0, controller_ = 0;
  unsigned divider_ = 5;
  bool controllerLocked_ = true, receivingPacket_ = false, released_ = true;

  void PacketPins(bool p14, bool p15) noexcept;
};

} // namespace snes::core::gameboy
