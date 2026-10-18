#include "snes/core/gameboy/Peripherals.hpp"

namespace snes::core::gameboy {

void Peripherals::Reset(bool secondModel) {
  clocks_ = 0;
  video_.Reset(secondModel);
  audio_.Reset(secondModel);
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

} // namespace snes::core::gameboy
