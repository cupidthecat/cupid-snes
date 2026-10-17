#pragma once

#include "snes/core/Cartridge.hpp"

namespace snes::core::detail {

std::optional<RomHeader> ParseRomHeader(std::span<const uint8_t> rom);

} // namespace snes::core::detail
