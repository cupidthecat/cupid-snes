#include "snes/core/Hg51b.hpp"

#include <algorithm>

namespace snes::core {

void Hg51b::Reset() {
  state_.registers = {};
  state_.control = {};
  idleCycle_ = false;
}

bool Hg51b::LoadDataRom(std::span<const uint8_t> bytes) {
  if (bytes.size() != state_.dataRom.size() * 3)
    return false;
  for (unsigned i = 0; i < state_.dataRom.size(); ++i)
    state_.dataRom[i] = uint32_t(bytes[i * 3]) | (uint32_t(bytes[i * 3 + 1]) << 8) |
                        (uint32_t(bytes[i * 3 + 2]) << 16);
  return true;
}

void Hg51b::SetState(const State &state) {
  state_ = state;
  idleCycle_ = false;
  auto &r = state_.registers;
  r.product &= 0xffffffffffffull;
  for (auto &value : r.general)
    value &= Mask;
  r.accumulator &= Mask;
  r.memoryData &= Mask;
  r.romData &= Mask;
  r.ramData &= Mask;
  r.memoryAddress &= Mask;
  r.dataPointer &= Mask;
  r.bank &= 0x7fff;
  r.page &= 0x7fff;
  r.flags &= 31;
  auto &c = state_.control;
  c.romWait &= 7;
  c.ramWait &= 7;
  c.cache.page &= 1;
  c.cache.base &= Mask;
  c.cache.bank &= 0x7fff;
  for (auto &value : c.cache.address)
    value &= Mask;
  c.dma.source &= Mask;
  c.dma.target &= Mask;
  c.bus.address &= Mask;
  c.bus.pending &= 15;
  for (auto &value : state_.stack)
    value &= 0x7fffff;
  for (auto &value : state_.dataRom)
    value &= Mask;
}

bool Hg51b::Running() const noexcept {
  const auto &c = state_.control;
  return c.cache.loading || c.dma.active || c.bus.pending || !c.halted;
}

bool Hg51b::Busy() const noexcept {
  const auto &c = state_.control;
  return c.cache.loading || c.dma.active || c.bus.pending;
}

bool Hg51b::CanAdvanceIdle() const noexcept {
  const auto &c = state_.control;
  return idleCycle_ && c.halted && !c.locked && !c.suspended && !c.cache.loading && !c.dma.active &&
         !c.bus.active && !c.bus.pending;
}

void Hg51b::Halt() {
  state_.control.halted = true;
}

void Hg51b::Cycle(unsigned clocks) {
  auto &bus = state_.control.bus;
  if (bus.active) {
    if (bus.pending > clocks)
      bus.pending = uint8_t(bus.pending - clocks);
    else {
      bus.active = false;
      bus.pending = 0;
      if (bus.reading) {
        bus.reading = false;
        state_.registers.memoryData = ReadExternal(bus.address);
      }
      if (bus.writing) {
        bus.writing = false;
        WriteExternal(bus.address, uint8_t(state_.registers.memoryData));
      }
    }
  }
  StepClocks(clocks);
}

unsigned Hg51b::Wait(uint32_t address) const {
  if (IsRom(address))
    return 1 + state_.control.romWait;
  return IsRam(address) ? 1 + state_.control.ramWait : 0;
}

bool Hg51b::CacheReady() {
  auto &cache = state_.control.cache;
  uint32_t address = (cache.base + state_.registers.bank * 512) & Mask;
  if (!cache.preload) {
    if (cache.address[cache.page] == address) {
      cache.loading = false;
      return true;
    }
    cache.page ^= 1;
    if (cache.address[cache.page] == address) {
      cache.loading = false;
      return true;
    }
    if (cache.locked[cache.page])
      cache.page ^= 1;
    if (cache.locked[cache.page]) {
      cache.loading = false;
      return false;
    }
  }
  cache.preload = false;
  cache.address[cache.page] = address;
  for (unsigned word = 0; word < 256; ++word) {
    Cycle(Wait(address));
    state_.program[cache.page][word] = ReadExternal(address);
    address = (address + 1) & Mask;
    Cycle(Wait(address));
    state_.program[cache.page][word] |= uint16_t(ReadExternal(address)) << 8;
    address = (address + 1) & Mask;
  }
  cache.loading = false;
  return true;
}

void Hg51b::Next() {
  auto &r = state_.registers;
  if (++r.counter)
    return;
  auto &cache = state_.control.cache;
  if (cache.page == 1) {
    Halt();
    return;
  }
  cache.page = 1;
  if (cache.locked[1]) {
    Halt();
    return;
  }
  r.bank = r.page;
  if (!CacheReady())
    Halt();
}

void Hg51b::Transfer() {
  auto &dma = state_.control.dma;
  const unsigned length = dma.length;
  for (unsigned offset = 0; offset < length; ++offset) {
    const uint32_t source = (dma.source + offset) & Mask;
    const uint32_t target = (dma.target + offset) & Mask;
    if ((IsRom(source) && IsRom(target)) || (IsRam(source) && IsRam(target))) {
      state_.control.locked = true;
      return;
    }
    Cycle(Wait(source));
    const auto value = ReadExternal(source);
    Cycle(Wait(target));
    WriteExternal(target, value);
  }
  dma.active = false;
}

void Hg51b::RunStep() {
  auto &c = state_.control;
  if (c.locked) {
    Cycle(1);
  } else if (c.suspended) {
    if (!c.suspendClocks)
      Cycle(1);
    else {
      Cycle(c.suspendClocks);
      c.suspendClocks = 0;
      c.suspended = false;
    }
  } else if (c.cache.loading) {
    CacheReady();
  } else if (c.dma.active) {
    Transfer();
  } else if (c.halted) {
    struct ClearIdle {
      bool &value;
      ~ClearIdle() {
        value = false;
      }
    } clear{idleCycle_};
    idleCycle_ = true;
    Cycle(1);
  } else if (!CacheReady()) {
    Halt();
  } else {
    const auto instruction = state_.program[c.cache.page][state_.registers.counter];
    Next();
    Cycle(1);
    Execute(instruction);
  }
}

void Hg51b::BusRequest(bool write, bool ram) {
  auto &c = state_.control;
  c.bus.active = true;
  (write ? c.bus.writing : c.bus.reading) = true;
  c.bus.pending = ram ? c.ramWait : c.romWait;
  c.bus.address = state_.registers.memoryAddress;
}

uint32_t Hg51b::Register(unsigned address) {
  auto &r = state_.registers;
  address &= 127;
  if (address >= 0x60)
    return r.general[address & 15];
  constexpr uint32_t constants[]{0,        0xffffff, 0x00ff00, 0xff0000, 0x00ffff, 0xffff00,
                                 0x800000, 0x7fffff, 0x008000, 0x007fff, 0xff7fff, 0xffff7f,
                                 0x010000, 0xfeffff, 0x000100, 0x00feff};
  if (address >= 0x50)
    return constants[address - 0x50];
  switch (address) {
  case 1:
    return uint32_t(r.product >> 24);
  case 2:
    return uint32_t(r.product) & Mask;
  case 3:
    return r.memoryData;
  case 8:
    return r.romData;
  case 12:
    return r.ramData;
  case 0x13:
    return r.memoryAddress;
  case 0x1c:
    return r.dataPointer;
  case 0x20:
    return r.counter;
  case 0x28:
    return r.page;
  case 0x2e:
  case 0x2f:
    BusRequest(false, address & 1);
    break;
  }
  return 0;
}

void Hg51b::Put(unsigned address, uint32_t value) {
  auto &r = state_.registers;
  address &= 127;
  value &= Mask;
  if (address >= 0x60) {
    r.general[address & 15] = value;
    return;
  }
  switch (address) {
  case 1:
    r.product = (r.product & Mask) | (uint64_t(uint8_t(value)) << 24);
    break;
  case 2:
    r.product = (r.product & 0xffffff000000ull) | value;
    break;
  case 3:
    r.memoryData = value;
    break;
  case 8:
    r.romData = value;
    break;
  case 12:
    r.ramData = value;
    break;
  case 0x13:
    r.memoryAddress = value;
    break;
  case 0x1c:
    r.dataPointer = value;
    break;
  case 0x20:
    r.counter = uint8_t(value);
    break;
  case 0x28:
    r.page = uint16_t(value & 0x7fff);
    break;
  case 0x2e:
  case 0x2f:
    BusRequest(true, address & 1);
    break;
  }
}

void Hg51b::ResultFlags(uint32_t value) {
  auto &flags = state_.registers.flags;
  flags = uint8_t((flags & ~(Negative | Zero)) | (value & Sign ? Negative : 0) |
                  (value == 0 ? Zero : 0));
}

uint32_t Hg51b::Arithmetic(uint32_t a, uint32_t b, bool subtract) {
  const int64_t wide = subtract ? int64_t(a) - b : int64_t(a) + b;
  const auto result = uint32_t(wide) & Mask;
  ResultFlags(result);
  auto &flags = state_.registers.flags;
  flags =
      uint8_t((flags & ~(Carry | Overflow)) | ((subtract ? wide >= 0 : wide > Mask) ? Carry : 0) |
              (((a ^ result) & (a ^ ~b) & Sign) ? Overflow : 0));
  return result;
}

void Hg51b::Branch(uint8_t target, bool far, bool call) {
  auto &r = state_.registers;
  if (call) {
    for (unsigned i = 7; i; --i)
      state_.stack[i] = state_.stack[i - 1];
    state_.stack[0] = (uint32_t(r.bank) << 8) | r.counter;
  }
  if (far)
    r.bank = r.page;
  r.counter = target;
  Cycle(2);
}

void Hg51b::Execute(uint16_t op) {
  auto &r = state_.registers;
  const unsigned group = op >> 10, high = op >> 8;
  if ((group >= 2 && group <= 6) || (group >= 10 && group <= 14)) {
    const unsigned condition = group >= 10 ? group - 10 : group - 2;
    constexpr uint8_t flags[]{0, Zero, Carry, Negative, Overflow};
    if (!condition || (r.flags & flags[condition]))
      Branch(uint8_t(op), op & 0x200, group >= 10);
  } else if (group == 7) {
    if (state_.control.bus.active)
      Cycle(state_.control.bus.pending);
  } else if (group == 9) {
    constexpr uint8_t flags[]{Overflow, Carry, Zero, Negative};
    if (bool(r.flags & flags[high & 3]) == bool(op & 1)) {
      Next();
      Cycle(1);
    }
  } else if (group == 15) {
    const uint32_t address = state_.stack[0];
    std::move(state_.stack.begin() + 1, state_.stack.end(), state_.stack.begin());
    state_.stack.back() = 0;
    r.bank = uint16_t(address >> 8);
    r.counter = uint8_t(address);
    Cycle(2);
  } else if (group == 16) {
    r.memoryAddress = (r.memoryAddress + 1) & Mask;
  } else if (group >= 18 && group <= 21) {
    const uint32_t a = (r.accumulator << std::array{0u, 1u, 8u, 16u}[high & 3]) & Mask;
    const uint32_t b = group & 1 ? uint8_t(op) : Register(op);
    Arithmetic(group < 20 ? b : a, group < 20 ? a : b, true);
  } else if (high == 0x59 || high == 0x5a) {
    const unsigned shift = high == 0x59 ? 24 : 16;
    r.accumulator = uint32_t(int32_t(r.accumulator << shift) >> shift) & Mask;
    ResultFlags(r.accumulator);
  } else if (high >= 0x60 && high <= 0x67) {
    uint32_t value;
    if (high == 0x63)
      value = r.general[op & 15];
    else
      value = high & 4 ? uint8_t(op) : Register(op);
    switch (high & 3) {
    case 0:
      r.accumulator = value;
      break;
    case 1:
      r.memoryData = value;
      break;
    case 2:
      r.memoryAddress = value;
      break;
    case 3:
      r.page = uint16_t(value & 0x7fff);
      break;
    }
  } else if ((high >= 0x68 && high <= 0x6e) || (high >= 0xe8 && high <= 0xee)) {
    const unsigned byte = high & 3;
    if (byte < 3) {
      unsigned address = (high & 4 ? r.dataPointer + uint8_t(op) : r.accumulator) & 0xfff;
      if (address >= 0xc00)
        address -= 0x400;
      if (high & 0x80)
        state_.dataRam[address] = uint8_t(r.ramData >> (byte * 8));
      else
        r.ramData =
            (r.ramData & ~(255u << (byte * 8))) | (uint32_t(state_.dataRam[address]) << (byte * 8));
    }
  } else if (group == 28 || group == 29) {
    r.romData = state_.dataRom[(group == 28 ? r.accumulator : op) & 1023];
  } else if (high == 0x7c) {
    r.page = uint16_t((r.page & 0x7f00) | uint8_t(op));
  } else if (high == 0x7d) {
    r.page = uint16_t((r.page & 255) | ((op & 127) << 8));
  } else if (group >= 32 && group <= 47) {
    const uint32_t b = group & 1 ? uint8_t(op) : Register(op);
    if (group == 38 || group == 39) {
      const int64_t aSigned = int32_t(r.accumulator << 8) >> 8;
      const int64_t bSigned = int32_t(b << 8) >> 8;
      r.product = uint64_t(aSigned * bSigned) & 0xffffffffffffull;
    } else {
      const uint32_t a = (r.accumulator << std::array{0u, 1u, 8u, 16u}[high & 3]) & Mask;
      if (group < 38)
        r.accumulator = Arithmetic(group < 34 || group >= 36 ? a : b,
                                   group < 34 || group >= 36 ? b : a, group >= 34);
      else {
        switch ((group - 40) / 2) {
        case 0:
          r.accumulator = (~a ^ b) & Mask;
          break;
        case 1:
          r.accumulator = a ^ b;
          break;
        case 2:
          r.accumulator = a & b;
          break;
        case 3:
          r.accumulator = a | b;
          break;
        }
        ResultFlags(r.accumulator);
      }
    }
  } else if (group >= 48 && group <= 55) {
    unsigned count = (group & 1 ? op : Register(op)) & 31;
    if (count > 24)
      count = 0;
    uint32_t value = r.accumulator;
    if (count) {
      switch ((group - 48) / 2) {
      case 0:
        value >>= count;
        break;
      case 1:
        value = uint32_t((int32_t(value << 8) >> 8) >> count);
        break;
      case 2:
        value = (value >> count) | (value << (24 - count));
        break;
      case 3:
        value <<= count;
        break;
      }
    }
    r.accumulator = value & Mask;
    ResultFlags(r.accumulator);
  } else if (high == 0xe0 || high == 0xe1) {
    Put(op, high & 1 ? r.memoryData : r.accumulator);
  } else if (group == 60) {
    std::swap(r.accumulator, r.general[op & 15]);
  } else if (group == 62) {
    r.accumulator = r.page = r.ramData = r.dataPointer = 0;
  } else if (group == 63) {
    Halt();
  }
}

} // namespace snes::core
