#include "RomBoardProfile.hpp"

#include <algorithm>

namespace snes::core::detail {
namespace {

size_t RamBytes(uint8_t declaration) {
  const unsigned bits = std::min(unsigned(declaration & 15), 8u);
  return bits ? size_t(1024) << bits : 0;
}

bool HasWiring(const RomHeader &header, size_t romBytes, bool hasRam) {
  const bool low = header.mapping == MappingType::LoRom;
  const bool high = header.mapping == MappingType::HiRom;
  switch (header.mapping) {
  case MappingType::Bsx:
  case MappingType::SufamiTurbo:
  case MappingType::BroadcastLoRom:
  case MappingType::BroadcastHiRom:
  case MappingType::BroadcastSa1:
  case MappingType::Competition92:
  case MappingType::Competition94:
    return true;
  default:
    break;
  }
  switch (header.chip) {
  case EnhancementChip::Dsp1:
  case EnhancementChip::Dsp2:
  case EnhancementChip::Dsp3:
  case EnhancementChip::Dsp4:
    return low || high;
  case EnhancementChip::Obc1:
    return low && hasRam;
  case EnhancementChip::Srtc:
    return header.mapping == MappingType::ExHiRom && hasRam;
  case EnhancementChip::Cx4:
  case EnhancementChip::St010:
  case EnhancementChip::St011:
    return low && !hasRam;
  case EnhancementChip::St018:
    return low && hasRam;
  case EnhancementChip::Spc7110:
    return hasRam && romBytes != 0x700000;
  case EnhancementChip::Spc7110Rtc:
    return hasRam;
  case EnhancementChip::SuperFx:
  case EnhancementChip::Sa1:
  case EnhancementChip::Sdd1:
  case EnhancementChip::Competition:
    return true;
  case EnhancementChip::None:
    return low || high || header.mapping == MappingType::ExLoRom ||
           header.mapping == MappingType::ExHiRom;
  }
  return false;
}

} // namespace

RomBoardProfile ResolveRomBoardProfile(const RomHeader &header, size_t romBytes,
                                       std::optional<size_t> forcedRam, bool forcedMapping) {
  const size_t ordinary = RamBytes(header.sramSizeShift);
  size_t expansion = header.maker == 0x33 ? RamBytes(header.expansionRamSizeShift) : 0;
  if (!expansion && (header.cartridgeType >> 4) == 1)
    expansion = 0x8000;
  const size_t bytes = forcedRam.value_or(ordinary ? ordinary : expansion);
  return {forcedMapping || HasWiring(header, romBytes, bytes != 0), bytes};
}

} // namespace snes::core::detail
