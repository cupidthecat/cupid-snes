// Clocked GSU execution, cartridge arbitration, and graphics writeback.
#include "snes/core/Timing.hpp"
#include "superfx/Impl.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <utility>
#include <vector>

namespace snes::core {
namespace {
size_t Mirror(size_t address, size_t size) {
  if (!size)
    return 0;
  if (address < size)
    return address;
  const auto line = std::bit_floor(address);
  return size > line ? line + Mirror(address - line, size - line) : Mirror(address - line, size);
}

bool IoBank(uint32_t address) {
  return (address & 0x400000) == 0;
}
} // namespace

SuperFx::Impl::Impl(std::span<const uint8_t> image, std::span<uint8_t> memory)
    : rom(image), ram(memory) {
  steps.reserve(8);
}
inline bool SuperFx::Impl::Running() const {
  return (status & Go) != 0;
}
inline unsigned SuperFx::Impl::CoreCost() const {
  return clockSelect ? 1u : 2u;
}
inline unsigned SuperFx::Impl::MemoryCost() const {
  return clockSelect ? 5u : 6u;
}
inline bool SuperFx::Impl::Granted(Port port) const {
  return port == Port::None || (screenMode & (port == Port::Rom ? 0x10 : 0x08));
}
inline uint8_t SuperFx::Impl::RamByte(uint32_t address) const {
  return ram.empty() ? 0 : ram[Mirror(address & 0x3ffff, ram.size())];
}
inline void SuperFx::Impl::StoreRam(uint32_t address, uint8_t value) {
  if (!ram.empty())
    ram[Mirror(address & 0x3ffff, ram.size())] = value;
}
inline uint8_t SuperFx::Impl::MemoryByte(uint32_t address) const {
  const auto bank = uint8_t(address >> 16);
  if (BankPort(bank) == Port::Ram)
    return RamByte(address);
  if (BankPort(bank) != Port::Rom || rom.empty())
    return 0;
  const size_t offset =
      bank < 0x40 ? (size_t(bank) << 15) | (address & 0x7fff) : address & 0x1fffff;
  return rom[Mirror(offset, std::min(rom.size(), size_t(0x200000)))];
}
inline void SuperFx::Impl::Flag(uint16_t mask, bool set) {
  status = uint16_t((status & ~mask) | (set ? mask : 0));
}
inline void SuperFx::Impl::ResultFlags(uint16_t value, bool byteSign) {
  Flag(Zero, value == 0);
  Flag(Sign, (value & (byteSign ? 0x80 : 0x8000)) != 0);
}
inline void SuperFx::Impl::SetRegister(unsigned index, uint16_t value) {
  r[index] = value;
  if (index == 14)
    r14Written = true;
  if (index == 15)
    pcWritten = true;
}
inline void SuperFx::Impl::ClearPrefix() {
  status &= uint16_t(~(Alt1 | Alt2 | With));
  source = destination = 0;
}
inline void SuperFx::Impl::StartRomRead() {
  romAddress = (uint32_t(romBank) << 16) | r[14];
  romRemaining = MemoryCost();
  status |= RomBusy;
}
inline void SuperFx::Impl::SetColor(uint8_t value) {
  if (plotOptions & 4)
    color = uint8_t((color & 0xf0) | (value >> 4));
  else if (plotOptions & 8)
    color = uint8_t((color & 0xf0) | (value & 15));
  else
    color = value;
}
inline unsigned SuperFx::Impl::Depth() const {
  constexpr unsigned depths[]{2, 4, 4, 8};
  return depths[screenMode & 3];
}
inline uint32_t SuperFx::Impl::PixelAddress(uint8_t x, uint8_t y) const {
  const unsigned heightMode =
      (plotOptions & 16) ? 3u : ((screenMode >> 2) & 1u) | ((screenMode >> 4) & 2u);
  const unsigned tx = x / 8, ty = y / 8;
  const unsigned tile = heightMode == 3
                            ? (tx % 16) + (ty % 16) * 16 + (tx / 16) * 256 + (ty / 16) * 512
                            : tx * (16 + heightMode * 4) + ty;
  return (uint32_t(screenBase) << 10) + tile * Depth() * 8 + (y % 8) * 2;
}
inline unsigned SuperFx::Impl::PlaneOffset(unsigned plane) {
  return (plane / 2) * 16 + plane % 2;
}
inline void SuperFx::Impl::BeginFetch(uint16_t pc, Phase next) {
  work.phase = next;
  fetch.offset = uint16_t(pc - cacheBase);
  fetch.cached = fetch.offset < 512;
  fetch.hit = fetch.cached && (validLines & (uint32_t(1) << (fetch.offset / 16)));
  if (fetch.hit) {
    BeginWait(CoreCost(), Port::None);
    return;
  }
  // Cache hits retain only the selected byte. Fill latches are used on a miss.
  fetch.address = pc;
  fetch.bank = programBank;
  fetch.fillByte = 0;
  fetch.phase = FetchState::Phase::Read;
  const auto port = BankPort(fetch.bank);
  if (fetch.cached && !fetch.hit) {
    cacheFill = true;
    BeginWait(MemoryCost(), port);
    return;
  }
  const unsigned conflict = fetch.cached        ? 0
                            : port == Port::Rom ? romRemaining
                            : port == Port::Ram ? ramRemaining
                                                : 0;
  if (!fetch.cached) {
    cacheFill = true;
    // Synchronization and the memory access settle buffers on separate steps.
    fetch.phase = conflict ? FetchState::Phase::Synchronize : FetchState::Phase::Read;
    BeginWait(conflict ? conflict : MemoryCost(), Port::None);
    return;
  }
  BeginWait(CoreCost(), Port::None);
}

inline bool SuperFx::Impl::CompleteFetch() {
  if (!fetch.cached) {
    if (fetch.phase == FetchState::Phase::Synchronize) {
      fetch.phase = FetchState::Phase::Read;
      BeginWait(MemoryCost(), Port::None);
      return false;
    }
    if (fetch.phase == FetchState::Phase::Read) {
      fetch.bank = programBank;
      fetch.phase = FetchState::Phase::Access;
    }
    if (!Granted(BankPort(fetch.bank))) {
      BeginWait(6, Port::None);
      return false;
    }
    cacheFill = false;
    return true;
  }
  const unsigned line = fetch.offset & 0x1f0;
  const uint32_t base = (uint32_t(fetch.bank) << 16) | (fetch.address & 0xfff0);
  cache[line + fetch.fillByte] = MemoryByte(base + fetch.fillByte);
  if (++fetch.fillByte < 16) {
    BeginWait(MemoryCost(), waitPort);
    return false;
  }
  validLines |= uint32_t(1) << (fetch.offset / 16);
  cacheFill = false;
  return true;
}

inline uint8_t SuperFx::Impl::FetchResult() const {
  return fetch.cached ? cache[fetch.offset]
                      : MemoryByte((uint32_t(fetch.bank) << 16) | fetch.address);
}

inline bool SuperFx::Impl::BeginRamRead(uint32_t address, Phase next) {
  work.phase = next;
  work.address = address;
  if (!ramRemaining && Granted(Port::Ram))
    return false;
  BeginWait(ramRemaining, Port::Ram);
  return true;
}

inline bool SuperFx::Impl::BeginRamWrite(uint32_t address, uint8_t data, Phase next) {
  work.phase = next;
  work.address = address;
  work.data = data;
  if (!ramRemaining)
    return false;
  BeginWait(ramRemaining, Port::None);
  return true;
}

inline void SuperFx::Impl::QueueRamWrite() {
  ramAddress = work.address;
  ramBuffer = work.data;
  ramRemaining = MemoryCost();
}

inline bool SuperFx::Impl::BeginRomRead(Phase next) {
  work.phase = next;
  if (!romRemaining)
    return false;
  BeginWait(romRemaining, Port::None);
  return true;
}

inline bool SuperFx::Impl::BeginDelay(uint32_t clocks, Port port, Phase next) {
  work.phase = next;
  if (!clocks && Granted(port))
    return false;
  BeginWait(clocks, port);
  return true;
}

inline bool SuperFx::Impl::BeginFlush(unsigned index, Phase next) {
  work.phase = next;
  flush = {};
  flush.index = index;
  flush.address =
      PixelAddress(uint8_t(pixels[index].position * 8), uint8_t(pixels[index].position >> 5));
  flush.depth = Depth();
  if (!pixels[index].pending)
    return false;
  pixelFlush = true;
  BeginPlane();
  return true;
}

inline void SuperFx::Impl::BeginPlane() {
  const auto &row = pixels[flush.index];
  flush.data = 0;
  for (unsigned lane = 0; lane < 8; ++lane)
    flush.data |= uint8_t(((row.colors[lane] >> flush.plane) & 1) << (7 - lane));
  flush.reading = row.pending != 0xff;
  BeginWait(MemoryCost(), Port::Ram);
}

inline bool SuperFx::Impl::CompletePixelAccess() {
  const auto target = flush.address + PlaneOffset(flush.plane);
  if (flush.reading) {
    const auto mask = pixels[flush.index].pending;
    flush.data = uint8_t((flush.data & mask) | (RamByte(target) & ~mask));
    flush.reading = false;
    BeginWait(MemoryCost(), Port::Ram);
    return false;
  }
  StoreRam(target, flush.data);
  if (++flush.plane < flush.depth) {
    BeginPlane();
    return false;
  }
  pixelFlush = false;
  return true;
}
inline void SuperFx::Impl::PrepareStep() {
  // A native step settles buffered accesses before yielding its clocks.
  // A denied access inserts six-clock polls and retains the interrupted
  // step, including the ordering of simultaneous ROM and RAM completion.
  for (;;) {
    auto &step = steps.back();
    switch (step.phase) {
    case StepPhase::RomCountdown:
      step.phase = StepPhase::RamCountdown;
      if (romRemaining) {
        romRemaining -= std::min(step.clocks, romRemaining);
        if (!romRemaining) {
          status &= uint16_t(~RomBusy);
          step.address = romAddress;
          step.phase = StepPhase::RomAccess;
        }
      }
      break;
    case StepPhase::RomAccess:
      if (!Granted(BankPort(uint8_t(step.address >> 16)))) {
        steps.push_back(NativeStep{6});
      } else {
        romBuffer = MemoryByte(step.address);
        step.phase = StepPhase::RamCountdown;
      }
      break;
    case StepPhase::RamCountdown:
      step.phase = StepPhase::Clock;
      if (ramRemaining) {
        ramRemaining -= std::min(step.clocks, ramRemaining);
        if (!ramRemaining) {
          step.address = ramAddress;
          step.data = ramBuffer;
          step.phase = StepPhase::RamAccess;
        }
      }
      break;
    case StepPhase::RamAccess:
      if (!Granted(Port::Ram)) {
        steps.push_back(NativeStep{6});
      } else {
        StoreRam(step.address, step.data);
        step.phase = StepPhase::Clock;
      }
      break;
    case StepPhase::Clock:
      waitRemaining = step.clocks;
      return;
    }
  }
}
inline void SuperFx::Impl::BeginWait(uint32_t clocks, Port port) {
  waitPort = port;
  steps.push_back(NativeStep{clocks});
  PrepareStep();
}
inline void SuperFx::Impl::ResumeReady() {
  if (pixelFlush) {
    if (!CompletePixelAccess())
      return;
    pixels[flush.index].pending = 0;
  }
  if (waitingForGo) {
    if (!Running()) {
      idleStep = true;
      BeginWait(6, Port::None);
      return;
    }
    waitingForGo = false;
  }
  Run();
}
inline void SuperFx::Impl::FinishStep() {
  steps.pop_back();
  if (!steps.empty()) {
    PrepareStep();
    return;
  }
  if (!Granted(waitPort)) {
    BeginWait(6, waitPort);
    return;
  }
  if (idleStep)
    idleStep = false;
  else if (cacheFill && !CompleteFetch())
    return;
  ResumeReady();
}
inline void SuperFx::Impl::Synchronize(bool inclusive) {
  while (masterBalance < 0 || (inclusive && masterBalance == 0)) {
    if (steps.empty()) {
      ResumeReady();
      continue;
    }
    masterBalance += int64_t(waitRemaining) * masterFrequency;
    waitRemaining = 0;
    // A CPU clock edge returns before the step resumes its caller.
    if (masterBalance > 0 || (!inclusive && masterBalance == 0))
      return;
    FinishStep();
  }
}
inline void SuperFx::Impl::Advance(uint64_t clocks) {
  while (clocks) {
    const auto slice = std::min<uint64_t>(clocks, std::numeric_limits<uint32_t>::max());
    masterBalance -= int64_t(slice) * masterFrequency;
    Synchronize(true);
    clocks -= slice;
  }
}
inline void SuperFx::Impl::AdvanceMasterClocks(uint32_t clocks) {
  masterBalance -= int64_t(clocks) * int64_t(ClockFrequency);
  Synchronize(false);
}
inline void SuperFx::Impl::Run() {
  const auto opcode = work.opcode;
  const unsigned n = opcode & 15;
// Dispatch once when resuming; completed phases continue without another switch.
Dispatch:
  switch (work.phase) {
  case Phase::Begin:
    goto Begin;
  case Phase::Opcode:
    goto Opcode;
  case Phase::ImmediateLow:
    goto ImmediateLow;
  case Phase::ImmediateHigh:
    goto ImmediateHigh;
  case Phase::Execute:
    goto Execute;
  case Phase::StoreLow:
    goto StoreLow;
  case Phase::StoreWordLow:
    goto StoreWordLow;
  case Phase::StoreHigh:
    goto StoreHigh;
  case Phase::LoadLow:
    goto LoadLow;
  case Phase::LoadWordLow:
    goto LoadWordLow;
  case Phase::LoadHigh:
    goto LoadHigh;
  case Phase::RpixFlushFirst:
    goto RpixFlushFirst;
  case Phase::RpixFlushSecond:
    goto RpixFlushSecond;
  case Phase::RpixPlane:
    goto RpixPlane;
  case Phase::PlotEvict:
    goto PlotEvict;
  case Phase::PlotWrite:
    goto PlotWrite;
  case Phase::PlotFull:
    goto PlotFull;
  case Phase::RomColor:
    goto RomColor;
  case Phase::RamBank:
    goto RamBank;
  case Phase::RomBank:
    goto RomBank;
  case Phase::RomByte:
    goto RomByte;
  case Phase::Finish:
    goto Finish;
  }
Begin:
  if (!Running()) {
    waitingForGo = true;
    return;
  }
  work.opcode = pipeline;
  BeginFetch(r[15], Phase::Opcode);
  return;
Opcode: {
  pipeline = FetchResult();
  pcWritten = false;
  work.mode = (status >> 8) & 3;
  work.input = r[source];
  work.takeBranch = false;
  if (opcode >= 0x05 && opcode <= 0x0f) {
    const bool s = (status & Sign) != 0, v = (status & Overflow) != 0;
    switch (opcode) {
    case 0x05:
      work.takeBranch = true;
      break;
    case 0x06:
      work.takeBranch = s == v;
      break;
    case 0x07:
      work.takeBranch = s != v;
      break;
    case 0x08:
      work.takeBranch = !(status & Zero);
      break;
    case 0x09:
      work.takeBranch = (status & Zero) != 0;
      break;
    case 0x0a:
      work.takeBranch = !s;
      break;
    case 0x0b:
      work.takeBranch = s;
      break;
    case 0x0c:
      work.takeBranch = !(status & Carry);
      break;
    case 0x0d:
      work.takeBranch = (status & Carry) != 0;
      break;
    case 0x0e:
      work.takeBranch = !v;
      break;
    case 0x0f:
      work.takeBranch = v;
      break;
    }
  }

  work.immediate = 0;
  if ((opcode >= 0x05 && opcode <= 0x0f) || (opcode & 0xf0) == 0xa0 || opcode >= 0xf0) {
    work.immediate = pipeline;
    ++r[15];
    BeginFetch(r[15], Phase::ImmediateLow);
    return;
  }
  goto Execute;
}
ImmediateLow:
  pipeline = FetchResult();
  pcWritten = false;
  if (opcode >= 0xf0) {
    work.immediate |= uint16_t(pipeline) << 8;
    ++r[15];
    BeginFetch(r[15], Phase::ImmediateHigh);
    return;
  }
  goto Execute;
ImmediateHigh:
  pipeline = FetchResult();
  pcWritten = false;
  goto Execute;
Execute: {
  work.operand = r[n];
  work.clearPrefix = true;
  work.phase = Phase::Finish;
  const auto mode = work.mode;
  const auto input = work.input;
  const auto immediate = work.immediate;
  const auto operandRegister = work.operand;
  if (opcode >= 0x05 && opcode <= 0x0f) {
    if (work.takeBranch)
      SetRegister(15, uint16_t(r[15] + int8_t(immediate)));
    work.clearPrefix = false;
  } else if (opcode >= 0x10 && opcode <= 0x1f) {
    if (status & With)
      SetRegister(n, input);
    else {
      destination = n;
      work.clearPrefix = false;
    }
  } else if (opcode >= 0x20 && opcode <= 0x2f) {
    source = destination = n;
    status |= With;
    work.clearPrefix = false;
  } else if (opcode >= 0x30 && opcode <= 0x3b) {
    lastRam = r[n];
    work.base = uint32_t(ramBank) << 16;
    if (BeginRamWrite(work.base | lastRam, uint8_t(input), Phase::StoreLow))
      return;
  } else if (opcode >= 0x40 && opcode <= 0x4b) {
    lastRam = r[n];
    work.base = uint32_t(ramBank) << 16;
    if (BeginRamRead(work.base | lastRam, Phase::LoadLow))
      return;
  } else if (opcode >= 0x50 && opcode <= 0x6f) {
    const bool subtract = opcode >= 0x60;
    const uint16_t right = (mode & 2) && !(subtract && mode == 3) ? uint16_t(n) : r[n];
    const int borrow = subtract && mode == 1 && !(status & Carry);
    const int carry = !subtract && (mode & 1) && (status & Carry);
    const int32_t value =
        subtract ? int32_t(input) - right - borrow : int32_t(input) + right + carry;
    Flag(Carry, subtract ? value >= 0 : value > 0xffff);
    Flag(Overflow,
         ((subtract ? (input ^ right) : ~(input ^ right)) & (input ^ value) & 0x8000) != 0);
    ResultFlags(uint16_t(value));
    if (!(subtract && mode == 3))
      SetRegister(destination, uint16_t(value));
  } else if ((opcode >= 0x71 && opcode <= 0x7f) || (opcode >= 0xc1 && opcode <= 0xcf)) {
    const uint16_t right = (mode & 2) ? uint16_t(n) : r[n];
    const auto value = uint16_t(opcode < 0x80 ? input & ((mode & 1) ? uint16_t(~right) : right)
                                : (mode & 1)  ? input ^ right
                                              : input | right);
    SetRegister(destination, value);
    ResultFlags(value);
  } else if (opcode >= 0x80 && opcode <= 0x8f) {
    const uint8_t right = uint8_t((mode & 2) ? n : r[n]);
    const auto value =
        uint16_t((mode & 1) ? uint8_t(input) * right : int8_t(input) * int8_t(right));
    // Results and prefixes are visible during the remaining multiply clocks.
    SetRegister(destination, value);
    ResultFlags(value);
    ClearPrefix();
    work.clearPrefix = false;
    if (!(config & 0x20) && BeginDelay(CoreCost(), Port::None, Phase::Finish))
      return;
  } else if (opcode >= 0x91 && opcode <= 0x94) {
    r[11] = uint16_t(r[15] + n);
  } else if (opcode >= 0x98 && opcode <= 0x9d) {
    if (mode & 1) {
      programBank = uint8_t(r[n] & 0x7f);
      SetRegister(15, input);
      cacheBase = r[15] & 0xfff0;
      validLines = 0;
    } else
      SetRegister(15, r[n]);
  } else if ((opcode & 0xf0) == 0xa0 || opcode >= 0xf0) {
    if (!mode)
      SetRegister(n, opcode < 0xf0 ? uint16_t(int16_t(int8_t(immediate))) : immediate);
    else {
      lastRam = opcode < 0xf0 ? uint16_t(immediate * 2) : immediate;
      work.base = uint32_t(ramBank) << 16;
      if (mode & 1) {
        if (BeginRamRead(work.base | lastRam, Phase::LoadWordLow))
          return;
      } else {
        if (BeginRamWrite(work.base | lastRam, uint8_t(operandRegister), Phase::StoreWordLow))
          return;
      }
    }
  } else if (opcode >= 0xb0 && opcode <= 0xbf) {
    if (status & With) {
      const uint16_t value = r[n];
      SetRegister(destination, value);
      ResultFlags(value);
      Flag(Overflow, (value & 0x80) != 0);
    } else {
      source = n;
      work.clearPrefix = false;
    }
  } else if ((opcode >= 0xd0 && opcode <= 0xde) || (opcode >= 0xe0 && opcode <= 0xee)) {
    SetRegister(n, uint16_t(r[n] + (opcode < 0xe0 ? 1 : -1)));
    ResultFlags(r[n]);
  } else {
    switch (opcode) {
    case 0x00:
      status &= uint16_t(~Go);
      if (!(config & 0x80)) {
        status |= Irq;
        irqOutput = true;
      }
      pipeline = 1;
      break;
    case 0x01:
      break;
    case 0x02: {
      const uint16_t base = r[15] & 0xfff0;
      if (base != cacheBase) {
        cacheBase = base;
        validLines = 0;
      }
      break;
    }
    case 0x03:
    case 0x04:
    case 0x96:
    case 0x97: {
      uint16_t value;
      const bool carry = (status & Carry) != 0;
      if (opcode == 0x04)
        value = uint16_t((input << 1) | carry);
      else if (opcode == 0x97)
        value = uint16_t((input >> 1) | (carry ? 0x8000 : 0));
      else if (opcode == 0x96)
        value = (mode & 1) && input == 0xffff ? 0 : uint16_t(int16_t(input) >> 1);
      else
        value = input >> 1;
      Flag(Carry, (input & (opcode == 0x04 ? 0x8000 : 1)) != 0);
      SetRegister(destination, value);
      ResultFlags(value);
      break;
    }
    case 0x3c:
      --r[12];
      ResultFlags(r[12]);
      if (r[12])
        SetRegister(15, r[13]);
      break;
    case 0x3d:
    case 0x3e:
    case 0x3f:
      status = uint16_t((status & ~With) | ((opcode - 0x3c) << 8));
      work.clearPrefix = false;
      break;
    case 0x4c:
      work.x = uint8_t(r[1]);
      work.y = uint8_t(r[2]);
      if (mode & 1) {
        if (BeginFlush(1, Phase::RpixFlushFirst))
          return;
      } else {
        const bool transparent =
            !(plotOptions & 1) &&
            (((screenMode & 3) != 3 || (plotOptions & 8)) ? !(color & 15) : !color);
        if (transparent) {
          ++r[1];
          break;
        }
        work.data = color;
        if ((plotOptions & 2) && (screenMode & 3) != 3)
          work.data = uint8_t((((work.x ^ work.y) & 1) ? work.data >> 4 : work.data) & 15);
        work.row = uint16_t(unsigned(work.y) * 32 + work.x / 8);
        if (pixels[0].position != work.row) {
          if (BeginFlush(1, Phase::PlotEvict))
            return;
        } else
          work.phase = Phase::PlotWrite;
      }
      break;
    case 0x4d:
    case 0x4f:
    case 0x95:
    case 0x9e:
    case 0xc0: {
      const uint16_t value = opcode == 0x4d   ? uint16_t((input << 8) | (input >> 8))
                             : opcode == 0x4f ? uint16_t(~input)
                             : opcode == 0x95 ? uint16_t(int16_t(int8_t(input)))
                             : opcode == 0x9e ? uint16_t(input & 0xff)
                                              : uint16_t(input >> 8);
      SetRegister(destination, value);
      ResultFlags(value, opcode == 0x9e || opcode == 0xc0);
      break;
    }
    case 0x4e:
      if (mode & 1)
        plotOptions = uint8_t(input & 31);
      else
        SetColor(uint8_t(input));
      break;
    case 0x70: {
      const uint16_t value = uint16_t((r[7] & 0xff00) | (r[8] >> 8));
      SetRegister(destination, value);
      Flag(Overflow, (value & 0xc0c0) != 0);
      Flag(Sign, (value & 0x8080) != 0);
      Flag(Carry, (value & 0xe0e0) != 0);
      Flag(Zero, (value & 0xf0f0) != 0);
      break;
    }
    case 0x90: {
      work.base = uint32_t(ramBank) << 16;
      if (BeginRamWrite(work.base | lastRam, uint8_t(input), Phase::StoreWordLow))
        return;
      break;
    }
    case 0x9f: {
      const auto product = uint32_t(int32_t(int16_t(input)) * int32_t(int16_t(r[6])));
      if (mode & 1)
        r[4] = uint16_t(product);
      SetRegister(destination, uint16_t(product >> 16));
      ResultFlags(uint16_t(product >> 16));
      Flag(Carry, (product & 0x8000) != 0);
      ClearPrefix();
      work.clearPrefix = false;
      if (BeginDelay(((config & 0x20) ? 3u : 7u) * CoreCost(), Port::None, Phase::Finish))
        return;
      break;
    }
    case 0xdf:
      if (!(mode & 2)) {
        if (BeginRomRead(Phase::RomColor))
          return;
      } else if (mode == 2) {
        if (BeginDelay(ramRemaining, Port::None, Phase::RamBank))
          return;
      } else {
        if (BeginRomRead(Phase::RomBank))
          return;
      }
      break;
    case 0xef:
      if (BeginRomRead(Phase::RomByte))
        return;
      break;
    }
  }

  if (work.phase == Phase::Finish)
    goto Finish;
  goto Dispatch;
}
StoreLow:
StoreWordLow: {
  const bool word = work.phase == Phase::StoreWordLow || !(status & Alt1);
  QueueRamWrite();
  work.phase = Phase::Finish;
  if (word) {
    const unsigned highSource = (opcode & 0xf0) == 0xa0 || opcode >= 0xf0 ? n : source;
    if (BeginRamWrite(work.base | (lastRam ^ 1u), uint8_t(r[highSource] >> 8), Phase::StoreHigh))
      return;
  }
  goto Dispatch;
}
StoreHigh:
  QueueRamWrite();
  work.phase = Phase::Finish;
  goto Dispatch;
LoadLow:
LoadWordLow: {
  const bool word = work.phase == Phase::LoadWordLow || !(status & Alt1);
  work.word = RamByte(work.address);
  if (word) {
    if (BeginRamRead(work.base | (lastRam ^ 1u), Phase::LoadHigh))
      return;
  } else {
    SetRegister(destination, work.word);
    work.phase = Phase::Finish;
  }
  goto Dispatch;
}
LoadHigh:
  work.word |= uint16_t(RamByte(work.address)) << 8;
  SetRegister((opcode & 0xf0) == 0xa0 || opcode >= 0xf0 ? n : destination, work.word);
  work.phase = Phase::Finish;
  goto Dispatch;
RpixFlushFirst:
  if (BeginFlush(0, Phase::RpixFlushSecond))
    return;
  goto Dispatch;
RpixFlushSecond:
  work.word = 0;
  work.address = PixelAddress(work.x, work.y);
  work.depth = Depth();
  work.plane = 0;
  if (BeginDelay(MemoryCost(), Port::Ram, Phase::RpixPlane))
    return;
  goto Dispatch;
RpixPlane: {
  const auto packed = RamByte(work.address + PlaneOffset(work.plane));
  work.word |= uint16_t(((packed >> (7 - (work.x & 7))) & 1) << work.plane);
  if (++work.plane < work.depth) {
    if (BeginDelay(MemoryCost(), Port::Ram, Phase::RpixPlane))
      return;
  } else {
    SetRegister(destination, work.word);
    ResultFlags(work.word);
    work.phase = Phase::Finish;
  }
  goto Dispatch;
}
PlotEvict:
  pixels[1] = pixels[0];
  pixels[0].position = work.row;
  pixels[0].pending = 0;
  work.phase = Phase::PlotWrite;
  goto Dispatch;
PlotWrite:
  pixels[0].colors[work.x & 7] = work.data;
  pixels[0].pending |= uint8_t(0x80 >> (work.x & 7));
  if (pixels[0].pending == 0xff) {
    if (BeginFlush(1, Phase::PlotFull))
      return;
  } else {
    ++r[1];
    work.phase = Phase::Finish;
  }
  goto Dispatch;
PlotFull:
  pixels[1] = pixels[0];
  pixels[0].pending = 0;
  ++r[1];
  work.phase = Phase::Finish;
  goto Dispatch;
RomColor:
  SetColor(romBuffer);
  work.phase = Phase::Finish;
  goto Dispatch;
RamBank:
  ramBank = uint8_t(r[source] & 1);
  work.phase = Phase::Finish;
  goto Dispatch;
RomBank:
  romBank = uint8_t(r[source] & 0x7f);
  work.phase = Phase::Finish;
  goto Dispatch;
RomByte: {
  const auto byte = romBuffer;
  const uint16_t value = work.mode == 0   ? byte
                         : work.mode == 1 ? uint16_t((r[source] & 0xff) | (uint16_t(byte) << 8))
                         : work.mode == 2 ? uint16_t((work.input & 0xff00) | byte)
                                          : uint16_t(int16_t(int8_t(byte)));
  SetRegister(destination, value);
  work.phase = Phase::Finish;
  goto Dispatch;
}
Finish:
  if (work.clearPrefix)
    ClearPrefix();
  // CPU R14 writes remain pending through opcode fetches.
  if (r14Written) {
    r14Written = false;
    StartRomRead();
  }
  if (!pcWritten)
    ++r[15];
  work.phase = Phase::Begin;
  goto Begin;
}
inline uint8_t SuperFx::Impl::ReadIo(uint16_t address) {
  address = uint16_t(0x3000 | (address & 0x3ff));
  if (address >= 0x3100 && address <= 0x32ff)
    return cache[(address - 0x3100 + cacheBase) & 511];
  if (address < 0x3020)
    return uint8_t(r[(address >> 1) & 15] >> ((address & 1) * 8));
  switch (address) {
  case 0x3030:
    return uint8_t(status & StatusMask);
  case 0x3031: {
    const auto value = uint8_t((status & StatusMask) >> 8);
    status &= uint16_t(~Irq);
    irqOutput = false;
    return value;
  }
  case 0x3034:
    return programBank;
  case 0x3036:
    return romBank;
  case 0x303b:
    return 0x04;
  case 0x303c:
    return ramBank;
  case 0x303e:
    return uint8_t(cacheBase);
  case 0x303f:
    return uint8_t(cacheBase >> 8);
  default:
    return 0;
  }
}
inline void SuperFx::Impl::WriteIo(uint16_t address, uint8_t value) {
  address = uint16_t(0x3000 | (address & 0x3ff));
  if (address >= 0x3100 && address <= 0x32ff) {
    const unsigned offset = (address - 0x3100 + cacheBase) & 511;
    cache[offset] = value;
    if ((offset & 15) == 15)
      validLines |= uint32_t(1) << (offset / 16);
    return;
  }
  if (address < 0x3020) {
    const unsigned n = (address >> 1) & 15;
    r[n] = (address & 1) ? uint16_t((r[n] & 0xff) | (uint16_t(value) << 8))
                         : uint16_t((r[n] & 0xff00) | value);
    if (n == 14) {
      r14Written = true;
      StartRomRead();
    }
    if (n == 15)
      pcWritten = true;
    if (address == 0x301f)
      status |= Go;
    return;
  }
  switch (address) {
  case 0x3030: {
    const bool wasRunning = Running();
    status = uint16_t((status & 0xff00) | (value & StatusMask));
    if (wasRunning && !Running()) {
      cacheBase = 0;
      validLines = 0;
    }
    break;
  }
  case 0x3031:
    status = uint16_t((status & 0xff) | ((uint16_t(value) << 8) & StatusMask));
    break;
  case 0x3033:
    backupEnable = value & 1;
    break;
  case 0x3034:
    programBank = value & 0x7f;
    validLines = 0;
    break;
  case 0x3037:
    config = value & 0xa0;
    break;
  case 0x3038:
    screenBase = value;
    break;
  case 0x3039:
    clockSelect = value & 1;
    break;
  case 0x303a:
    screenMode = value & 0x3f;
    break;
  default:
    break;
  }
}
inline bool SuperFx::Impl::CpuRamAddress(uint32_t address, uint32_t &offset) const {
  const unsigned bank = (address >> 16) & 0xff;
  const unsigned low = address & 0xffff;
  if (IoBank(address) && low >= 0x6000 && low < 0x8000) {
    offset = low & 0x1fff;
    return true;
  }
  if (bank == 0x70 || bank == 0x71 || bank == 0xf0 || bank == 0xf1) {
    offset = ((bank & 1) << 16) | low;
    return true;
  }
  return false;
}
inline bool SuperFx::Impl::CpuRomAddress(uint32_t address, size_t &offset) const {
  const unsigned bank = address >> 16, low = address & 0xffff;
  size_t linear = 0;
  if (IoBank(address) && low >= 0x8000) {
    linear = (size_t(bank) << 15) | (low & 0x7fff);
  } else if ((bank >= 0x40 && bank <= 0x5f) || (bank >= 0xc0 && bank <= 0xdf)) {
    linear = address;
  } else
    return false;
  if (rom.empty())
    return false;
  offset = Mirror(linear, rom.size());
  return true;
}
SuperFx::SuperFx(std::span<const uint8_t> rom, std::span<uint8_t> ram)
    : impl_(std::make_unique<Impl>(rom, ram)) {}
SuperFx::~SuperFx() = default;
SuperFx::SuperFx(SuperFx &&) noexcept = default;
SuperFx &SuperFx::operator=(SuperFx &&) noexcept = default;
void SuperFx::Reset() {
  Reset(impl_->rom, impl_->ram);
}
void SuperFx::Reset(std::span<const uint8_t> rom, std::span<uint8_t> ram) {
  const auto masterFrequency = impl_->masterFrequency;
  impl_ = std::make_unique<Impl>(rom, ram);
  impl_->masterFrequency = masterFrequency;
}
void SuperFx::Advance(uint64_t clocks) {
  impl_->Advance(clocks);
}
void SuperFx::SetPal(bool pal) {
  impl_->masterFrequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
}
void SuperFx::AdvanceMasterClocks(uint32_t clocks) {
  impl_->AdvanceMasterClocks(clocks);
}
bool SuperFx::CpuIrqPending() const noexcept {
  return impl_->irqOutput;
}

bool SuperFx::Selects(uint32_t address) noexcept {
  address &= 0xffffff;
  const unsigned low = address & 0xffff, bank = (address >> 16) & 0x7f;
  return SelectsRegisters(address) || (IoBank(address) && low >= 0x6000) ||
         (bank >= 0x40 && bank <= 0x7d) || address >= 0xc00000;
}

bool SuperFx::SelectsRegisters(uint32_t address) noexcept {
  const auto low = uint16_t(address);
  return IoBank(address) && low >= 0x3000 && low <= 0x34ff;
}

uint8_t SuperFx::ReadCpu(uint32_t address, uint8_t openBus) {
  address &= 0xffffff;
  const auto low = uint16_t(address);
  if (SelectsRegisters(address))
    return impl_->ReadIo(low);
  uint32_t offset = 0;
  if (impl_->CpuRamAddress(address, offset)) {
    if (impl_->ram.empty() || (impl_->Running() && (impl_->screenMode & 8)))
      return openBus;
    // CPU expansion addresses are independent of the GSU's RAM bank register.
    return impl_->ram[Mirror(offset, impl_->ram.size())];
  }
  size_t romOffset = 0;
  if (!impl_->CpuRomAddress(address, romOffset))
    return openBus;
  if (impl_->Running() && (impl_->screenMode & 0x10)) {
    constexpr std::array<uint8_t, 16> driven{0, 1, 0, 1, 4, 1, 0, 1, 0, 1, 8, 1, 0, 1, 12, 1};
    return driven[low & 15];
  }
  return impl_->rom[romOffset];
}

void SuperFx::WriteCpu(uint32_t address, uint8_t value) {
  address &= 0xffffff;
  const auto low = uint16_t(address);
  if (SelectsRegisters(address)) {
    impl_->WriteIo(low, value);
    return;
  }
  uint32_t offset = 0;
  if (impl_->CpuRamAddress(address, offset) && !impl_->ram.empty())
    impl_->ram[Mirror(offset, impl_->ram.size())] = value;
}

std::optional<size_t> SuperFx::CheatRomOffset(uint32_t address) const {
  size_t offset = 0;
  if (impl_->CpuRomAddress(address & 0xffffff, offset))
    return offset;
  return {};
}

std::span<const uint8_t> SuperFx::InspectCache() const noexcept {
  return impl_->cache;
}

} // namespace snes::core
