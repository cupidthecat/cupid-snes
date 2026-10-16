#include "snes/core/Hg51b.hpp"

namespace snes::core {

uint8_t Hg51b::ReadHost(uint16_t address) const {
  address = uint16_t(0x7c00 | (address & 0x3ff));
  const auto &r = state_.registers;
  const auto &c = state_.control;
  switch (address) {
  case 0x7f40:
  case 0x7f41:
  case 0x7f42:
    return uint8_t(c.dma.source >> ((address - 0x7f40) * 8));
  case 0x7f43:
  case 0x7f44:
    return uint8_t(c.dma.length >> ((address - 0x7f43) * 8));
  case 0x7f45:
  case 0x7f46:
  case 0x7f47:
    return uint8_t(c.dma.target >> ((address - 0x7f45) * 8));
  case 0x7f48:
    return c.cache.page;
  case 0x7f49:
  case 0x7f4a:
  case 0x7f4b:
    return uint8_t(c.cache.base >> ((address - 0x7f49) * 8));
  case 0x7f4c:
    return uint8_t(c.cache.locked[0] | (c.cache.locked[1] << 1));
  case 0x7f4d:
  case 0x7f4e:
    return uint8_t(c.cache.bank >> ((address - 0x7f4d) * 8));
  case 0x7f4f:
    return c.cache.counter;
  case 0x7f50:
    return uint8_t(c.ramWait | (c.romWait << 4));
  case 0x7f51:
    return uint8_t(c.irqDisabled);
  case 0x7f52:
    return uint8_t(c.singleRom);
  case 0x7f53:
  case 0x7f54:
  case 0x7f55:
  case 0x7f56:
  case 0x7f57:
  case 0x7f59:
  case 0x7f5b:
  case 0x7f5c:
  case 0x7f5d:
  case 0x7f5e:
  case 0x7f5f:
    return uint8_t(c.suspended | (bool(r.flags & Interrupt) << 1) | (Running() << 6) |
                   (Busy() << 7));
  }
  if (address >= 0x7f60 && address <= 0x7f7f)
    return c.vectors[address & 31];
  if ((address >= 0x7f80 && address <= 0x7faf) || (address >= 0x7fc0 && address <= 0x7fef)) {
    const unsigned offset = address & 63;
    return uint8_t(r.general[offset / 3] >> ((offset % 3) * 8));
  }
  return 0;
}

void Hg51b::WriteHost(uint16_t address, uint8_t value) {
  address = uint16_t(0x7c00 | (address & 0x3ff));
  auto &r = state_.registers;
  auto &c = state_.control;
  const auto byte = [&](uint32_t &target, unsigned shift) {
    target = (target & ~(255u << shift)) | (uint32_t(value) << shift);
  };
  switch (address) {
  case 0x7f40:
  case 0x7f41:
  case 0x7f42:
    byte(c.dma.source, (address - 0x7f40) * 8);
    return;
  case 0x7f43:
  case 0x7f44: {
    uint32_t length = c.dma.length;
    byte(length, (address - 0x7f43) * 8);
    c.dma.length = uint16_t(length);
    return;
  }
  case 0x7f45:
  case 0x7f46:
  case 0x7f47:
    byte(c.dma.target, (address - 0x7f45) * 8);
    if (address == 0x7f47 && c.halted)
      c.dma.active = true;
    return;
  case 0x7f48:
    c.cache.page = value & 1;
    if (c.halted) {
      r.bank = c.cache.bank;
      c.cache.preload = true;
      c.cache.loading = true;
    }
    return;
  case 0x7f49:
  case 0x7f4a:
  case 0x7f4b:
    byte(c.cache.base, (address - 0x7f49) * 8);
    return;
  case 0x7f4c:
    c.cache.locked[0] = value & 1;
    c.cache.locked[1] = value & 2;
    return;
  case 0x7f4d:
  case 0x7f4e: {
    uint32_t bank = c.cache.bank;
    byte(bank, (address - 0x7f4d) * 8);
    c.cache.bank = uint16_t(bank & 0x7fff);
    return;
  }
  case 0x7f4f:
    c.cache.counter = value;
    if (c.halted) {
      c.halted = false;
      r.bank = c.cache.bank;
      r.counter = value;
    }
    return;
  case 0x7f50:
    c.ramWait = value & 7;
    c.romWait = (value >> 4) & 7;
    return;
  case 0x7f51:
    c.irqDisabled = value & 1;
    if (c.irqDisabled) {
      r.flags &= ~Interrupt;
      ClearIrq();
    }
    return;
  case 0x7f52:
    c.singleRom = value & 1;
    return;
  case 0x7f53:
    c.locked = false;
    c.halted = true;
    return;
  case 0x7f55:
  case 0x7f56:
  case 0x7f57:
  case 0x7f58:
  case 0x7f59:
  case 0x7f5a:
  case 0x7f5b:
  case 0x7f5c:
    c.suspended = true;
    c.suspendClocks = uint8_t((address - 0x7f55) * 32);
    return;
  case 0x7f5d:
    c.suspended = false;
    return;
  case 0x7f5e:
    r.flags &= ~Interrupt;
    return;
  }
  if (address >= 0x7f60 && address <= 0x7f7f) {
    c.vectors[address & 31] = value;
    return;
  }
  if ((address >= 0x7f80 && address <= 0x7faf) || (address >= 0x7fc0 && address <= 0x7fef)) {
    const unsigned offset = address & 63;
    byte(r.general[offset / 3], (offset % 3) * 8);
  }
}

} // namespace snes::core
