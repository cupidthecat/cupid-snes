#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace snes::core::detail {
class Sha256Hasher {
public:
  void Update(std::span<const uint8_t> bytes) noexcept;
  std::array<uint8_t, 32> Finish() const noexcept;

private:
  void Block(std::span<const uint8_t, 64> bytes) noexcept;
  std::array<uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<uint8_t, 64> tail_{};
  uint64_t length_ = 0;
  size_t used_ = 0;
};
std::array<uint8_t, 32> Sha256(std::span<const uint8_t> bytes);
} // namespace snes::core::detail
