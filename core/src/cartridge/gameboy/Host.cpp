// Cartridge-side registers and the handheld video and serial packet links.
#include "snes/core/gameboy/Host.hpp"

#include <utility>

namespace snes::core::gameboy {

Host::Host(std::function<void()> resetHandheld) : resetHandheld_(std::move(resetHandheld)) {
  Reset();
}

void Host::Reset() {
  video_.fill(0xff);
  packets_.fill({});
  receiving_.fill(0);
  command_.fill(0);
  controllers_.fill(0xff);
  readAddress_ = 0;
  readBank_ = writeBank_ = x_ = y_ = 0;
  packetHead_ = packetCount_ = receivedBits_ = 0;
  control_ = multiplayer_ = controller_ = 0;
  divider_ = 5;
  controllerLocked_ = true;
  receivingPacket_ = false;
  released_ = true;
}

uint8_t Host::Read(uint32_t address) {
  address &= 0x40ffff;
  if (address == 0x6000)
    return uint8_t((y_ & 0xf8) | writeBank_);
  if (address == 0x6002) {
    if (packetCount_ == 0)
      return 0;
    command_ = packets_[packetHead_];
    packetHead_ = uint8_t((packetHead_ + 1) & 63);
    --packetCount_;
    return 1;
  }
  if (address == 0x600f)
    return 0x21;
  if ((address & 0x40fff0) == 0x7000)
    return command_[address & 15];
  if (address == 0x7800) {
    const auto value = video_[readBank_ * 512 + readAddress_];
    readAddress_ = uint16_t((readAddress_ + 1) & 511);
    return value;
  }
  return 0;
}

void Host::Write(uint32_t address, uint8_t value) {
  switch (address & 0xffff) {
  case 0x6001:
    readBank_ = value & 3;
    readAddress_ = 0;
    break;
  case 0x6003: {
    if (!(control_ & 0x80) && (value & 0x80)) {
      Reset();
      if (resetHandheld_)
        resetHandheld_();
    }
    multiplayer_ = (value >> 4) & 3;
    if (multiplayer_ != 0)
      controller_ &= uint8_t(multiplayer_ == 1 ? 2 : 0);
    constexpr unsigned divisors[] = {4, 5, 7, 9};
    divider_ = divisors[value & 3];
    control_ = value;
    break;
  }
  case 0x6004:
  case 0x6005:
  case 0x6006:
  case 0x6007:
    controllers_[(address & 0xffff) - 0x6004] = value;
    break;
  }
}

void Host::BeginFrame() noexcept {
  x_ = y_ = 0;
}

void Host::BeginLine() noexcept {
  x_ = 0;
  ++y_;
  if ((y_ & 7) == 0)
    writeBank_ = uint8_t((writeBank_ + 1) & 3);
}

void Host::Pixel(uint8_t color) noexcept {
  const unsigned x = x_++;
  if (x >= 160)
    return;
  const unsigned address = writeBank_ * 512 + (y_ & 7) * 2 + (x / 8) * 16;
  video_[address] = uint8_t((video_[address] << 1) | (color & 1));
  video_[address + 1] = uint8_t((video_[address + 1] << 1) | ((color >> 1) & 1));
}

uint8_t Host::JoypadWrite(bool p14, bool p15) noexcept {
  if (p14 && p15 && !controllerLocked_) {
    controllerLocked_ = true;
    const unsigned mask = multiplayer_ == 0 ? 0 : multiplayer_ == 1 ? 1 : 3;
    controller_ = uint8_t((controller_ + 1) & mask);
  }
  uint8_t input = p14 && p15 ? uint8_t(15 - controller_) : 15;
  if (!p14)
    input &= controllers_[controller_] & 15;
  if (!p15)
    input &= controllers_[controller_] >> 4;
  if (p14 && !p15)
    controllerLocked_ = !controllerLocked_;
  PacketPins(p14, p15);
  return input;
}

void Host::PacketPins(bool p14, bool p15) noexcept {
  if (!p14 && !p15) {
    receivingPacket_ = true;
    released_ = false;
    receivedBits_ = 0;
    receiving_.fill(0);
    return;
  }
  if (!receivingPacket_)
    return;
  if (p14 && p15) {
    released_ = true;
    return;
  }
  if (!released_) {
    receivingPacket_ = false;
    return;
  }
  released_ = false;
  if (receivedBits_ == 128) {
    if (!p14) {
      if (packetCount_ < packets_.size()) {
        packets_[(packetHead_ + packetCount_) & 63] = receiving_;
        ++packetCount_;
      }
      receivingPacket_ = false;
    }
    return;
  }
  if (!p15)
    receiving_[receivedBits_ / 8] |= uint8_t(1u << (receivedBits_ & 7));
  ++receivedBits_;
}

} // namespace snes::core::gameboy
