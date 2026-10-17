#include "snes/core/CompetitionBoard.hpp"
#include "snes/core/MemoryBus.hpp"
#include "snes/core/Timing.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace snes::core {

CompetitionBoard::CompetitionBoard(Settings settings, Images images)
    : settings_(settings), images_(std::move(images)), save_(settings.saveRamSize, 0xff),
      commandDsp_(std::make_unique<Dsp1>()), masterFrequency_(Timing::kMasterClockHz) {}

bool CompetitionBoard::LoadFirmware(std::span<const uint8_t> bytes) {
  auto processor = std::make_unique<UpdDsp>();
  if (!processor->LoadFirmware(bytes))
    return false;
  processor_ = std::move(processor);
  commandDsp_.reset();
  dspBalance_ = 0;
  return true;
}

void CompetitionBoard::Reset() {
  timerBalance_ = dspBalance_ = 0;
  gameSeconds_ = scoreSeconds_ = 0;
  status_ = select_ = 0;
  if (processor_)
    processor_->Reset();
  else
    commandDsp_->Reset();
}

void CompetitionBoard::SetPal(bool pal) {
  masterFrequency_ = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
}

void CompetitionBoard::TickSeconds(uint64_t seconds) {
  scoreSeconds_ -= unsigned(std::min<uint64_t>(seconds, scoreSeconds_));
  if (!gameSeconds_)
    return;
  if (seconds < gameSeconds_) {
    gameSeconds_ -= unsigned(seconds);
    return;
  }
  const uint64_t afterExpiration = seconds - gameSeconds_;
  gameSeconds_ = 0;
  status_ |= 2;
  scoreSeconds_ = unsigned(5 - std::min<uint64_t>(afterExpiration, 5));
}

void CompetitionBoard::Advance(uint32_t masterClocks) {
  timerBalance_ -= masterClocks;
  if (timerBalance_ < 0) {
    const uint64_t seconds = (uint64_t(-timerBalance_) + masterFrequency_ - 1) / masterFrequency_;
    timerBalance_ += int64_t(seconds * masterFrequency_);
    TickSeconds(seconds);
  }
  if (processor_) {
    dspBalance_ -= int64_t(masterClocks) * 7'600'000;
    while (dspBalance_ < 0) {
      processor_->Step();
      dspBalance_ += masterFrequency_;
    }
  }
}

size_t CompetitionBoard::Mirror(size_t address, size_t size) {
  size_t base = 0;
  while (address >= size) {
    const size_t bit = std::bit_floor(address);
    address -= bit;
    if (size > bit) {
      base += bit;
      size -= bit;
    }
  }
  return base + address;
}

bool CompetitionBoard::SelectsDsp(uint32_t address) const {
  const unsigned bank = (address >> 16) & 0x7f, offset = address & 0xffff;
  if (settings_.board == Board::CampusChallenge92)
    return bank >= 0x20 && bank <= 0x3f && offset >= 0x8000;
  return bank <= 0x0f && offset >= 0x6000 && offset <= 0x7fff;
}

bool CompetitionBoard::SelectsDspStatus(uint32_t address) const {
  return settings_.board == Board::CampusChallenge92 || (address & 0x1000);
}

std::optional<size_t> CompetitionBoard::SaveAddress(uint32_t address) const {
  if (save_.empty())
    return std::nullopt;
  const unsigned bank = address >> 16, offset = address & 0xffff;
  if (settings_.board == Board::CampusChallenge92) {
    if (offset < 0x8000 && ((bank >= 0x70 && bank <= 0x7d) || bank >= 0xf0))
      return Mirror((size_t(bank) << 15) | offset, save_.size());
  } else if ((bank & 0x7f) >= 0x30 && (bank & 0x7f) <= 0x3f && offset >= 0x6000 &&
             offset <= 0x7fff) {
    return Mirror((size_t(bank) << 13) | (offset & 0x1fff), save_.size());
  }
  return std::nullopt;
}

unsigned CompetitionBoard::SelectedRom() const {
  if (select_ == 9)
    return 1;
  if (select_ == (settings_.board == Board::CampusChallenge92 ? 5 : 12))
    return 2;
  if (select_ == (settings_.board == Board::CampusChallenge92 ? 3 : 10))
    return 3;
  return 0;
}

std::optional<std::pair<unsigned, size_t>> CompetitionBoard::RomAddress(uint32_t address) const {
  const unsigned bank = address >> 16, offset = address & 0xffff;
  unsigned chip = SelectedRom();
  size_t location;
  if (settings_.board == Board::CampusChallenge92) {
    if ((bank & 0x7f) > 0x1f || offset < 0x8000)
      return std::nullopt;
    if (bank & 0x80)
      chip = 0;
    location = (size_t(bank & 0x7f) << 15) | (offset & 0x7fff);
  } else {
    if (bank < 0xc0 && ((bank & 0x7f) > 0x3f || offset < 0x8000))
      return std::nullopt;
    if ((bank & 0x20) && (offset & 0x8000))
      chip = 0;
    if (bank >= 0xc0)
      location = address & 0x3fffff;
    else if (chip == 2)
      location = address & 0x1fffff;
    else
      location = (size_t(bank & 0x1f) << 15) | (offset & 0x7fff);
  }
  if (images_[chip].empty())
    return std::nullopt;
  return std::pair{chip, Mirror(location, images_[chip].size())};
}

uint8_t CompetitionBoard::Read(uint32_t address, uint8_t openBus) {
  address &= 0xffffff;
  if (SelectsDsp(address)) {
    const bool status = SelectsDspStatus(address);
    if (processor_)
      return status ? processor_->ReadStatus() : processor_->ReadData();
    return status ? commandDsp_->GetSr() : commandDsp_->GetDr();
  }
  const uint32_t statusAddress = settings_.board == Board::CampusChallenge92 ? 0xc00000 : 0x106000;
  if (address == statusAddress)
    return status_;
  if (const auto offset = SaveAddress(address))
    return save_[*offset];
  if (const auto rom = RomAddress(address))
    return images_[rom->first][rom->second];
  return openBus;
}

void CompetitionBoard::Write(uint32_t address, uint8_t value) {
  address &= 0xffffff;
  if (SelectsDsp(address)) {
    if (!SelectsDspStatus(address)) {
      if (processor_)
        processor_->WriteData(value);
      else
        commandDsp_->SetDr(value);
    }
    return;
  }
  const uint32_t selectAddress = settings_.board == Board::CampusChallenge92 ? 0xe00000 : 0x206000;
  if (address == selectAddress) {
    select_ = value;
    if (value == 9)
      gameSeconds_ = (3 + (settings_.dipSwitches & 15)) * 60;
  } else if (const auto offset = SaveAddress(address)) {
    save_[*offset] = value;
  }
}

void CompetitionBoard::MapCpu(MemoryBus &bus, uint8_t slot) const {
  const auto mirrors = [&](uint8_t first, uint8_t last, uint16_t low, uint16_t high) {
    bus.MapRange(first, last, low, high, slot);
    bus.MapRange(first | 0x80, last | 0x80, low, high, slot);
  };
  if (settings_.board == Board::CampusChallenge92) {
    mirrors(0, 0x3f, 0x8000, 0xffff);
    bus.MapRange(0x70, 0x7d, 0, 0x7fff, slot);
    bus.MapRange(0xf0, 0xff, 0, 0x7fff, slot);
    bus.MapRange(0xc0, 0xc0, 0, 0, slot);
    bus.MapRange(0xe0, 0xe0, 0, 0, slot);
  } else {
    mirrors(0, 0x3f, 0x8000, 0xffff);
    mirrors(0, 0x0f, 0x6000, 0x7fff);
    mirrors(0x30, 0x3f, 0x6000, 0x7fff);
    bus.MapRange(0xc0, 0xff, 0, 0xffff, slot);
    bus.MapRange(0x10, 0x10, 0x6000, 0x6000, slot);
    bus.MapRange(0x20, 0x20, 0x6000, 0x6000, slot);
  }
}

std::span<const uint8_t> CompetitionBoard::BootRom() const noexcept {
  return images_[0];
}

std::span<const uint8_t> CompetitionBoard::SaveRam() const noexcept {
  return save_;
}

void CompetitionBoard::LoadRam(std::span<const uint8_t> bytes) {
  std::copy_n(bytes.begin(), std::min(bytes.size(), save_.size()), save_.begin());
}
} // namespace snes::core
