// OBC1 object RAM and register interface.

#pragma once

#include <cstdint>
#include <span>

namespace snes::core {

class Obc1 {
public:
  explicit Obc1(std::span<uint8_t, 8192> ram) : ram_(ram) {
    Reset();
  }
  static bool Selects(uint32_t address) noexcept {
    const unsigned bank = (address >> 16) & 0xff;
    return (address & 0x40e000) == 0x006000 ||
           ((bank & 0x7e) == 0x70 && (address & 0x6000) == 0x6000);
  }
  void Reset() {
    base_ = (ram_[0x1ff5] & 1) ? 0x1800 : 0x1c00;
    object_ = ram_[0x1ff6] & 0x7f;
  }
  uint8_t Read(uint16_t address) const {
    const unsigned port = address & 0x1fff;
    if (port >= 0x1ff0 && port <= 0x1ff3)
      return ram_[base_ + object_ * 4 + port - 0x1ff0];
    if (port == 0x1ff4)
      return ram_[base_ + 0x200 + object_ / 4];
    return ram_[port];
  }
  void Write(uint16_t address, uint8_t value) {
    const unsigned port = address & 0x1fff;
    if (port >= 0x1ff0 && port <= 0x1ff3) {
      ram_[base_ + object_ * 4 + port - 0x1ff0] = value;
      return;
    } else if (port == 0x1ff4) {
      auto &attributes = ram_[base_ + 0x200 + object_ / 4];
      const unsigned shift = (object_ % 4) * 2;
      attributes = static_cast<uint8_t>((attributes & ~(3u << shift)) | ((value & 3u) << shift));
      return;
    } else if (port == 0x1ff5) {
      base_ = (value & 1) ? 0x1800 : 0x1c00;
    } else if (port == 0x1ff6) {
      object_ = value & 0x7f;
    }
    ram_[port] = value;
  }

private:
  std::span<uint8_t, 8192> ram_;
  unsigned base_ = 0x1800;
  unsigned object_ = 0x7f;
};

} // namespace snes::core
