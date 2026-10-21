// Cartridge loading, header detection, mapping, and save RAM.

#include "snes/core/Cartridge.hpp"

#include "RomBoardProfile.hpp"
#include "RomHeaderParser.hpp"

#include "snes/core/Logging.hpp"
#include "snes/core/Sdd1.hpp"
#include "snes/core/gameboy/Adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string_view>

namespace snes::core {

namespace {

size_t MirrorOffset(size_t offset, size_t size) {
  if (size == 0) {
    return 0;
  }

  // Fold unconnected address lines while retaining each populated block.
  // For example, a 3 MiB image repeats its last MiB at offsets 3..4 MiB.
  size_t base = 0;
  while (offset >= size) {
    const size_t block = std::bit_floor(offset);
    offset -= block;
    if (size > block) {
      base += block;
      size -= block;
    }
  }
  return base + offset;
}

bool IsFastRegion(uint32_t cpuAddress) {
  const auto bank = static_cast<uint8_t>(cpuAddress >> 16);
  const auto addr = static_cast<uint16_t>(cpuAddress & 0xFFFF);

  if (bank >= 0xC0) {
    return true;
  }

  if (bank >= 0x80 && bank <= 0xBF && addr >= 0x8000) {
    return true;
  }

  return false;
}

std::optional<FirmwareKind> SignalFirmware(const RomHeader &header) {
  switch (header.chip) {
  case EnhancementChip::Dsp1:
    return header.title == "PILOTWINGS" ? FirmwareKind::Dsp1 : FirmwareKind::Dsp1b;
  case EnhancementChip::Dsp2:
    return FirmwareKind::Dsp2;
  case EnhancementChip::Dsp3:
    return FirmwareKind::Dsp3;
  case EnhancementChip::Dsp4:
    return FirmwareKind::Dsp4;
  case EnhancementChip::St010:
    return FirmwareKind::St010;
  case EnhancementChip::St011:
    return FirmwareKind::St011;
  case EnhancementChip::St018:
    return FirmwareKind::St018;
  case EnhancementChip::Cx4:
    return FirmwareKind::Cx4;
  default:
    return std::nullopt;
  }
}

size_t AppendedFirmwareSize(const RomHeader &header, std::span<const uint8_t> image) {
  // Trailer layout follows the raw header even when an identifier selects
  // different board hardware.
  const uint8_t family = header.cartridgeType >> 4;
  const uint8_t device = header.cartridgeType & 15;
  const uint8_t subtype = image[header.headerOffset - 1];
  size_t bytes = 0;
  size_t alignment = 0x8000;
  if (device >= 3) {
    if (family == 0)
      bytes = 0x2000;
    if (family == 15) {
      if (subtype == 0x10)
        bytes = 0xc00;
      if (subtype == 1) {
        bytes = 0xd000;
        alignment = 0x10000;
      }
      if (subtype == 2) {
        bytes = 0x28000;
        alignment = 0x40000;
      }
    }
  }
  const auto identifier = image.subspan(header.headerOffset - 14, 4);
  if ((family == 14 && device == 3) ||
      (header.maker == 0x33 && std::equal(identifier.begin(), identifier.end(), "042J")))
    bytes = 0x100;
  return bytes && image.size() > bytes && image.size() % alignment == bytes ? bytes : 0;
}

bool HasSignature(std::span<const uint8_t> bytes, size_t offset, std::string_view signature) {
  return offset <= bytes.size() && signature.size() <= bytes.size() - offset &&
         std::equal(signature.begin(), signature.end(), bytes.begin() + offset);
}

} // namespace

Cartridge::Cartridge() = default;
Cartridge::~Cartridge() = default;
Cartridge::Cartridge(Cartridge &&) noexcept = default;
Cartridge &Cartridge::operator=(Cartridge &&) noexcept = default;

const char *MappingName(MappingType mapping) noexcept {
  switch (mapping) {
  case MappingType::LoRom:
    return "LoROM";
  case MappingType::HiRom:
    return "HiROM";
  case MappingType::ExLoRom:
    return "ExLoROM";
  case MappingType::ExHiRom:
    return "ExHiROM";
  case MappingType::LoRomNoMad1:
    return "LoROM (NoMAD-1)";
  case MappingType::LoRom24Mbit:
    return "LoROM (24 Mbit board)";
  case MappingType::LoRomLargeSram:
    return "LoROM (large SRAM board)";
  case MappingType::St010:
    return "ST010 LoROM";
  case MappingType::Spc7110:
    return "SPC7110 HiROM";
  case MappingType::Sa1:
    return "SA-1";
  case MappingType::SuperFx:
    return "Super FX";
  case MappingType::St011:
    return "ST011 LoROM";
  case MappingType::BroadcastSa1:
    return "BS cartridge (SA-1)";
  case MappingType::Bsx:
    return "BS-X BIOS";
  case MappingType::Competition92:
    return "Campus Challenge '92";
  case MappingType::Competition94:
    return "PowerFest '94";
  case MappingType::SufamiTurbo:
    return "Sufami Turbo";
  case MappingType::BroadcastLoRom:
    return "BS cartridge (LoROM)";
  case MappingType::BroadcastHiRom:
    return "BS cartridge (HiROM)";
  case MappingType::Sdd1:
    return "S-DD1";
  default:
    return "Unknown";
  }
}

const char *ChipName(EnhancementChip chip) noexcept {
  switch (chip) {
  case EnhancementChip::None:
    return "None";
  case EnhancementChip::Dsp1:
    return "DSP-1";
  case EnhancementChip::Dsp2:
    return "DSP-2";
  case EnhancementChip::Dsp3:
    return "DSP-3";
  case EnhancementChip::Dsp4:
    return "DSP-4";
  case EnhancementChip::Obc1:
    return "OBC1";
  case EnhancementChip::Srtc:
    return "S-RTC";
  case EnhancementChip::SuperFx:
    return "Super FX";
  case EnhancementChip::Sa1:
    return "SA-1";
  case EnhancementChip::Sdd1:
    return "S-DD1";
  case EnhancementChip::Spc7110:
    return "SPC7110";
  case EnhancementChip::Spc7110Rtc:
    return "SPC7110 + RTC";
  case EnhancementChip::Cx4:
    return "Cx4";
  case EnhancementChip::St010:
    return "ST010";
  case EnhancementChip::St011:
    return "ST011";
  case EnhancementChip::St018:
    return "ST018";
  case EnhancementChip::Competition:
    return "Competition timer + DSP-1";
  case EnhancementChip::SuperGameBoy:
    return "Super Game Boy";
  }
  return "Unknown";
}

CartridgeDatabase::CartridgeDatabase() {
  AddOverride(0x00000000, DatabaseOverride{});
}

void CartridgeDatabase::AddOverride(uint32_t crc32, DatabaseOverride entry) {
  overrides_[crc32] = std::move(entry);
}

std::optional<DatabaseOverride> CartridgeDatabase::Find(uint32_t crc32) const {
  if (const auto it = overrides_.find(crc32); it != overrides_.end()) {
    return it->second;
  }

  return std::nullopt;
}

std::optional<Cartridge> Cartridge::FromRomImage(std::span<const uint8_t> romImage,
                                                 RomNormalizationInfo *normalization,
                                                 const CartridgeDatabase *database,
                                                 std::string *error,
                                                 const CartridgeFirmware *firmware) {
  if (error)
    error->clear();
  bool removedHeader = false;
  auto normalized = RemoveCopierHeader(romImage, &removedHeader);

  // Combined images reserve one MiB for the base and one MiB per slot.
  if (HasSignature(normalized, 0, "BANDAI SFC-ADX") &&
      HasSignature(normalized, 0x10, "SFC-ADX BACKUP") && normalized.size() > 0x40000) {
    if (normalized.size() != 0x200000 && normalized.size() != 0x300000) {
      if (error)
        *error = "Combined Sufami Turbo image must be 2 or 3 MiB";
      return std::nullopt;
    }
    const auto image = std::span<const uint8_t>(normalized);
    const auto slot = [&](size_t offset) -> std::span<const uint8_t> {
      if (offset >= image.size())
        return {};
      const auto bytes = image.subspan(offset, 0x100000);
      if (std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0xff; }) ||
          std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0; }))
        return {};
      return bytes;
    };
    auto combined = FromSufamiTurbo(image.first(0x40000), slot(0x100000), slot(0x200000), error);
    if (combined) {
      combined->romCrc32_ = ComputeCrc32(normalized);
      if (normalization)
        *normalization = {removedHeader, false};
    }
    return combined;
  }

  bool deinterleaved = false;
  auto header = detail::ParseRomHeader(normalized);
  if (!header.has_value()) {
    if (error != nullptr) {
      *error = "Unable to parse SNES header from ROM image";
    }
    return std::nullopt;
  }

  Cartridge cart;
  cart.rom_ = std::move(normalized);
  cart.header_ = std::move(*header);
  const auto parsedMapping = cart.header_.mapping;
  std::vector<uint8_t> embeddedFirmware;
  if (const size_t trailer = AppendedFirmwareSize(cart.header_, cart.rom_)) {
    if (const auto kind = SignalFirmware(cart.header_);
        kind && CartridgeFirmware::Size(*kind) == trailer)
      embeddedFirmware.assign(cart.rom_.end() - trailer, cart.rom_.end());
    cart.rom_.resize(cart.rom_.size() - trailer);
  }
  cart.romCrc32_ = ComputeCrc32(cart.rom_);
  cart.ApplyBiosCompatibility();
  cart.memselFast_ = false;

  if (cart.header_.chip == EnhancementChip::Sdd1)
    cart.header_.mapping = MappingType::Sdd1;
  if (cart.header_.chip == EnhancementChip::St010)
    cart.header_.mapping = MappingType::St010;
  if (cart.header_.chip == EnhancementChip::Sa1 &&
      cart.header_.mapping != MappingType::BroadcastSa1)
    cart.header_.mapping = MappingType::Sa1;
  if (cart.header_.chip == EnhancementChip::SuperFx)
    cart.header_.mapping = MappingType::SuperFx;
  if (cart.header_.chip == EnhancementChip::Spc7110 ||
      cart.header_.chip == EnhancementChip::Spc7110Rtc)
    cart.header_.mapping = MappingType::Spc7110;
  if (cart.header_.chip == EnhancementChip::St011)
    cart.header_.mapping = MappingType::St011;
  if (cart.rom_.size() == 0x40000 && HasSignature(cart.rom_, 0, "BANDAI SFC-ADX") &&
      HasSignature(cart.rom_, 0x10, "SFC-ADX BACKUP")) {
    cart.header_.mapping = MappingType::SufamiTurbo;
    cart.header_.chip = EnhancementChip::None;
  }

  std::optional<size_t> forcedSramSize;
  bool forcedMapping = false;
  if (database != nullptr) {
    if (const auto overrideEntry = database->Find(cart.romCrc32_); overrideEntry.has_value()) {
      if (overrideEntry->forceMapping.has_value()) {
        cart.header_.mapping = *overrideEntry->forceMapping;
        forcedMapping = true;
      }
      if (overrideEntry->forceSpeed.has_value()) {
        cart.header_.speed = *overrideEntry->forceSpeed;
      }
      if (overrideEntry->forceTitle.has_value()) {
        cart.header_.title = *overrideEntry->forceTitle;
      }
      if (overrideEntry->forceSramSize.has_value()) {
        forcedSramSize = *overrideEntry->forceSramSize;
      }
    }
  }

  auto boardHeader = cart.header_;
  if (cart.header_.mapping != MappingType::Bsx && cart.header_.mapping != MappingType::SufamiTurbo)
    boardHeader.mapping = parsedMapping;
  const auto profile =
      detail::ResolveRomBoardProfile(boardHeader, cart.rom_.size(), forcedSramSize, forcedMapping);
  cart.boardConnected_ = profile.connected;
  if (!profile.connected) {
    if (normalization)
      *normalization = {removedHeader, false};
    return cart;
  }

  if (cart.header_.chip == EnhancementChip::SuperFx ||
      cart.header_.mapping == MappingType::SuperFx) {
    cart.header_.chip = EnhancementChip::SuperFx;
    cart.header_.mapping = MappingType::SuperFx;
    // A separate expansion declaration supplies RAM when the ordinary field is empty.
    if ((cart.header_.sramSizeShift & 15) == 0)
      cart.header_.sramSizeShift =
          cart.header_.maker == 0x33 && (cart.header_.expansionRamSizeShift & 15)
              ? cart.header_.expansionRamSizeShift
              : 5;
    cart.sram_.assign(profile.ramBytes, 0xff);
    cart.superFx_ = std::make_unique<SuperFx>(cart.rom_, cart.sram_);
  } else if (cart.header_.chip == EnhancementChip::St011 ||
             cart.header_.mapping == MappingType::St011) {
    cart.header_.chip = EnhancementChip::St011;
    cart.header_.mapping = MappingType::St011;
    cart.header_.sramSizeShift = 2;
    cart.sram_.assign(0x1000, 0);
    cart.st011_ = std::make_unique<St011>(cart.sram_);
  } else if (cart.header_.chip == EnhancementChip::St018) {
    cart.sram_.assign(profile.ramBytes, 0xff);
    cart.st018_ = std::make_unique<St018>(cart.sram_);
  } else if (cart.header_.chip == EnhancementChip::St010 ||
             cart.header_.mapping == MappingType::St010) {
    cart.header_.chip = EnhancementChip::St010;
    cart.header_.mapping = MappingType::St010;
    cart.header_.sramSizeShift = 2;
    cart.sram_.assign(0x1000, 0);
  } else {
    cart.sram_.assign(profile.ramBytes, 0xff);
  }
  if (cart.header_.chip == EnhancementChip::Obc1) {
    cart.obc1_ = std::make_unique<Obc1>(std::span<uint8_t>(cart.sram_));
  }
  if (cart.header_.mapping == MappingType::Sdd1)
    cart.sdd1_ = std::make_unique<Sdd1>(cart.rom_, cart.sram_);
  if (cart.header_.chip == EnhancementChip::Cx4)
    cart.cx4_ = std::make_unique<Cx4>(cart.rom_);
  if (cart.header_.chip == EnhancementChip::Dsp3) {
    cart.dsp3_ = std::make_unique<Dsp3>();
    cart.dsp3_->Reset();
  }
  if (cart.header_.chip == EnhancementChip::Dsp4) {
    cart.dsp4_ = std::make_unique<Dsp4>();
    cart.dsp4_->Reset();
  }
  if (cart.header_.chip == EnhancementChip::Sa1 || cart.header_.mapping == MappingType::Sa1) {
    cart.header_.chip = EnhancementChip::Sa1;
    if (cart.header_.mapping != MappingType::BroadcastSa1)
      cart.header_.mapping = MappingType::Sa1;
    cart.sa1_ = std::make_unique<Sa1>(cart.rom_, cart.sram_);
  }
  if (cart.header_.chip == EnhancementChip::Spc7110 ||
      cart.header_.chip == EnhancementChip::Spc7110Rtc ||
      cart.header_.mapping == MappingType::Spc7110) {
    cart.header_.mapping = MappingType::Spc7110;
    if (cart.header_.chip != EnhancementChip::Spc7110Rtc)
      cart.header_.chip = EnhancementChip::Spc7110;
    cart.spc7110_ = std::make_unique<Spc7110>(cart.rom_, cart.sram_,
                                              cart.header_.chip == EnhancementChip::Spc7110Rtc);
  }
  if (cart.header_.mapping == MappingType::Bsx)
    cart.InitializeBroadcast();
  if (cart.header_.chip == EnhancementChip::SuperGameBoy) {
    cart.sram_.clear();
    cart.gameBoy_ = std::make_unique<gameboy::Adapter>(cart.header_.title == "Super GAMEBOY2");
  }

  const auto firmwareKind = SignalFirmware(cart.header_);
  const auto firmwareBytes = !embeddedFirmware.empty()  ? std::span<const uint8_t>(embeddedFirmware)
                             : firmwareKind && firmware ? firmware->Find(*firmwareKind)
                                                        : std::span<const uint8_t>{};
  if (firmwareBytes.empty() &&
      (firmwareKind == FirmwareKind::Dsp3 || firmwareKind == FirmwareKind::St011 ||
       firmwareKind == FirmwareKind::St018)) {
    if (error)
      *error = std::string("Missing cartridge firmware for ") + ChipName(cart.header_.chip) + ": " +
               CartridgeFirmware::FileName(*firmwareKind);
    return std::nullopt;
  }
  const bool seta =
      cart.header_.chip == EnhancementChip::St010 || cart.header_.chip == EnhancementChip::St011;
  const auto board = seta                                         ? NecDsp::Board::Seta
                     : cart.header_.mapping == MappingType::HiRom ? NecDsp::Board::HiRom
                     : cart.sram_.empty()                         ? NecDsp::Board::LoRom
                     : cart.rom_.size() <= 0x100000               ? NecDsp::Board::LoRomSmallRam
                                                                  : NecDsp::Board::LoRomLargeRam;
  if (cart.header_.chip == EnhancementChip::Dsp1 || cart.header_.chip == EnhancementChip::Dsp2 ||
      cart.header_.chip == EnhancementChip::Dsp3 || cart.header_.chip == EnhancementChip::Dsp4 ||
      (seta && !firmwareBytes.empty()))
    cart.signalBoard_.emplace(board);
  if (firmwareKind == FirmwareKind::Cx4 && !firmwareBytes.empty()) {
    cart.cx4Processor_ = std::make_unique<Cx4Processor>(cart.rom_, cart.sram_, firmwareBytes);
    cart.cx4_.reset();
  } else if (firmwareKind == FirmwareKind::St018 && !firmwareBytes.empty()) {
    cart.armDsp_ = std::make_unique<St018Processor>(firmwareBytes);
    cart.st018_.reset();
  } else if (firmwareKind && !firmwareBytes.empty()) {
    cart.necDsp_ = std::make_unique<NecDsp>(*firmwareKind, board);
    if (!cart.necDsp_->LoadFirmware(firmwareBytes)) {
      if (error)
        *error = "Invalid cartridge signal processor firmware";
      return std::nullopt;
    }
    cart.dsp3_.reset();
    cart.dsp4_.reset();
    cart.st011_.reset();
    if (seta)
      cart.sram_.clear();
  }

  if (normalization != nullptr) {
    normalization->hadCopierHeader = removedHeader;
    normalization->hadInterleave = deinterleaved;
  }

  return cart;
}

std::optional<Cartridge> Cartridge::FromSuperGameBoy(std::span<const uint8_t> bios,
                                                     std::span<const uint8_t> game,
                                                     std::span<const uint8_t> boot,
                                                     std::string *error) {
  auto cartridge = FromRomImage(bios, nullptr, nullptr, error);
  if (!cartridge)
    return std::nullopt;
  if (!cartridge->gameBoy_) {
    if (error)
      *error = "The base image is not a Super Game Boy BIOS";
    return std::nullopt;
  }
  if (!cartridge->gameBoy_->Load(game, boot, error))
    return std::nullopt;
  return cartridge;
}

std::optional<Cartridge> Cartridge::FromSufamiTurbo(std::span<const uint8_t> bios,
                                                    std::span<const uint8_t> slotA,
                                                    std::span<const uint8_t> slotB,
                                                    std::string *error) {
  if (error)
    error->clear();
  const auto normalizedBios = RemoveCopierHeader(bios, nullptr);
  const std::array slots{RemoveCopierHeader(slotA, nullptr), RemoveCopierHeader(slotB, nullptr)};
  if (normalizedBios.size() != 0x40000 || !HasSignature(normalizedBios, 0, "BANDAI SFC-ADX") ||
      !HasSignature(normalizedBios, 0x10, "SFC-ADX BACKUP")) {
    if (error)
      *error = "Invalid Sufami Turbo BIOS (expected a 256 KiB base cartridge)";
    return std::nullopt;
  }
  for (const auto &slot : slots) {
    if (!slot.empty() &&
        (slot.size() < 0x80000 || slot.size() > 0x100000 ||
         !HasSignature(slot, 0, "BANDAI SFC-ADX") || HasSignature(slot, 0x10, "SFC-ADX BACKUP"))) {
      if (error)
        *error = "Invalid Sufami Turbo slot image (expected a 512 KiB to 1 MiB game)";
      return std::nullopt;
    }
  }
  auto cart = FromRomImage(normalizedBios, nullptr, nullptr, error);
  if (!cart)
    return std::nullopt;
  cart->header_.mapping = MappingType::SufamiTurbo;
  cart->header_.chip = EnhancementChip::None;
  // A stable 16 KiB region per slot keeps saves independent of slot occupancy.
  cart->sram_.assign(0x8000, 0xff);
  for (unsigned i = 0; i < 2; ++i) {
    cart->slotOffset_[i] = cart->rom_.size();
    cart->slotSize_[i] = slots[i].size();
    cart->rom_.insert(cart->rom_.end(), slots[i].begin(), slots[i].end());
  }
  cart->romCrc32_ = ComputeCrc32(cart->rom_);
  return cart;
}

const RomHeader &Cartridge::Header() const noexcept {
  return header_;
}

uint32_t Cartridge::RomCrc32() const noexcept {
  return romCrc32_;
}

void Cartridge::InitializePowerOnMemory(PowerOnState &state) {
  if (armDsp_)
    armDsp_->Power(state);
}

void Cartridge::InitializeGameBoyStartup(std::shared_ptr<gameboy::StartupState> state) {
  if (gameBoy_)
    gameBoy_->InitializeStartup(std::move(state));
}

void Cartridge::SetSa1BusContention(bool enabled) {
  if (sa1_)
    sa1_->SetBusContention(enabled);
}

void Cartridge::SetCx4DelayedSync(bool enabled) {
  if (cx4Processor_)
    cx4Processor_->SetDelayedSync(enabled);
}

void Cartridge::Reset() {
  if (gameBoy_)
    gameBoy_->Reset();
  if (competition_)
    competition_->Reset();
  if (necDsp_)
    necDsp_->Reset();
  memselFast_ = false;
  if (memoryPackDevice_)
    memoryPackDevice_->Reset();
  if (sdd1_)
    sdd1_->Reset();
  st010_ = St010{};
  if (header_.mapping == MappingType::St010 && !necDsp_)
    std::fill(sram_.begin(), sram_.end(), 0);
  if (spc7110_)
    spc7110_->Reset();
  if (sa1_)
    sa1_->Reset();
  if (superFx_)
    superFx_->Reset();
  if (cx4_)
    cx4_->Reset();
  if (cx4Processor_)
    cx4Processor_->Reset();
  if (st011_)
    st011_->Reset();
  if (st018_)
    st018_->Reset();
  if (armDsp_)
    armDsp_->Reset();
  if (bsx_)
    bsx_->Reset();
  if (dsp3_)
    dsp3_->Reset();
  if (dsp4_)
    dsp4_->Reset();
  if (obc1_)
    obc1_->Reset();
}

void Cartridge::SetMemselFast(bool enabled) noexcept {
  memselFast_ = enabled;
}

bool Cartridge::MemselFast() const noexcept {
  return memselFast_;
}

uint8_t Cartridge::Read(uint32_t cpuAddress, uint8_t openBus) const {
  if (!boardConnected_)
    return openBus;
  if (gameBoy_ && gameboy::Adapter::Selects(cpuAddress))
    return gameBoy_->Read(cpuAddress);
  if (competition_)
    return competition_->Read(cpuAddress, openBus);
  if (cx4Processor_)
    return cx4Processor_->Read(cpuAddress, openBus);
  if (sdd1_)
    return sdd1_->Read(cpuAddress, openBus);
  if (signalBoard_) {
    if (signalBoard_->SelectsDevice(cpuAddress)) {
      if (necDsp_)
        return necDsp_->Read(cpuAddress);
      const uint16_t port = signalBoard_->StatusPort(cpuAddress) ? 0xc000 : 0x8000;
      if (dsp3_)
        return dsp3_->Read(port);
      if (dsp4_)
        return dsp4_->Read(port);
      return openBus;
    }
    if (const auto address = signalBoard_->SramAddress(cpuAddress); address && !sram_.empty())
      return sram_[MirrorOffset(*address, sram_.size())];
    if (const auto address = signalBoard_->RomAddress(cpuAddress); address && !rom_.empty())
      return rom_[MirrorOffset(*address, rom_.size())];
    return openBus;
  }
  if (obc1_) {
    if (Obc1::Selects(cpuAddress))
      return obc1_->Read(static_cast<uint16_t>(cpuAddress));
    if ((cpuAddress & 0x408000) != 0x008000)
      return openBus;
  }
  if (bsx_)
    return bsx_->ReadCpu(cpuAddress, openBus);
  if (st011_ && St010::Selects(cpuAddress))
    return st011_->Read(cpuAddress);
  if (armDsp_ && St018Processor::Selects(cpuAddress))
    return armDsp_->Read(cpuAddress);
  if (st018_ && (cpuAddress & 0x40f000) == 0x003000)
    return st018_->Read(cpuAddress);
  if (cx4_ && Cx4::Selects(cpuAddress))
    return cx4_->Read(cpuAddress);
  if (sa1_)
    return sa1_->ReadCpu(cpuAddress, openBus);
  if (superFx_)
    return superFx_->ReadCpu(cpuAddress, openBus);
  if (spc7110_)
    return spc7110_->Read(cpuAddress, openBus);
  if (header_.mapping == MappingType::St010 && St010::Selects(cpuAddress))
    return st010_.Read(cpuAddress, sram_);
  if (const auto offset = ResolveMemoryPackOffset(cpuAddress))
    return ReadMemoryPack(cpuAddress, *offset);
  if (const auto sramOffset = ResolveSramOffset(cpuAddress);
      sramOffset.has_value() && !sram_.empty()) {
    return sram_[*sramOffset];
  }

  if (const auto romOffset = ResolveRomOffset(cpuAddress); romOffset.has_value() && !rom_.empty()) {
    return rom_[*romOffset];
  }

  return openBus;
}

void Cartridge::Write(uint32_t cpuAddress, uint8_t value) {
  if (!boardConnected_)
    return;
  if (gameBoy_ && gameboy::Adapter::Selects(cpuAddress)) {
    gameBoy_->Write(cpuAddress, value);
    return;
  }
  if (competition_) {
    competition_->Write(cpuAddress, value);
    return;
  }
  if (cx4Processor_) {
    cx4Processor_->Write(cpuAddress, value);
    return;
  }
  if (sdd1_) {
    sdd1_->Write(cpuAddress, value);
    return;
  }
  if (signalBoard_) {
    if (signalBoard_->SelectsDevice(cpuAddress)) {
      if (necDsp_)
        necDsp_->Write(cpuAddress, value);
      else if (!signalBoard_->StatusPort(cpuAddress)) {
        if (dsp3_)
          dsp3_->Write(0x8000, value);
        if (dsp4_)
          dsp4_->Write(0x8000, value);
      }
      return;
    }
    if (const auto address = signalBoard_->SramAddress(cpuAddress); address && !sram_.empty())
      sram_[MirrorOffset(*address, sram_.size())] = value;
    return;
  }
  if (obc1_) {
    if (Obc1::Selects(cpuAddress))
      obc1_->Write(static_cast<uint16_t>(cpuAddress), value);
    return;
  }
  if (bsx_) {
    bsx_->WriteCpu(cpuAddress, value);
    return;
  }
  if (st011_ && St010::Selects(cpuAddress)) {
    st011_->Write(cpuAddress, value);
    return;
  }
  if (armDsp_ && St018Processor::Selects(cpuAddress)) {
    armDsp_->Write(cpuAddress, value);
    return;
  }
  if (st018_ && (cpuAddress & 0x40f000) == 0x003000) {
    st018_->Write(cpuAddress, value);
    return;
  }
  if (cx4_ && Cx4::Selects(cpuAddress)) {
    cx4_->Write(cpuAddress, value);
    return;
  }
  if (sa1_) {
    sa1_->WriteCpu(cpuAddress, value);
    return;
  }
  if (superFx_) {
    superFx_->WriteCpu(cpuAddress, value);
    return;
  }
  if (spc7110_) {
    spc7110_->Write(cpuAddress, value);
    return;
  }
  if (header_.mapping == MappingType::St010 && St010::Selects(cpuAddress)) {
    st010_.Write(cpuAddress, value, sram_);
    return;
  }
  if (const auto offset = ResolveMemoryPackOffset(cpuAddress)) {
    WriteMemoryPack(cpuAddress, *offset, value);
    return;
  }
  if (const auto sramOffset = ResolveSramOffset(cpuAddress);
      sramOffset.has_value() && !sram_.empty()) {
    sram_[*sramOffset] = value;
  }
}

uint32_t Cartridge::AccessCycles(uint32_t cpuAddress) const noexcept {
  return (memselFast_ && IsFastRegion(cpuAddress)) ? 6u : 8u;
}

void Cartridge::AdvanceHardware(uint32_t masterClocks, uint32_t cpuAddress, bool cpuRefreshing,
                                bool synchronizeProcessors) {
  if (gameBoy_)
    gameBoy_->Advance(masterClocks, synchronizeProcessors);
  if (bsx_)
    bsx_->Advance(masterClocks);
  if (memoryPackDevice_)
    memoryPackDevice_->Advance(masterClocks);
  if (competition_)
    competition_->Advance(masterClocks);
  if (spc7110_)
    spc7110_->Advance(masterClocks);
  if (cx4Processor_)
    cx4Processor_->Advance(masterClocks, synchronizeProcessors);
  if (armDsp_)
    armDsp_->Advance(masterClocks);
  if (necDsp_)
    necDsp_->Advance(masterClocks);
  if (sa1_)
    sa1_->Advance(masterClocks, cpuAddress, cpuRefreshing, synchronizeProcessors);
  if (superFx_)
    superFx_->AdvanceMasterClocks(masterClocks);
}

void Cartridge::SetPal(bool pal) {
  if (gameBoy_)
    gameBoy_->SetPal(pal);
  if (bsx_)
    bsx_->SetPal(pal);
  if (memoryPackDevice_)
    memoryPackDevice_->SetPal(pal);
  if (competition_)
    competition_->SetPal(pal);
  if (spc7110_)
    spc7110_->SetPal(pal);
  if (cx4Processor_)
    cx4Processor_->SetPal(pal);
  if (armDsp_)
    armDsp_->SetPal(pal);
  if (necDsp_)
    necDsp_->SetPal(pal);
  if (superFx_)
    superFx_->SetPal(pal);
  if (sa1_)
    sa1_->SetPal(pal);
}

bool Cartridge::CpuIrqPending() const noexcept {
  return (cx4Processor_ && cx4Processor_->CpuIrqPending()) || (sa1_ && sa1_->CpuIrqPending()) ||
         (superFx_ && superFx_->CpuIrqPending()) || (bsx_ && bsx_->CpuIrqPending());
}

void Cartridge::ObserveDmaWrite(uint32_t address, uint8_t value) {
  if (sdd1_)
    sdd1_->ObserveDmaWrite(address, value);
}

std::span<const uint8_t> Cartridge::RomData() const noexcept {
  if (competition_)
    return competition_->BootRom();
  return rom_;
}

std::span<const uint8_t> Cartridge::SramData() const noexcept {
  if (competition_)
    return competition_->SaveRam();
  if (necDsp_ && necDsp_->HasSaveRam())
    return necDsp_->SaveRam();
  return sram_;
}

void Cartridge::LoadSram(std::span<const uint8_t> data) {
  if (competition_) {
    competition_->LoadRam(data);
    return;
  }
  if (necDsp_ && necDsp_->HasSaveRam()) {
    necDsp_->LoadRam(data);
    return;
  }
  if (sram_.empty()) {
    return;
  }

  const auto bytes = std::min(sram_.size(), data.size());
  std::copy_n(data.begin(), bytes, sram_.begin());
  if (obc1_)
    obc1_->Reset();
}

bool Cartridge::MapSignalProcessor(MemoryBus &bus, uint8_t slot) const {
  if (!signalBoard_)
    return false;
  signalBoard_->MapCpu(bus, slot);
  return true;
}

bool Cartridge::MapGameBoy(MemoryBus &bus, uint8_t slot) const {
  if (!gameBoy_)
    return false;
  gameboy::Adapter::MapCpu(bus, slot);
  return true;
}

void Cartridge::SetGameBoyAudioOutput(GameBoyAudioSink output) {
  if (gameBoy_)
    gameBoy_->SetAudioOutput(std::move(output));
}

void Cartridge::SetGameBoyMixedAudioOutput(std::function<void(float, float)> output) {
  if (gameBoy_)
    gameBoy_->SetMixedAudioOutput(std::move(output));
}

void Cartridge::WriteGameBoyConsoleAudio(int16_t left, int16_t right) {
  if (gameBoy_)
    gameBoy_->WriteConsoleAudio(left, right);
}

void Cartridge::SetGameBoyConsoleAudioRate(uint32_t rate) {
  if (gameBoy_)
    gameBoy_->SetConsoleAudioRate(rate);
}

void Cartridge::SetGameBoyExpansionAudioRate(uint32_t rate) {
  if (gameBoy_)
    gameBoy_->SetExpansionAudioRate(rate);
}

void Cartridge::WriteGameBoyExpansionAudio(float left, float right) {
  if (gameBoy_)
    gameBoy_->WriteExpansionAudio(left, right);
}

bool Cartridge::MapSpc7110(MemoryBus &bus, uint8_t slot) const {
  if (!spc7110_)
    return false;
  spc7110_->MapCpu(bus, slot);
  return true;
}

bool Cartridge::MapCx4Processor(MemoryBus &bus, uint8_t slot) const {
  if (!cx4Processor_)
    return false;
  cx4Processor_->MapCpu(bus, slot);
  return true;
}

std::span<const uint8_t> Cartridge::SlotSramData(unsigned slot) const noexcept {
  if (header_.mapping != MappingType::SufamiTurbo || slot >= 2 || slotSize_[slot] == 0)
    return {};
  return std::span<const uint8_t>(sram_).subspan(slot * 0x4000, 0x4000);
}

void Cartridge::LoadSlotSram(unsigned slot, std::span<const uint8_t> data) {
  const auto size = SlotSramData(slot).size();
  if (size)
    std::copy_n(data.begin(), std::min(data.size(), size), sram_.begin() + slot * 0x4000);
}

std::vector<uint8_t> Cartridge::SaveSlotBattery(unsigned slot) const {
  if (gameBoy_ && slot == 0)
    return gameBoy_->SaveBattery();
  const auto bytes = SlotSramData(slot);
  return {bytes.begin(), bytes.end()};
}

bool Cartridge::LoadSlotBattery(unsigned slot, std::span<const uint8_t> data) {
  if (gameBoy_ && slot == 0) {
    gameBoy_->LoadBattery(data);
    return true;
  }
  if (SlotSramData(slot).empty())
    return false;
  LoadSlotSram(slot, data);
  return true;
}

std::vector<uint8_t> Cartridge::RemoveCopierHeader(std::span<const uint8_t> rom, bool *removed) {
  if (removed != nullptr) {
    *removed = false;
  }

  if (rom.size() > 512 && (rom.size() % 0x8000) == 512) {
    if (removed != nullptr) {
      *removed = true;
    }

    return std::vector<uint8_t>(rom.begin() + 512, rom.end());
  }

  return std::vector<uint8_t>(rom.begin(), rom.end());
}

std::optional<size_t> Cartridge::ResolveRomOffset(uint32_t cpuAddress) const {
  if (rom_.empty()) {
    return std::nullopt;
  }

  const auto bank = static_cast<uint8_t>(cpuAddress >> 16);
  const auto addr = static_cast<uint16_t>(cpuAddress & 0xFFFF);

  if (SelectsLoRomSram(cpuAddress))
    return std::nullopt;

  switch (header_.mapping) {
  case MappingType::Spc7110:
    return std::nullopt;
  case MappingType::St011:
  case MappingType::St010:
    if (addr >= 0x8000 && bank != 0x7e && bank != 0x7f)
      return MirrorOffset((size_t(bank & 0x7f) << 15) | (addr & 0x7fff), rom_.size());
    return std::nullopt;
  case MappingType::Sdd1:
    return std::nullopt;
  case MappingType::BroadcastLoRom: {
    if ((bank & 0x40) || addr < 0x8000)
      return std::nullopt;
    const size_t chip = (bank & 0x20) ? 0x100000 : (bank & 0x80) ? 0x200000 : 0;
    return MirrorOffset(chip + (size_t(bank & 0x1f) << 15) + (addr & 0x7fff), rom_.size());
  }
  case MappingType::BroadcastHiRom:
    if (bank == 0x7e || bank == 0x7f || (!(bank & 0x40) && addr < 0x8000) || (bank & 0x20))
      return std::nullopt;
    return MirrorOffset((size_t(bank & 0x1f) << 16) | addr, rom_.size());
  case MappingType::SufamiTurbo: {
    if (addr < 0x8000 || (bank & 0x7f) >= 0x60)
      return std::nullopt;
    const unsigned window = (bank & 0x7f) / 0x20;
    const size_t linear = (size_t(bank & 0x1f) << 15) | (addr & 0x7fff);
    if (window == 0)
      return MirrorOffset(linear, std::min(rom_.size(), size_t(0x40000)));
    const unsigned slot = window - 1;
    if (slotSize_[slot] == 0)
      return std::nullopt;
    return slotOffset_[slot] + MirrorOffset(linear, slotSize_[slot]);
  }
  case MappingType::ExLoRom: {
    if (bank == 0x7e || bank == 0x7f || addr < 0x8000)
      return std::nullopt;
    const size_t linear = (static_cast<size_t>(bank & 0x7f) << 15) | (addr & 0x7fff);
    if (bank & 0x80)
      return MirrorOffset(linear, std::min(rom_.size(), size_t(0x400000)));
    if (rom_.size() <= 0x400000)
      return MirrorOffset(linear, rom_.size());
    return 0x400000 + MirrorOffset(linear, rom_.size() - 0x400000);
  }

  case MappingType::LoRom24Mbit: {
    if ((bank & 0x40) || addr < 0x8000)
      return std::nullopt;
    const size_t chip = (bank & 0x20) ? 0x100000 : (bank & 0x80) ? 0x200000 : 0;
    const size_t linear = chip + (static_cast<size_t>(bank & 0x1f) << 15) + (addr & 0x7fff);
    return MirrorOffset(linear, rom_.size());
  }

  case MappingType::LoRom: {
    const bool lowerArmRom = header_.chip == EnhancementChip::St018 &&
                             ((bank >= 0x40 && bank <= 0x6f) || (bank >= 0xc0 && bank <= 0xef));
    const bool lowerGameBoyRom = gameBoy_ && (bank & 0x40);
    if ((addr >= 0x8000 || lowerArmRom || lowerGameBoyRom) && bank != 0x7e && bank != 0x7f)
      return MirrorOffset((size_t(bank) << 15) | (addr & 0x7fff), rom_.size());
    return std::nullopt;
  }
  case MappingType::LoRomNoMad1:
  case MappingType::LoRomLargeSram: {
    // Mirror each 32 KiB page into both halves of the full-ROM banks.
    const bool fullRomBank = (bank >= 0x40 && bank <= 0x7D) || bank >= 0xC0;
    if (fullRomBank || (addr >= 0x8000 && bank != 0x7E && bank != 0x7F)) {
      const auto linear =
          (static_cast<size_t>(bank & 0x7F) << 15) | static_cast<size_t>(addr & 0x7FFF);
      return MirrorOffset(linear, rom_.size());
    }

    return std::nullopt;
  }

  case MappingType::HiRom: {
    if ((bank >= 0x40 && bank <= 0x7D) || bank >= 0xC0) {
      const auto linear = (static_cast<size_t>(bank) << 16) | addr;
      return MirrorOffset(linear, rom_.size());
    }

    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && addr >= 0x8000) {
      const auto linear = (static_cast<size_t>(bank) << 16) | addr;
      return MirrorOffset(linear, rom_.size());
    }

    return std::nullopt;
  }

  case MappingType::ExHiRom: {
    size_t linear = std::numeric_limits<size_t>::max();

    if (bank >= 0xC0) {
      linear = (static_cast<size_t>(bank - 0xC0) * 0x10000) + addr;
    } else if (bank >= 0x40 && bank <= 0x7D) {
      linear = 0x400000 + (static_cast<size_t>(bank - 0x40) * 0x10000) + addr;
    } else if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && addr >= 0x8000) {
      const size_t half = (bank & 0x80) ? 0 : 0x400000;
      linear = half + (static_cast<size_t>(bank & 0x3F) * 0x10000) + addr;
    }

    if (linear == std::numeric_limits<size_t>::max()) {
      return std::nullopt;
    }

    return MirrorOffset(linear, rom_.size());
  }

  case MappingType::Unknown:
  default:
    return std::nullopt;
  }
}

bool Cartridge::SelectsLoRomSram(uint32_t cpuAddress) const {
  if (header_.mapping == MappingType::LoRom || header_.mapping == MappingType::ExLoRom) {
    const unsigned bank = (cpuAddress >> 16) & 0xff;
    if (sram_.empty() || !((bank >= 0x70 && bank <= 0x7d) || bank >= 0xf0))
      return false;
    const bool fullBank = header_.mapping == MappingType::LoRom &&
                          (header_.chip == EnhancementChip::St018 || rom_.size() <= 0x200000);
    return (cpuAddress & 0xffff) < 0x8000 || fullBank;
  }
  switch (header_.mapping) {
  case MappingType::St011:
  case MappingType::St010:
  case MappingType::LoRom:
  case MappingType::ExLoRom:
  case MappingType::BroadcastLoRom:
  case MappingType::LoRomNoMad1:
  case MappingType::LoRom24Mbit:
    break;
  default:
    return false;
  }
  const unsigned bank = (cpuAddress >> 16) & 0xff;
  if (!((bank >= 0x70 && bank <= 0x7d) || (bank >= 0xf0 && !sram_.empty())))
    return false;
  if (header_.mapping == MappingType::BroadcastLoRom)
    return (cpuAddress & 0xffff) < 0x8000;
  return (cpuAddress & 0xffff) < 0x8000 || header_.chip == EnhancementChip::St018 ||
         header_.mapping == MappingType::LoRomNoMad1 ||
         (header_.romSizeShift <= 11 && sram_.size() <= 0x8000);
}

std::optional<size_t> Cartridge::ResolveSramOffset(uint32_t cpuAddress) const {
  if (sram_.empty()) {
    return std::nullopt;
  }

  const auto bank = static_cast<uint8_t>(cpuAddress >> 16);
  const auto addr = static_cast<uint16_t>(cpuAddress & 0xFFFF);

  switch (header_.mapping) {
  case MappingType::Spc7110:
    if ((bank == 0 || bank == 0x30) && addr >= 0x6000 && addr < 0x8000)
      return MirrorOffset(addr & 0x1fff, sram_.size());
    return std::nullopt;
  case MappingType::Sdd1:
    return std::nullopt;
  case MappingType::SufamiTurbo: {
    const unsigned mirroredBank = bank & 0x7f;
    if (addr < 0x8000 || !((mirroredBank >= 0x60 && mirroredBank <= 0x63) ||
                           (mirroredBank >= 0x70 && mirroredBank <= 0x73)))
      return std::nullopt;
    const unsigned slot = mirroredBank >= 0x70 ? 1 : 0;
    if (slotSize_[slot] == 0)
      return std::nullopt;
    return slot * 0x4000 + (addr & 0x3fff);
  }
  case MappingType::LoRomLargeSram:
    if (bank >= 0x70 && bank <= 0x73) {
      const size_t linear = static_cast<size_t>(bank - 0x70) * 0x8000 + addr;
      return MirrorOffset(linear, sram_.size());
    }
    return std::nullopt;
  case MappingType::ExLoRom:
  case MappingType::LoRomNoMad1:
  case MappingType::LoRom24Mbit:
  case MappingType::BroadcastLoRom:
  case MappingType::St011:
  case MappingType::St010:
  case MappingType::LoRom: {
    if (SelectsLoRomSram(cpuAddress)) {
      const auto linear = (static_cast<size_t>(bank & 0x0F) * 0x8000) + (addr & 0x7fff);
      return MirrorOffset(linear, sram_.size());
    }
    return std::nullopt;
  }

  case MappingType::HiRom:
  case MappingType::BroadcastHiRom:
  case MappingType::ExHiRom: {
    if (((bank >= 0x20 && bank <= 0x3F) || (bank >= 0xA0 && bank <= 0xBF)) && addr >= 0x6000 &&
        addr <= 0x7FFF) {
      const auto linear = (static_cast<size_t>(bank & 0x1F) * 0x2000) + (addr - 0x6000);
      return MirrorOffset(linear, sram_.size());
    }
    return std::nullopt;
  }

  case MappingType::Unknown:
  default:
    return std::nullopt;
  }
}

uint32_t Cartridge::ComputeCrc32(std::span<const uint8_t> data) {
  constexpr uint32_t polynomial = 0xEDB88320;

  uint32_t crc = 0xFFFFFFFF;
  for (auto byte : data) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
      crc = (crc >> 1) ^ ((crc & 1) ? polynomial : 0);
    }
  }

  return ~crc;
}

} // namespace snes::core
