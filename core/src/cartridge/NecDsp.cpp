#include "snes/core/NecDsp.hpp"
#include "snes/core/MemoryBus.hpp"
#include "snes/core/Timing.hpp"

#include <algorithm>

namespace snes::core {
NecDsp::NecDsp(FirmwareKind firmware, Board board)
    : processor_(firmware == FirmwareKind::St010 || firmware == FirmwareKind::St011
                     ? UpdDsp::Revision::Upd96050
                     : UpdDsp::Revision::Upd7725),
      board_(board), frequency_(firmware == FirmwareKind::St010   ? 11'000'000
                                : firmware == FirmwareKind::St011 ? 15'000'000
                                                                  : 7'600'000),
      masterFrequency_(Timing::kMasterClockHz) {}

bool NecDsp::LoadFirmware(std::span<const uint8_t> bytes) {
  if (!processor_.LoadFirmware(bytes))
    return false;
  clockBalance_ = 0;
  return true;
}

void NecDsp::Reset() {
  processor_.Reset();
  clockBalance_ = 0;
}

void NecDsp::SetPal(bool pal) {
  masterFrequency_ = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
}

void NecDsp::Advance(uint32_t masterClocks) {
  clockBalance_ -= int64_t(masterClocks) * frequency_;
  while (clockBalance_ < 0) {
    processor_.Step();
    clockBalance_ += masterFrequency_;
  }
}

bool NecDsp::SelectsDataRam(uint32_t address) const noexcept {
  return board_ == Board::Seta && (address & 0x788000) == 0x680000;
}

bool NecDsp::SelectsDevice(uint32_t address) const noexcept {
  const unsigned bank = (address >> 16) & 0x7f, offset = address & 0xffff;
  switch (board_) {
  case Board::LoRom:
    return bank >= 0x30 && bank <= 0x3f && offset >= 0x8000;
  case Board::LoRomSmallRam:
    return bank >= 0x20 && bank <= 0x3f && offset >= 0x8000;
  case Board::LoRomLargeRam:
    return bank >= 0x60 && bank <= 0x6f && offset < 0x8000;
  case Board::HiRom:
    return bank <= 0x1f && offset >= 0x6000 && offset <= 0x7fff;
  case Board::Seta:
    return SelectsDataRam(address) || (bank >= 0x60 && bank <= 0x67 && offset < 0x4000);
  }
  return false;
}

bool NecDsp::SelectsRom(uint32_t address) const noexcept {
  const unsigned bank = (address >> 16) & 0x7f, offset = address & 0xffff;
  if ((address >> 16) == 0x7e || (address >> 16) == 0x7f)
    return false;
  switch (board_) {
  case Board::LoRom:
  case Board::LoRomSmallRam:
    return bank <= 0x1f && offset >= 0x8000;
  case Board::LoRomLargeRam:
    return bank <= 0x3f && offset >= 0x8000;
  case Board::HiRom:
    return bank >= 0x40 || offset >= 0x8000;
  case Board::Seta:
    return offset >= 0x8000;
  }
  return false;
}

std::optional<uint32_t> NecDsp::RomAddress(uint32_t address) const noexcept {
  if (!SelectsRom(address))
    return std::nullopt;
  if (board_ == Board::HiRom)
    return address;
  return ((address & 0xff0000) >> 1) | (address & 0x7fff);
}

std::optional<uint32_t> NecDsp::SramAddress(uint32_t address) const noexcept {
  const unsigned bank = address >> 16, offset = address & 0xffff;
  if (board_ == Board::HiRom) {
    if ((bank & 0x7f) >= 0x20 && (bank & 0x7f) <= 0x3f && offset >= 0x6000 && offset < 0x8000)
      return ((bank & 0x1f) << 13) | (offset & 0x1fff);
    return std::nullopt;
  }
  if (board_ != Board::LoRomSmallRam && board_ != Board::LoRomLargeRam)
    return std::nullopt;
  if (!((bank >= 0x70 && bank <= 0x7d) || bank >= 0xf0))
    return std::nullopt;
  if (board_ == Board::LoRomSmallRam)
    return ((bank & 15) << 16) | offset;
  if (offset < 0x8000)
    return ((bank & 15) << 15) | offset;
  return std::nullopt;
}

bool NecDsp::StatusPort(uint32_t address) const noexcept {
  if (board_ == Board::Seta)
    return address & 1;
  return address & (board_ == Board::HiRom ? 0x1000 : 0x4000);
}

uint8_t NecDsp::Read(uint32_t address) {
  if (SelectsDataRam(address))
    return processor_.ReadRam(static_cast<uint16_t>(address));
  return StatusPort(address) ? processor_.ReadStatus() : processor_.ReadData();
}

void NecDsp::Write(uint32_t address, uint8_t value) {
  if (SelectsDataRam(address))
    processor_.WriteRam(static_cast<uint16_t>(address), value);
  else if (!StatusPort(address))
    processor_.WriteData(value);
}

std::span<const uint8_t> NecDsp::SaveRam() const noexcept {
  if (board_ != Board::Seta)
    return {};
  for (size_t i = 0; i < savedRam_.size(); ++i)
    savedRam_[i] = processor_.ReadRam(static_cast<uint16_t>(i));
  return savedRam_;
}

void NecDsp::LoadRam(std::span<const uint8_t> bytes) {
  if (board_ == Board::Seta)
    for (size_t i = 0; i < std::min(bytes.size(), size_t{4096}); ++i)
      processor_.WriteRam(static_cast<uint16_t>(i), bytes[i]);
}

void NecDsp::MapCpu(MemoryBus &bus, uint8_t slot) const {
  const auto map = [&](uint8_t first, uint8_t last, uint16_t low, uint16_t high) {
    bus.MapRange(first, last, low, high, slot);
    bus.MapRange(first | 0x80, last | 0x80, low, high, slot);
  };
  if (board_ == Board::Seta) {
    map(0, 0x7d, 0x8000, 0xffff);
    bus.MapRange(0xfe, 0xff, 0x8000, 0xffff, slot);
    map(0x60, 0x67, 0, 0x3fff);
    map(0x68, 0x6f, 0, 0x7fff);
  } else if (board_ == Board::HiRom) {
    map(0, 0x3f, 0x8000, 0xffff);
    map(0x40, 0x7d, 0, 0xffff);
    bus.MapRange(0xfe, 0xff, 0, 0xffff, slot);
    map(0, 0x1f, 0x6000, 0x7fff);
    map(0x20, 0x3f, 0x6000, 0x7fff);
  } else {
    map(0, board_ == Board::LoRomLargeRam ? 0x3f : 0x1f, 0x8000, 0xffff);
    if (board_ == Board::LoRom)
      map(0x30, 0x3f, 0x8000, 0xffff);
    else if (board_ == Board::LoRomSmallRam)
      map(0x20, 0x3f, 0x8000, 0xffff);
    else
      map(0x60, 0x6f, 0, 0x7fff);
    if (board_ != Board::LoRom) {
      bus.MapRange(0x70, 0x7d, 0, board_ == Board::LoRomSmallRam ? 0xffff : 0x7fff, slot);
      bus.MapRange(0xf0, 0xff, 0, board_ == Board::LoRomSmallRam ? 0xffff : 0x7fff, slot);
    }
  }
}
} // namespace snes::core
