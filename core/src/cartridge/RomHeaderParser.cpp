#include "RomHeaderParser.hpp"

#include "HeaderTitle.hpp"

#include <array>

namespace snes::core::detail {
namespace {

MappingType MappingFromModeByte(uint8_t mapMode) {
  if ((mapMode & 0xf0) != 0x20 && (mapMode & 0xf0) != 0x30)
    return MappingType::Unknown;
  switch (mapMode & 0x0F) {
  case 0x00:
    return MappingType::LoRom;
  case 0x01:
    return MappingType::HiRom;
  case 0x02:
    return MappingType::Sdd1;
  case 0x03:
    return MappingType::Sa1;
  case 0x05:
    return MappingType::ExHiRom;
  case 0x0a:
    return MappingType::Spc7110;
  default:
    return MappingType::Unknown;
  }
}

RomSpeed SpeedFromModeByte(uint8_t mapMode) {
  return (mapMode & 0x10) != 0 ? RomSpeed::Fast : RomSpeed::Slow;
}

EnhancementChip DetectChip(const RomHeader &header, uint8_t subtype) {
  const auto type = header.cartridgeType;
  if ((type & 15) >= 3) {
    switch (type >> 4) {
    case 0:
      if (header.title == "DUNGEON MASTER")
        return EnhancementChip::Dsp2;
      if (header.title == "SD\xe3\x82\xac\xe3\x83\xb3\xe3\x83\x80\xe3\x83\xa0GX")
        return EnhancementChip::Dsp3;
      if (header.title == "TOP GEAR 3000" || header.title == "PLANETS CHAMP TG3000")
        return EnhancementChip::Dsp4;
      return EnhancementChip::Dsp1;
    case 1:
      return EnhancementChip::SuperFx;
    case 2:
      return EnhancementChip::Obc1;
    case 3:
      return EnhancementChip::Sa1;
    case 4:
      return EnhancementChip::Sdd1;
    case 5:
      return EnhancementChip::Srtc;
    case 15:
      switch (subtype) {
      case 0:
        if ((type & 15) == 5)
          return EnhancementChip::Spc7110;
        if ((type & 15) == 9)
          return EnhancementChip::Spc7110Rtc;
        break;
      case 1:
        return header.title == "2DAN MORITA SHOUGI" ? EnhancementChip::St011
                                                    : EnhancementChip::St010;
      case 2:
        return EnhancementChip::St018;
      case 0x10:
        return EnhancementChip::Cx4;
      }
      break;
    }
  }
  switch (header.mapping) {
  case MappingType::Sa1:
    return EnhancementChip::Sa1;
  case MappingType::Sdd1:
    return EnhancementChip::Sdd1;
  case MappingType::Spc7110:
    return EnhancementChip::Spc7110;
  default:
    return EnhancementChip::None;
  }
}

int ResetInstructionWeight(uint8_t opcode) {
  switch (opcode) {
  case 0x18:
  case 0x38:
  case 0x4c:
  case 0x5c:
  case 0x78:
  case 0x9c:
    return 8;
  case 0x20:
  case 0x22:
  case 0xa0:
  case 0xa2:
  case 0xa9:
  case 0xac:
  case 0xad:
  case 0xae:
  case 0xaf:
  case 0xc2:
  case 0xe2:
    return 4;
  case 0x40:
  case 0x60:
  case 0x6b:
  case 0xcc:
  case 0xcd:
  case 0xec:
    return -4;
  case 0x00:
  case 0x02:
  case 0x42:
  case 0xdb:
  case 0xff:
    return -8;
  default:
    return 0;
  }
}

uint16_t Read16(std::span<const uint8_t> bytes, size_t offset) {
  return uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8);
}

std::optional<RomHeader> ReadCandidate(std::span<const uint8_t> rom, size_t base) {
  if (base > rom.size() || rom.size() - base < 64)
    return std::nullopt;

  RomHeader candidate;
  candidate.headerOffset = base;
  candidate.title = detail::DecodeHeaderTitle(rom.subspan(base, 21));

  const auto mapMode = rom[base + 0x15];
  candidate.mapMode = mapMode;
  candidate.maker = rom[base + 0x1a];
  candidate.mapping = MappingFromModeByte(mapMode);
  if (candidate.title == "YUYU NO QUIZ DE GO!GO")
    candidate.mapping = MappingType::LoRom;
  candidate.speed = SpeedFromModeByte(mapMode);
  candidate.cartridgeType = rom[base + 0x16];
  candidate.romSizeShift = rom[base + 0x17];
  candidate.sramSizeShift = rom[base + 0x18];
  candidate.expansionRamSizeShift = rom[base - 3];
  candidate.country = rom[base + 0x19];
  candidate.version = rom[base + 0x1B];
  candidate.checksumComplement = Read16(rom, base + 0x1C);
  candidate.checksum = Read16(rom, base + 0x1E);
  candidate.resetVector = Read16(rom, base + 0x3C);
  candidate.chip = DetectChip(candidate, rom[base - 1]);

  int confidence = 0;
  if (candidate.resetVector >= 0x8000) {
    const size_t entry = (base & ~size_t(0x7fff)) | (candidate.resetVector & 0x7fff);
    confidence = ResetInstructionWeight(rom[entry]);
    if ((candidate.checksum ^ candidate.checksumComplement) == 0xffff)
      confidence += 4;
    const uint8_t slowMode = mapMode & 0xef;
    if ((base == 0x7fc0 && slowMode == 0x20) || (base == 0xffc0 && slowMode == 0x21))
      confidence += 2;
  }
  if (confidence > 0)
    candidate.score = uint32_t(confidence) + (base >= 0x400000 ? 4 : 0);

  // Prototype headers may omit the mode byte. The header's physical
  // position still identifies the layout used by the reset vector.
  if (candidate.mapping == MappingType::Unknown) {
    candidate.mapping = base == 0x7fc0     ? MappingType::LoRom
                        : base == 0xffc0   ? MappingType::HiRom
                        : base == 0x407fc0 ? MappingType::ExLoRom
                                           : MappingType::ExHiRom;
    candidate.speed = RomSpeed::Slow;
  }
  if (base == 0x407fc0 && candidate.mapping == MappingType::LoRom)
    candidate.mapping = MappingType::ExLoRom;
  return candidate;
}

} // namespace

std::optional<RomHeader> ParseRomHeader(std::span<const uint8_t> rom) {
  std::optional<RomHeader> selected;
  for (size_t base : std::array<size_t, 4>{0x7fc0, 0xffc0, 0x407fc0, 0x40ffc0}) {
    auto candidate = ReadCandidate(rom, base);
    if (candidate && (!selected || candidate->score > selected->score))
      selected = std::move(candidate);
  }
  return selected;
}

} // namespace snes::core::detail
