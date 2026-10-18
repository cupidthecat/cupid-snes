#include "snes/core/gameboy/Machine.hpp"

#include <algorithm>

namespace snes::core::gameboy {
namespace {
bool VideoBus(uint16_t address) noexcept {
  return address >= 0x8000 && address < 0xa000;
}
} // namespace

Machine::Machine(Host &host, Devices &devices) : host_(host), devices_(devices) {}

bool Machine::Load(std::span<const uint8_t> image, std::span<const uint8_t> boot,
                   std::string *error) {
  if (boot.size() != boot_.size()) {
    if (error)
      *error = "Handheld boot firmware must contain 256 bytes";
    return false;
  }
  if (!cartridge_.Load(image, error))
    return false;
  std::copy(boot.begin(), boot.end(), boot_.begin());
  Reset();
  return true;
}

void Machine::Reset(bool secondModel) {
  cpuClock_ = deviceClock_ = 0;
  cpu_.Reset();
  timer_.Reset();
  cartridge_.Reset();
  cartridge_.SetClockRate(secondModel ? 4194304 : 4295454);
  ram_.fill(secondModel ? 0x55 : 0);
  highRam_.fill(0);
  dmaSource_ = 0;
  dmaPosition_ = 0xa1;
  dmaClocks_ = 0;
  dmaRegister_ = 0xff;
  dmaRestarting_ = bootDisabled_ = false;
  flags_ = enable_ = dataBus_ = decay_ = 0;
  joypad_ = 0xcf;
  devices_.Reset(secondModel);
}

unsigned Machine::RunInstruction() {
  const auto before = cpuClock_;
  cpu_.RunInstruction();
  Synchronize();
  return unsigned(cpuClock_ - before);
}

void Machine::AdvanceHardware(unsigned clocks) {
  Advance(clocks);
  Synchronize();
}

void Machine::SetCpuState(const Cpu::State &state) {
  Synchronize();
  cpu_.SetState(state);
}

uint8_t Machine::ReadMemory(uint16_t address) {
  return Read(address);
}

void Machine::WriteMemory(uint16_t address, uint8_t value) {
  Synchronize();
  WriteData(address, value, Access::Cpu);
}

void Machine::Advance(unsigned clocks) {
  cpuClock_ += clocks;
}

void Machine::Synchronize() {
  Flush(cpuClock_);
}

void Machine::Flush(uint64_t target) {
  while (deviceClock_ < target) {
    ++deviceClock_;
    if (decay_) {
      decay_ = uint8_t(decay_ > 2 ? decay_ - 2 : 0);
      if (decay_ == 0)
        dataBus_ = 0xff;
    }
    timer_.Advance(1);
    flags_ |= devices_.Advance(1);
    RunDma(1);
    cartridge_.AdvanceRtc(1);
  }
  devices_.FinishAdvance();
}

void Machine::RequestInterrupt(uint8_t mask) {
  flags_ |= mask;
}

void Machine::AudioDividerEdge(bool rising) {
  devices_.AudioDividerEdge(rising, deviceClock_);
}

void Machine::DriveData(uint16_t address, uint8_t value) {
  if (!VideoBus(address) && address < 0xff00) {
    dataBus_ = value;
    decay_ = 12;
  }
}

bool Machine::DmaConflict(uint16_t address) const noexcept {
  if (dmaPosition_ == 0xa1 || dmaPosition_ == 0xff || dmaPosition_ == 0 || address >= 0xfe00 ||
      address == dmaSource_)
    return false;
  if (dmaSource_ >= 0xe000 && (dmaSource_ & ~0x2000) == address)
    return false;
  return VideoBus(address) == VideoBus(dmaSource_);
}

uint8_t Machine::Read(uint16_t address) {
  Synchronize();
  if (DmaConflict(address))
    address = uint16_t(dmaSource_ - 1);
  return ReadData(address, Access::Cpu);
}

uint8_t Machine::ReadData(uint16_t address, Access access) {
  uint8_t value = 0xff;
  bool driven = true;
  if (address < 0x100 && !bootDisabled_) {
    value = boot_[address];
  } else if (address < 0x8000 || (address >= 0xa000 && address < 0xc000)) {
    const auto result = cartridge_.Read(address);
    driven = result.has_value();
    value = result.value_or(dataBus_);
  } else if (address >= 0xc000 && address < 0xfe00) {
    value = ram_[address & 0x1fff];
  } else if (address >= 0xff80 && address != 0xffff) {
    value = highRam_[address - 0xff80];
  } else if (address == 0xffff) {
    value = enable_;
  } else if (address == 0xff00) {
    value = joypad_;
  } else if ((address >= 0xff01 && address <= 0xff02) || (address >= 0xff04 && address <= 0xff07)) {
    value = timer_.Read(address);
  } else if (address == 0xff0f) {
    value = flags_ | 0xe0;
  } else if (address == 0xff50) {
    value = uint8_t(0xfe | unsigned(bootDisabled_));
  } else if (address == 0xff46) {
    value = dmaRegister_;
  } else if (address >= 0xfe00 && address < 0xff00 && access == Access::Cpu &&
             dmaPosition_ != 0xa1 && (dmaPosition_ != 0 || dmaRestarting_)) {
    value = 0xff;
  } else {
    value = devices_.Read(address, access);
  }
  if (driven)
    DriveData(address, value);
  return value;
}

void Machine::Write(uint16_t address, uint8_t value) {
  const auto time = cpuClock_;
  if (address == 0xff43) {
    Flush(time >= 2 ? time - 2 : 0);
  } else if (address == 0xff42 || (address >= 0xff47 && address <= 0xff49)) {
    Flush(time ? time - 1 : 0);
  } else if (address == 0xff0f) {
    Flush(time + 1);
  } else if (address == 0xff40) {
    const auto previous = devices_.Read(address, Access::Cpu);
    Flush(time >= 2 ? time - 2 : 0);
    WriteData(address, value, Access::Cpu);
    WriteData(address, previous, Access::Cpu);
    Flush(time ? time - 1 : 0);
  } else {
    Flush(time);
  }
  if (address == 0xff41) {
    WriteData(address, 0xff, Access::Cpu);
    Flush(time + 1);
  }
  if (address == 0xff4b)
    devices_.WindowWrite(true);
  WriteData(address, value, Access::Cpu);
  if (address == 0xff4b) {
    Flush(time + 1);
    devices_.WindowWrite(false);
  }
}

void Machine::WriteData(uint16_t address, uint8_t value, Access access) {
  DriveData(address, value);
  if (access == Access::Cpu && DmaConflict(address)) {
    address = uint16_t(dmaSource_ - 1);
    if (address >= 0xa000) {
      const uint16_t last = uint16_t(0xfe00 + dmaPosition_ - 1);
      const auto previous = devices_.Read(last, Access::Dma);
      flags_ |= devices_.Write(last, uint8_t(previous & value), Access::Dma);
      return;
    }
  }
  if (address < 0x8000 || (address >= 0xa000 && address < 0xc000)) {
    cartridge_.Write(address, value);
  } else if (address >= 0xc000 && address < 0xfe00) {
    ram_[address & 0x1fff] = value;
  } else if (address >= 0xff80 && address != 0xffff) {
    highRam_[address - 0xff80] = value;
  } else if (address == 0xffff) {
    enable_ = value;
  } else if (address == 0xff00) {
    const auto input = host_.JoypadWrite(value & 0x10, value & 0x20);
    if (joypad_ & uint8_t(~input) & 15)
      flags_ |= 0x10;
    joypad_ = 0xc0 | input;
  } else if ((address >= 0xff01 && address <= 0xff02) || (address >= 0xff04 && address <= 0xff07)) {
    timer_.Write(address, value);
  } else if (address == 0xff0f) {
    flags_ = value;
  } else if (address == 0xff50) {
    bootDisabled_ |= value & 1;
  } else if (address == 0xff46) {
    dmaRestarting_ = dmaPosition_ != 0xa1 && dmaPosition_ != 0xa0;
    dmaRegister_ = value;
    dmaSource_ = uint16_t(value) << 8;
    dmaPosition_ = 0xff;
    dmaClocks_ = 2;
    flags_ |= devices_.DmaStatus(dmaPosition_, dmaRestarting_);
  } else if (!(address >= 0xfe00 && address < 0xff00 && access == Access::Cpu &&
               dmaPosition_ != 0xa1)) {
    flags_ |= devices_.Write(address, value, access);
  }
}

void Machine::RunDma(unsigned clocks, bool force) {
  if (dmaPosition_ == 0xa1 || (!force && (cpu_.GetState().halted || cpu_.GetState().stopped)))
    return;
  dmaClocks_ = uint8_t(dmaClocks_ + clocks);
  while (dmaClocks_ >= 4 && dmaPosition_ != 0xa1) {
    dmaClocks_ -= 4;
    if (dmaPosition_ == 0xff) {
      dmaPosition_ = 0;
    } else if (dmaPosition_ == 0xa0) {
      dmaPosition_ = 0xa1;
    } else {
      const uint16_t source = dmaSource_ >= 0xe000 ? dmaSource_ & ~0x2000 : dmaSource_;
      const auto value = ReadData(source, Access::Dma);
      flags_ |= devices_.Write(uint16_t(0xfe00 + dmaPosition_), value, Access::Dma);
      ++dmaPosition_;
      ++dmaSource_;
    }
    flags_ |= devices_.DmaStatus(dmaPosition_, dmaRestarting_);
  }
}

void Machine::AcknowledgeInterrupt(uint8_t mask) {
  Synchronize();
  flags_ &= uint8_t(~mask);
}

uint8_t Machine::WriteInterruptLow(uint16_t address, uint8_t value) {
  if (address != 0xff0f) {
    Write(address, value);
    return flags_;
  }
  Synchronize();
  const auto previous = flags_;
  WriteData(address, value, Access::Cpu);
  return previous;
}

void Machine::DriveAddress(uint16_t address) {
  Synchronize();
  devices_.AddressBus(address, false);
}

void Machine::AddressIncremented(uint16_t address) {
  Synchronize();
  devices_.AddressBus(address, true);
}

void Machine::SetStopped(bool stopped) {
  Synchronize();
  if (stopped)
    timer_.EnterStop(cpu_.GetState().ime);
  else {
    timer_.LeaveStop();
    RunDma(4, true);
  }
  devices_.Stop(stopped);
}

void Machine::Resume() {
  Synchronize();
  RunDma(4, true);
}

} // namespace snes::core::gameboy
