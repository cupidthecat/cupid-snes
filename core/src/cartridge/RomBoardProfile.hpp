#pragma once

#include "snes/core/Cartridge.hpp"

namespace snes::core::detail {

struct RomBoardProfile {
    bool connected = false;
    size_t ramBytes = 0;
};

void SelectRomBoard(RomHeader& header, std::span<const uint8_t> image);

RomBoardProfile ResolveRomBoardProfile(const RomHeader& header, size_t romBytes,
                                       std::optional<size_t> forcedRam, bool forcedMapping);

} // namespace snes::core::detail
