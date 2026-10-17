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
  return board_.Kind() == Board::Seta && (address & 0x788000) == 0x680000;
}

bool NecDsp::SelectsDevice(uint32_t address) const noexcept {
  return board_.SelectsDevice(address);
}

std::optional<uint32_t> NecDsp::RomAddress(uint32_t address) const noexcept {
  return board_.RomAddress(address);
}

std::optional<uint32_t> NecDsp::SramAddress(uint32_t address) const noexcept {
  return board_.SramAddress(address);
}

uint8_t NecDsp::Read(uint32_t address) {
  if (SelectsDataRam(address))
    return processor_.ReadRam(static_cast<uint16_t>(address));
  return board_.StatusPort(address) ? processor_.ReadStatus() : processor_.ReadData();
}

void NecDsp::Write(uint32_t address, uint8_t value) {
  if (SelectsDataRam(address))
    processor_.WriteRam(static_cast<uint16_t>(address), value);
  else if (!board_.StatusPort(address))
    processor_.WriteData(value);
}

std::span<const uint8_t> NecDsp::SaveRam() const noexcept {
  if (board_.Kind() != Board::Seta)
    return {};
  for (size_t i = 0; i < savedRam_.size(); ++i)
    savedRam_[i] = processor_.ReadRam(static_cast<uint16_t>(i));
  return savedRam_;
}

void NecDsp::LoadRam(std::span<const uint8_t> bytes) {
  if (board_.Kind() == Board::Seta)
    for (size_t i = 0; i < std::min(bytes.size(), size_t{4096}); ++i)
      processor_.WriteRam(static_cast<uint16_t>(i), bytes[i]);
}

void NecDsp::MapCpu(MemoryBus &bus, uint8_t slot) const {
  board_.MapCpu(bus, slot);
}
} // namespace snes::core
