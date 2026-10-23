#include "snes/core/Spc7110.hpp"
#include "snes/core/EpsonRtc.hpp"
#include "snes/core/MemoryBus.hpp"
#include "snes/core/Timing.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <utility>

namespace snes::core {
Spc7110::Spc7110(std::span<const uint8_t> rom, bool rtc, Clock clock)
    : Spc7110(rom, {}, rtc, std::move(clock)) {}
Spc7110::Spc7110(std::span<const uint8_t> rom, std::span<uint8_t> save, bool rtc, Clock clock)
    : rom_(rom), save_(save), decoder_([this](uint32_t address) { return Data(address); }) {
  if (rom.empty())
    throw std::invalid_argument("SPC7110 image has no program ROM");
  const size_t dataStart = std::min<size_t>(rom.size(), 0x100000);
  data_ = rom.subspan(dataStart, rom.size() == 0x700000 ? 0x500000 : rom.size() - dataStart);
  if (rtc)
    rtc_ = std::make_unique<EpsonRtc>(std::move(clock));
  Reset();
}
Spc7110::~Spc7110() = default;
void Spc7110::Reset() {
  phase_ = Phase::Begin;
  registers_ = {};
  registers_[0x32] = 1;
  registers_[0x33] = 2;
  mode_ = 0;
  streamAddress_ = 0;
  balance_ = 0;
  decodePending_ = multiplyPending_ = dividePending_ = false;
  if (rtc_)
    rtc_->Reset();
}
void Spc7110::SetPal(bool pal) {
  masterFrequency_ = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
  if (rtc_)
    rtc_->SetPal(pal);
}
void Spc7110::Advance(uint32_t masterClocks) {
  if (rtc_)
    rtc_->Advance(masterClocks);
  balance_ -= int64_t(masterClocks) * 21'477'272;
  if (balance_ < 0 && (phase_ == Phase::Begin || phase_ == Phase::Idle) && !decodePending_ &&
      !multiplyPending_ && !dividePending_) {
    balance_ += ((-balance_ + masterFrequency_ - 1) / masterFrequency_) * masterFrequency_;
    return;
  }
  Synchronize();
}
void Spc7110::Synchronize() {
  while (balance_ < 0) {
    switch (phase_) {
    case Phase::Begin:
      if (decodePending_) {
        decodePending_ = false;
        if (mode_ != 3) {
          Cycle(20, Phase::Decode);
          break;
        }
      }
      phase_ = Phase::CheckMultiply;
      break;
    case Phase::Decode:
      BeginStream();
      phase_ = Phase::CheckMultiply;
      break;
    case Phase::CheckMultiply:
      if (multiplyPending_) {
        multiplyPending_ = false;
        Cycle(30, Phase::Multiply);
      } else
        phase_ = Phase::CheckDivide;
      break;
    case Phase::Multiply:
      Multiply();
      phase_ = Phase::CheckDivide;
      break;
    case Phase::CheckDivide:
      if (dividePending_) {
        dividePending_ = false;
        Cycle(40, Phase::Divide);
      } else
        Cycle(1, Phase::Idle);
      break;
    case Phase::Divide:
      Divide();
      Cycle(1, Phase::Idle);
      break;
    case Phase::Idle:
      phase_ = Phase::Begin;
      break;
    }
  }
}
void Spc7110::Cycle(unsigned clocks, Phase next) {
  balance_ += int64_t(clocks) * masterFrequency_;
  phase_ = next;
}
void Spc7110::BeginStream() {
  if (mode_ == 3)
    return;
  decoder_.Initialize(mode_, streamAddress_);
  decodedRow_ = decoder_.NextRow();
  const unsigned seek = registers_[0x0b] & 2 ? Word(5, 2) : 0;
  for (unsigned i = 0; i < seek; ++i)
    decodedRow_ = decoder_.NextRow();
  registers_[0x0c] |= 0x80;
  tileOffset_ = 0;
}
void Spc7110::Multiply() {
  const uint32_t a = Word(0x20, 2), b = Word(0x24, 2);
  const uint32_t result =
      registers_[0x2e] & 1 ? uint32_t(int32_t(int16_t(a)) * int32_t(int16_t(b))) : a * b;
  Store(0x28, 4, result);
  registers_[0x2f] &= 0x7f;
}
void Spc7110::Divide() {
  const uint32_t a = Word(0x20, 4), b = Word(0x26, 2);
  int64_t dividend = registers_[0x2e] & 1 ? int64_t(int32_t(a)) : int64_t(a);
  int64_t divisor = registers_[0x2e] & 1 ? int64_t(int16_t(b)) : int64_t(b);
  Store(0x28, 4, divisor ? uint32_t(dividend / divisor) : 0);
  Store(0x2c, 2, divisor ? uint32_t(dividend % divisor) : a);
  registers_[0x2f] &= 0x7f;
}
size_t Spc7110::Mirror(uint32_t address, size_t size) {
  if (!size)
    return 0;
  size_t offset = address, base = 0;
  while (offset >= size) {
    const auto bit = std::bit_floor(offset);
    offset -= bit;
    if (size > bit) {
      size -= bit;
      base += bit;
    }
  }
  return base + offset;
}
uint8_t Spc7110::Data(uint32_t address) const {
  if (data_.empty())
    return 0;
  const unsigned size = registers_[0x34] & 3;
  if (size != 3 && (address & 0x400000))
    return 0;
  const uint32_t offset = address & ((0x100000u << size) - 1);
  return data_[Mirror(offset, data_.size())];
}
uint8_t Spc7110::Rom(uint32_t address, uint8_t openBus) const {
  const uint32_t linear = address & 0x3fffff;
  const unsigned bank = linear >> 20;
  if (bank == 0)
    return rom_[Mirror(linear, std::min<size_t>(rom_.size(), 0x100000))];
  if (bank == 1 && (registers_[0x34] & 4))
    return rom_[Mirror(linear & 0xfffff, std::min<size_t>(rom_.size(), 0x100000))];
  if (bank < 4)
    return Data((uint32_t(registers_[0x30 + bank]) << 20) | (linear & 0xfffff));
  return openBus;
}
uint8_t Spc7110::Read(uint32_t address, uint8_t openBus) {
  Synchronize();
  const uint8_t bank = uint8_t(address >> 16);
  const uint16_t offset = uint16_t(address);
  if (bank == 0x50 || bank == 0x58)
    return ReadRegister(bank == 0x50 ? 0 : 8, openBus);
  if (!(bank & 0x40)) {
    if (offset >= 0x4800 && offset <= 0x483f)
      return ReadRegister(offset & 63, openBus);
    if (rtc_ && offset >= 0x4840 && offset <= 0x4842)
      return rtc_->Read(offset & 3, openBus);
    if (offset >= 0x6000 && offset <= 0x7fff) {
      const uint32_t reduced = ((address & 0xff0000) >> 3) | (offset & 0x1fff);
      return !(registers_[0x30] & 0x80) || save_.empty() ? 0 : save_[Mirror(reduced, save_.size())];
    }
    if (offset >= 0x8000)
      return Rom(address, openBus);
  }
  if (bank >= 0xc0)
    return Rom(address, openBus);
  if (rom_.size() == 0x700000 && bank >= 0x40 && bank <= 0x4f)
    return rom_[0x600000 + (address & 0xfffff)];
  return openBus;
}
void Spc7110::Write(uint32_t address, uint8_t value) {
  Synchronize();
  const uint8_t bank = uint8_t(address >> 16);
  const uint16_t offset = uint16_t(address);
  if (bank == 0x50 || bank == 0x58) {
    WriteRegister(bank == 0x50 ? 0 : 8, value);
    return;
  }
  if (bank & 0x40)
    return;
  if (offset >= 0x4800 && offset <= 0x483f)
    WriteRegister(offset & 63, value);
  else if (rtc_ && offset >= 0x4840 && offset <= 0x4842)
    rtc_->Write(offset & 3, value);
  else if (offset >= 0x6000 && offset <= 0x7fff && (registers_[0x30] & 0x80) && !save_.empty()) {
    const uint32_t reduced = ((address & 0xff0000) >> 3) | (offset & 0x1fff);
    save_[Mirror(reduced, save_.size())] = value;
  }
}
void Spc7110::MapCpu(MemoryBus &bus, uint8_t slot) const {
  for (uint8_t bank : {0x00, 0x80}) {
    bus.MapRange(bank, uint8_t(bank + 0x3f), 0x4800, rtc_ ? 0x4842 : 0x483f, slot);
    bus.MapRange(bank, uint8_t(bank + 0x3f), 0x6000, 0xffff, slot);
  }
  bus.MapRange(0xc0, 0xff, 0, 0xffff, slot);
  bus.MapRange(0x50, 0x50, 0, 0xffff, slot);
  bus.MapRange(0x58, 0x58, 0, 0xffff, slot);
  if (rom_.size() == 0x700000)
    bus.MapRange(0x40, 0x4f, 0, 0xffff, slot);
}
std::vector<uint8_t> Spc7110::SaveRtc() {
  return rtc_ ? rtc_->Save() : std::vector<uint8_t>{};
}
bool Spc7110::LoadRtc(std::span<const uint8_t> data) {
  return rtc_ && rtc_->Load(data);
}
} // namespace snes::core
