#include "snes/core/gameboy/Peripherals.hpp"

namespace snes::core::gameboy {

void Peripherals::Reset(bool secondModel) {
  Reset(secondModel, false);
}

void Peripherals::Reset(bool secondModel, bool preserveAudio) {
  clocks_ = 0;
  video_.Reset(secondModel);
  audio_.Reset(secondModel, preserveAudio);
}

uint8_t Peripherals::Read(uint16_t address, Machine::Access access) {
  if (address >= 0xff10 && address < 0xff40) {
    audio_.AdvanceTo(clocks_);
    return audio_.Read(address);
  }
  return video_.Read(address, access == Machine::Access::Dma);
}

uint8_t Peripherals::Write(uint16_t address, uint8_t value, Machine::Access access) {
  if (address >= 0xff10 && address < 0xff40) {
    audio_.AdvanceTo(clocks_);
    audio_.Write(address, value);
    return 0;
  }
  return video_.Write(address, value, access == Machine::Access::Dma);
}

uint8_t Peripherals::Advance(unsigned clocks) {
  uint8_t interrupts = 0;
  while (clocks--) {
    ++clocks_;
    interrupts |= video_.Advance(1);
  }
  return interrupts;
}

void Peripherals::FinishAdvance() {
  audio_.AdvanceTo(clocks_);
}

void Peripherals::AudioDividerEdge(bool rising, uint64_t clock) {
  audio_.DividerEdge(rising, clock);
}

void Peripherals::Stop(bool stopped) {
  video_.Stop(stopped);
  audio_.Stop(stopped);
}

void Peripherals::WindowWrite(bool active) {
  video_.WindowWrite(active);
}

void Peripherals::AddressBus(uint16_t address, bool incremented) {
  video_.AddressBus(address, incremented);
}

void Peripherals::BlockedOamAccess(uint16_t address, bool write) {
  video_.BlockedOamAccess(address, write);
}

void Peripherals::SetDmaSynchronizer(std::function<void()> synchronize) {
  video_.SetDmaSynchronizer(std::move(synchronize));
}

void Peripherals::SetCpuSleeping(bool sleeping) {
  video_.SetCpuSleeping(sleeping);
}

uint8_t Peripherals::DmaStatus(uint16_t source, uint8_t position) {
  video_.DmaStatus(source, position);
  return 0;
}

} // namespace snes::core::gameboy
