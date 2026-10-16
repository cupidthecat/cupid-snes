#include "snes/core/Cx4Processor.hpp"
#include "snes/core/MemoryBus.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/detail/ExecutionContext.hpp"

#include <bit>
#include <stdexcept>

namespace snes::core {

Cx4Processor::Cx4Processor(std::span<const uint8_t> rom, std::span<uint8_t> save,
                           std::span<const uint8_t> firmware, Layout layout)
    : rom_(rom), save_(save), layout_(layout) {
  if (!LoadDataRom(firmware))
    throw std::invalid_argument("Invalid Cx4 data firmware size");
  Reset();
}

Cx4Processor::~Cx4Processor() = default;

void Cx4Processor::Reset() {
  execution_.reset();
  Hg51b::Reset();
  balance_ = 0;
  irqLine_ = false;
}

void Cx4Processor::SetPal(bool pal) {
  masterFrequency_ = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
}

void Cx4Processor::Advance(uint32_t masterClocks) {
  balance_ -= int64_t(masterClocks) * 20'000'000;
  if (balance_ < 0 && CanAdvanceIdle()) {
    // No pending event can change halted state. Retain the same final clock edge
    // without resuming the worker for each otherwise empty processor cycle.
    const int64_t clocks = (-balance_ + masterFrequency_ - 1) / masterFrequency_;
    balance_ += clocks * masterFrequency_;
    return;
  }
  Synchronize();
}

void Cx4Processor::Synchronize() {
  if (balance_ >= 0)
    return;
  if (!execution_)
    execution_ = std::make_unique<detail::ExecutionContext>([this] {
      for (;;)
        RunStep();
    });
  execution_->Resume();
}

void Cx4Processor::StepClocks(unsigned clocks) {
  balance_ += int64_t(clocks) * masterFrequency_;
  if (balance_ >= 0)
    execution_->Suspend();
}

void Cx4Processor::Halt() {
  Hg51b::Halt();
  if (IrqEnabled()) {
    SetInterruptFlag();
    irqLine_ = true;
  }
}

void Cx4Processor::ClearIrq() {
  irqLine_ = false;
}

bool Cx4Processor::IsRom(uint32_t address) const {
  return (address & 0x408000) == 0x008000 || (address & 0xc00000) == 0xc00000;
}

bool Cx4Processor::IsRam(uint32_t address) const {
  return layout_ == Layout::LoRom ? (address & 0xf88000) == 0x700000
                                  : (address & 0x70e000) == 0x306000;
}

bool Cx4Processor::IsData(uint32_t address) const {
  return (address & 0x40e000) == 0x006000 && (address & 0xc00) != 0xc00 &&
         (layout_ == Layout::LoRom || (address & 0x300000) != 0x300000);
}

bool Cx4Processor::IsIo(uint32_t address) const {
  return (address & 0x40ec00) == 0x006c00 &&
         (layout_ == Layout::LoRom || (address & 0x300000) != 0x300000);
}

uint32_t Cx4Processor::RomOffset(uint32_t address) const {
  return layout_ == Layout::LoRom ? (((address & 0x3f0000) >> 1) | (address & 0x7fff)) & 0x1fffff
                                  : address & 0x3fffff;
}

uint32_t Cx4Processor::RamOffset(uint32_t address) const {
  return layout_ == Layout::LoRom ? (((address & 0x070000) >> 1) | (address & 0x7fff)) & 0x3ffff
                                  : (((address & 0x0f0000) >> 3) | (address & 0x1fff)) & 0x1ffff;
}

std::size_t Cx4Processor::Mirror(uint32_t address, std::size_t size) {
  std::size_t offset = address, base = 0;
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

uint8_t Cx4Processor::ReadRom(uint32_t address, uint8_t openBus) const {
  return rom_.empty() ? openBus : rom_[Mirror(RomOffset(address), rom_.size())];
}

uint8_t Cx4Processor::ReadRam(uint32_t address) const {
  return save_.empty() ? 0 : save_[Mirror(RamOffset(address), save_.size())];
}

uint8_t Cx4Processor::Read(uint32_t address, uint8_t openBus) {
  Synchronize();
  // The CPU board map and DSP external decoder use different address wiring.
  if ((address & 0x408000) == 0x008000) {
    const auto offset = ((address & 0xff0000) >> 1) | (address & 0x7fff);
    if (!Busy())
      return rom_.empty() ? openBus : rom_[Mirror(offset, rom_.size())];
    const auto vectors = layout_ == Layout::LoRom ? 0x7fc0u : 0xffc0u;
    return (offset & 0xbfffc0) == vectors ? ReadHost(uint16_t(0x7f40 | (offset & 63))) : openBus;
  }
  if ((address & 0xf88000) == 0x700000)
    return save_.empty() ? 0 : save_[Mirror(address, save_.size())];
  if ((address & 0x40e000) == 0x006000)
    return (address & 0xc00) == 0xc00 ? ReadHost(uint16_t(address)) : DataRam()[address & 0xfff];
  return openBus;
}

void Cx4Processor::Write(uint32_t address, uint8_t value) {
  Synchronize();
  if ((address & 0xf88000) == 0x700000) {
    if (!save_.empty())
      save_[Mirror(address, save_.size())] = value;
  } else if ((address & 0x40e000) == 0x006000) {
    if ((address & 0xc00) == 0xc00)
      WriteHost(uint16_t(address), value);
    else
      DataRam()[address & 0xfff] = value;
  }
}

uint8_t Cx4Processor::ReadExternal(uint32_t address) {
  if (IsRom(address))
    return ReadRom(address, 0);
  if (IsRam(address))
    return ReadRam(address);
  if (IsData(address))
    return DataRam()[address & 0xfff];
  return IsIo(address) ? ReadHost(uint16_t(address)) : 0;
}

void Cx4Processor::WriteExternal(uint32_t address, uint8_t value) {
  if (IsRom(address))
    return;
  if (IsRam(address)) {
    if (!save_.empty())
      save_[Mirror(RamOffset(address), save_.size())] = value;
  } else if (IsData(address)) {
    DataRam()[address & 0xfff] = value;
  } else if (IsIo(address)) {
    WriteHost(uint16_t(address), value);
  }
}

void Cx4Processor::MapCpu(MemoryBus &bus, uint8_t slot) const {
  bus.MapRange(0x00, 0x3f, 0x8000, 0xffff, slot);
  bus.MapRange(0x80, 0xbf, 0x8000, 0xffff, slot);
  bus.MapRange(0x00, 0x3f, 0x6000, 0x7fff, slot);
  bus.MapRange(0x80, 0xbf, 0x6000, 0x7fff, slot);
  bus.MapRange(0x70, 0x77, 0x0000, 0x7fff, slot);
}

} // namespace snes::core
