#include "snes/core/Cartridge.hpp"

#include <algorithm>
#include <utility>

namespace snes::core {

std::optional<Cartridge>
Cartridge::FromCompetitionCartridge(CompetitionBoard::Settings settings,
                                    const std::array<std::span<const uint8_t>, 4> &images,
                                    std::string *error, const CartridgeFirmware *firmware) {
  if (error)
    error->clear();
  const bool campus = settings.board == CompetitionBoard::Board::CampusChallenge92;
  if ((!campus && settings.board != CompetitionBoard::Board::PowerFest94) ||
      settings.saveRamSize > (campus ? 0x800000u : 0x200000u)) {
    if (error)
      *error = "Invalid competition board or save RAM capacity";
    return std::nullopt;
  }
  CompetitionBoard::Images normalized;
  for (unsigned i = 0; i < images.size(); ++i) {
    normalized[i] = RemoveCopierHeader(images[i], nullptr);
    if (normalized[i].empty() || normalized[i].size() > 0x400000 || normalized[i].size() % 0x8000) {
      if (error)
        *error = "Each competition ROM must contain complete 32 KiB banks, up to 4 MiB";
      return std::nullopt;
    }
  }
  bool deinterleaved = false;
  auto header = NormalizeRomLayout(normalized[0], deinterleaved);
  if (!header) {
    if (error)
      *error = "Unable to parse the competition boot ROM header";
    return std::nullopt;
  }
  std::vector<uint8_t> combined;
  for (const auto &image : normalized)
    combined.insert(combined.end(), image.begin(), image.end());
  Cartridge cart;
  cart.header_ = std::move(*header);
  cart.header_.chip = EnhancementChip::Competition;
  cart.header_.mapping = campus ? MappingType::Competition92 : MappingType::Competition94;
  cart.romCrc32_ = ComputeCrc32(combined);
  cart.competition_ = std::make_unique<CompetitionBoard>(settings, std::move(normalized));
  if (firmware) {
    auto data = firmware->Find(FirmwareKind::Dsp1b);
    if (data.empty())
      data = firmware->Find(FirmwareKind::Dsp1);
    if (!data.empty() && !cart.competition_->LoadFirmware(data)) {
      if (error)
        *error = "Invalid competition DSP-1 firmware";
      return std::nullopt;
    }
  }
  return cart;
}

bool Cartridge::MapCompetitionCpu(MemoryBus &bus, uint8_t slot) const {
  if (!competition_)
    return false;
  competition_->MapCpu(bus, slot);
  return true;
}
} // namespace snes::core
