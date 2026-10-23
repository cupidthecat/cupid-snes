#pragma once

#include "../ConsoleConfig.hpp"
#include "../cartridge/StateFixture.hpp"

namespace snes::tests::emulator_state {
using namespace snes::core;
using cartridge_state::Board;

inline std::shared_ptr<CartridgeFirmware> Firmware(Board board) {
  std::optional<FirmwareKind> kind;
  switch (board) {
  case Board::Dsp1Processor:
  case Board::Competition92Processor:
  case Board::Competition94Processor:
    kind = FirmwareKind::Dsp1b;
    break;
  case Board::Dsp2Processor:
    kind = FirmwareKind::Dsp2;
    break;
  case Board::Dsp3Processor:
    kind = FirmwareKind::Dsp3;
    break;
  case Board::Dsp4Processor:
    kind = FirmwareKind::Dsp4;
    break;
  case Board::St010Processor:
    kind = FirmwareKind::St010;
    break;
  case Board::St011Processor:
    kind = FirmwareKind::St011;
    break;
  case Board::St018Processor:
    kind = FirmwareKind::St018;
    break;
  case Board::Cx4Processor:
    kind = FirmwareKind::Cx4;
    break;
  default:
    break;
  }
  if (!kind)
    return {};
  auto firmware = std::make_shared<CartridgeFirmware>();
  std::string error;
  if (!firmware->Set(*kind, std::vector<uint8_t>(CartridgeFirmware::Size(*kind)), &error))
    throw std::runtime_error(error);
  return firmware;
}

inline void Load(Emulator &core, Board board) {
  using namespace cartridge_state;
  bool loaded = false;
  std::string error;
  switch (board) {
  case Board::Handheld:
  case Board::Handheld2:
    loaded = core.LoadSuperGameBoy(
        Image(0x20, 0xe3, 0x80000, board == Board::Handheld2 ? "Super GAMEBOY2" : "Super GAMEBOY"),
        handheld::Image(), handheld::Boot(), &error);
    break;
  case Board::Sufami: {
    auto bios = Image(0x20, 0, 0x40000);
    std::copy_n("BANDAI SFC-ADX", 14, bios.begin());
    std::copy_n("SFC-ADX BACKUP", 14, bios.begin() + 0x10);
    std::array<std::vector<uint8_t>, 2> slots;
    for (unsigned n = 0; n < slots.size(); ++n) {
      slots[n] = std::vector<uint8_t>(0x20000, uint8_t(n * 31));
      std::copy_n("BANDAI SFC-ADX", 14, slots[n].begin());
      slots[n][0x37] = uint8_t(8 + n * 8);
    }
    loaded = core.LoadSufamiTurbo(bios, slots[0], slots[1], &error);
    break;
  }
  case Board::BsxFlash:
  case Board::BsxRom:
  case Board::SlotLo:
  case Board::SlotHi:
  case Board::SlotSa1:
    loaded = core.LoadBroadcastCartridge(SlotBase(board), Pack(board), &error);
    break;
  case Board::Competition92:
  case Board::Competition94:
  case Board::Competition92Processor:
  case Board::Competition94Processor: {
    std::array<std::vector<uint8_t>, 4> images;
    std::array<std::span<const uint8_t>, 4> spans;
    for (unsigned n = 0; n < images.size(); ++n) {
      images[n] = Image(0x20, 0, 0x8000);
      images[n][0x1234] = uint8_t(n * 17);
      spans[n] = images[n];
    }
    CompetitionBoard::Settings settings;
    settings.board = board == Board::Competition92 || board == Board::Competition92Processor
                         ? CompetitionBoard::Board::CampusChallenge92
                         : CompetitionBoard::Board::PowerFest94;
    settings.dipSwitches = 3;
    settings.saveRamSize = 8192;
    loaded = core.LoadCompetitionCartridge(settings, spans, &error);
    break;
  }
  default: {
    auto cartridge = Create(board);
    loaded = core.LoadCartridge(cartridge.RomData(), &error);
    break;
  }
  }
  if (!loaded)
    throw std::runtime_error("Load whole-machine board fixture: " + error);
}
} // namespace snes::tests::emulator_state
