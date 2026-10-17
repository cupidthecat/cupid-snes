#pragma once

#include "snes/core/Cartridge.hpp"

namespace snes::core::detail {

struct RomBoardProfile {
  bool connected = false;
  size_t ramBytes = 0;
};

RomBoardProfile ResolveRomBoardProfile(const RomHeader &header, size_t romBytes,
                                       std::optional<size_t> forcedRam, bool forcedMapping);

} // namespace snes::core::detail
