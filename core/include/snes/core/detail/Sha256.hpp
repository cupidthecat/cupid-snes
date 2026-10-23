#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace snes::core::detail {
std::array<uint8_t, 32> Sha256(std::span<const uint8_t> bytes);
} // namespace snes::core::detail
