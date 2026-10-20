#include "snes/core/gameboy/Cartridge.hpp"

#include <algorithm>
#include <bit>
#include <ctime>
#include <utility>

namespace snes::core::gameboy {
namespace {
uint64_t AdvanceField(uint8_t &value, unsigned limit, uint64_t ticks) {
  unsigned distance = (limit - value) & 255;
  if (distance == 0)
    distance = 256;
  if (ticks < distance) {
    value = uint8_t(value + ticks);
    return 0;
  }
  ticks -= distance;
  value = uint8_t(ticks % limit);
  return 1 + ticks / limit;
}
uint64_t LittleEndian(std::span<const uint8_t> bytes) {
  uint64_t value = 0;
  for (size_t i = 0; i < bytes.size(); ++i)
    value |= uint64_t(bytes[i]) << (i * 8);
  return value;
}
} // namespace

uint64_t Cartridge::Now() const {
  return timeSource_ ? timeSource_() : uint64_t(std::time(nullptr));
}

void Cartridge::SetTimeSource(TimeSource source) {
  timeSource_ = std::move(source);
}

bool Cartridge::Load(std::span<const uint8_t> image, std::string *) {
  const size_t romSize = std::bit_ceil(std::max(image.size(), size_t(0x8000)));
  std::vector<uint8_t> program(romSize, 0xff);
  std::copy(image.begin(), image.end(), program.begin());
  uint8_t type = program[0x147];
  const auto isMulticart = [](unsigned value) { return value >= 0x0b && value <= 0x0d; };
  if (isMulticart(type)) {
    std::rotate(program.begin(), program.begin() + 0x8000, program.end());
  } else {
    const auto tail = romSize - 0x8000;
    if (std::equal(program.begin() + 0x104, program.begin() + 0x134,
                   program.begin() + tail + 0x104)) {
      const auto tailType = program[tail + 0x147];
      if (isMulticart(tailType) || tailType == 0x11)
        type = tailType == 0x11 ? 0x0b : tailType;
    }
  }
  const uint8_t headerRamCode = program[0x149];
  const uint8_t ramCode = program[(isMulticart(type) ? romSize - 0x8000 : 0) + 0x149];
  Mapper mapper = Mapper::None;
  bool ram = false, battery = false, clock = false, rumble = false;
  switch (type) {
  case 1:
  case 2:
  case 3:
    mapper = Mapper::Mbc1;
    ram = type != 1 || ramCode != 0;
    battery = type == 3;
    break;
  case 5:
  case 6:
    mapper = Mapper::Mbc2;
    ram = true;
    battery = type == 6;
    break;
  case 8:
  case 9:
    ram = true;
    battery = type == 9;
    break;
  case 0x0f:
  case 0x10:
  case 0x11:
  case 0x12:
  case 0x13:
    mapper = Mapper::Mbc3;
    ram = type == 0x10 || type >= 0x12 || ramCode != 0;
    battery = type == 0x0f || type == 0x10 || type == 0x13;
    clock = type == 0x0f || type == 0x10;
    break;
  case 0x0b:
  case 0x0c:
  case 0x0d:
    mapper = Mapper::Mmm01;
    ram = type != 0x0b || headerRamCode != 0;
    battery = type == 0x0d;
    break;
  case 0x19:
  case 0x1a:
  case 0x1b:
  case 0x1c:
  case 0x1d:
  case 0x1e:
    mapper = Mapper::Mbc5;
    ram = (type != 0x19 && type != 0x1c) || ramCode != 0;
    battery = type == 0x1b || type == 0x1e;
    rumble = type >= 0x1c;
    break;
  case 0x22:
    mapper = Mapper::Mbc7;
    ram = battery = true;
    break;
  case 0xff:
    mapper = Mapper::Huc1;
    ram = battery = true;
    break;
  case 0xfe:
    mapper = Mapper::Huc3;
    ram = battery = clock = true;
    break;
  case 0xfc:
    mapper = Mapper::Camera;
    ram = battery = true;
    break;
  default:
    break;
  }
  if (type == 0 && image.size() > 0x8000) {
    mapper = Mapper::Mbc3;
    ram = ramCode != 0;
  }
  if (program[0x147] == 0xbc && program[0x149] == 0xc1 && program[0x14a] == 0x65) {
    mapper = Mapper::Tpp1;
    ram = clock = rumble = true;
    battery = (program[0x153] & 8) != 0;
  }
  const unsigned tpp1RamCode = program[0x152];
  rom_ = std::move(program);
  mapper_ = mapper;
  battery_ = battery;
  clock_ = clock;
  hasRumble_ = rumble;
  constexpr size_t ramSizes[] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};
  const size_t tpp1RamSize =
      tpp1RamCode >= 1 && tpp1RamCode <= 9 ? size_t(8192) << (tpp1RamCode - 1) : 0;
  const size_t ramSize = !ram                     ? 0
                         : mapper == Mapper::Mbc2 ? 512
                         : mapper == Mapper::Mbc7 ? 256
                         : mapper == Mapper::Tpp1 ? tpp1RamSize
                         : ramCode < 6            ? ramSizes[ramCode]
                                                  : 0;
  if (ram_.size() != ramSize)
    ram_.assign(ramSize, 0xff);
  multicart_ = mapper == Mapper::Mbc1 && romSize >= 0x44000 &&
               std::equal(rom_.begin() + 0x104, rom_.begin() + 0x134, rom_.begin() + 0x40104);
  extendedMbc3_ = mapper == Mapper::Mbc3 && (romSize > 0x200000 || ramSize > 0x8000);
  Reset();
  return true;
}

void Cartridge::Reset() {
  lowBank_ = (mapper_ == Mapper::Mbc5 || mapper_ == Mapper::Camera) ? 1 : 0;
  highBank_ = ramBank_ = huc3Mode_ = 0;
  mode_ = rtcMapped_ = rumble_ = false;
  ramEnabled_ = mapper_ == Mapper::Huc1;
  motionEnabled_ = bankRegistersWritten_ = cameraRegisters_ = false;
  motion_.Reset();
  infrared_.Reset();
  camera_.Reset();
  multicartRegisters_ = {};
  lastTime_ = Now();
  huc3Clock_.Reset(lastTime_);
  tpp1_.Reset(lastTime_);
}

unsigned Cartridge::RomBank(bool upper) const noexcept {
  switch (mapper_) {
  case Mapper::Mbc1: {
    const unsigned high = unsigned(highBank_) << (multicart_ ? 4 : 5);
    if (!upper)
      return mode_ ? high : 0;
    return high | (multicart_ ? lowBank_ & 15 : lowBank_) | (lowBank_ == 0 ? 1 : 0);
  }
  case Mapper::Mbc2:
    return upper ? (lowBank_ == 0 ? 1 : lowBank_) : 0;
  case Mapper::Mbc3: {
    const unsigned bank = lowBank_ & (extendedMbc3_ ? 255 : 127);
    return upper ? (bank == 0 ? 1 : bank) : 0;
  }
  case Mapper::Mbc5:
    return upper ? lowBank_ | (unsigned(highBank_) << 8) : 0;
  case Mapper::Mbc7:
  case Mapper::Camera:
    return upper ? lowBank_ : 0;
  case Mapper::Tpp1:
    return upper ? tpp1_.RomBank() : 0;
  case Mapper::Huc1:
  case Mapper::Huc3:
    return upper ? (bankRegistersWritten_ ? lowBank_ : 1) : 0;
  case Mapper::Mmm01: {
    const auto &m = multicartRegisters_;
    if (!m.locked)
      return unsigned(rom_.size() / 0x4000) - (upper ? 1 : 2);
    const unsigned high = unsigned(m.romHigh) << 7;
    const unsigned middle = unsigned(m.multiplex ? m.ramLow : m.romMid) << 5;
    const unsigned lower =
        high | (m.romLow & (unsigned(m.romMask) << 1)) | (m.multiplex && m.mode ? 0 : middle);
    const unsigned bank = high | middle | m.romLow;
    return upper ? bank + (bank == lower ? 1 : 0) : lower;
  }
  default:
    return upper ? 1 : 0;
  }
}

std::optional<unsigned> Cartridge::RamBank() const noexcept {
  if (mapper_ == Mapper::Mbc1)
    return mode_ && !multicart_ ? highBank_ : 0;
  if (mapper_ == Mapper::Mbc3 && !extendedMbc3_) {
    if (clock_ && ramBank_ > 3)
      return std::nullopt;
    return ramBank_ & 3;
  }
  if (mapper_ == Mapper::Mmm01) {
    const auto &m = multicartRegisters_;
    if (!m.locked)
      return 0;
    return (m.multiplex ? m.romMid : m.ramLow) | (unsigned(m.ramHigh) << 2);
  }
  return ramBank_;
}

std::optional<uint8_t> Cartridge::Read(uint16_t address) {
  if (address < 0x8000) {
    if (rom_.empty())
      return 0xff;
    const size_t offset = size_t(RomBank(address >= 0x4000)) * 0x4000 + (address & 0x3fff);
    return rom_[offset & (rom_.size() - 1)];
  }
  if (mapper_ == Mapper::Tpp1 && address >= 0xa000 && address < 0xc000)
    return tpp1_.Read(address, ram_);
  if (mapper_ == Mapper::Mbc7 && address >= 0xa000 && address < 0xc000)
    return ramEnabled_ && motionEnabled_ && address < 0xb000 ? motion_.Read(address) : 0xff;
  if (mapper_ == Mapper::Camera && address >= 0xa000 && address < 0xc000 && cameraRegisters_)
    return camera_.Read(address);
  if (mapper_ == Mapper::Huc3 && address >= 0xa000 && address < 0xc000) {
    switch (huc3Mode_) {
    case 0xc:
      return huc3Clock_.Read();
    case 0xe:
      return uint8_t(infrared_.Detected());
    case 0:
    case 0xa:
      break;
    default:
      return 1;
    }
  } else if (address < 0xa000 || address >= 0xc000 || (!ramEnabled_ && mapper_ != Mapper::Camera)) {
    return std::nullopt;
  }
  if (mapper_ == Mapper::Huc1 && mode_)
    return uint8_t(0xc0 | unsigned(infrared_.Detected()));
  if (clock_ && rtcMapped_) {
    if (ramBank_ >= rtc_.size())
      return std::nullopt;
    latched_[0] &= 0x3f;
    latched_[1] &= 0x3f;
    latched_[2] &= 0x1f;
    latched_[4] &= 0xc1;
    return latched_[ramBank_];
  }
  if (ram_.empty())
    return std::nullopt;
  if (mapper_ == Mapper::Camera) {
    if (camera_.Busy())
      return 0;
    if (ramBank_ == 0 && address >= 0xa100 && address < 0xaf00)
      return camera_.ReadImage(uint16_t(address - 0xa100));
  }
  const auto bank = RamBank();
  if (!bank)
    return 0xff;
  const auto value = ram_[(size_t(*bank) * 0x2000 + (address & 0x1fff)) & (ram_.size() - 1)];
  return mapper_ == Mapper::Mbc2 ? uint8_t(value | 0xf0) : value;
}

void Cartridge::Write(uint16_t address, uint8_t value) {
  if (address < 0x8000) {
    switch (mapper_) {
    case Mapper::Mbc1:
      if (address < 0x2000)
        ramEnabled_ = (value & 15) == 10;
      else if (address < 0x4000)
        lowBank_ = value & 31;
      else if (address < 0x6000)
        highBank_ = value & 3;
      else
        mode_ = value & 1;
      break;
    case Mapper::Mbc2:
      if (address < 0x4000) {
        if (address & 0x100)
          lowBank_ = value & 15;
        else
          ramEnabled_ = (value & 15) == 10;
      }
      break;
    case Mapper::Mbc3:
      if (address < 0x2000)
        ramEnabled_ = (value & 15) == 10;
      else if (address < 0x4000)
        lowBank_ = value;
      else if (address < 0x6000) {
        ramBank_ = value & 7;
        rtcMapped_ = value & 8;
      } else {
        latched_ = rtc_;
      }
      break;
    case Mapper::Mbc5:
      if (address < 0x2000)
        ramEnabled_ = value == 10;
      else if (address < 0x3000)
        lowBank_ = value;
      else if (address < 0x4000)
        highBank_ = value & 1;
      else if (address < 0x6000) {
        ramBank_ = value & (hasRumble_ ? 7 : 15);
        if (hasRumble_)
          rumble_ = value & 8;
      }
      break;
    case Mapper::Mmm01:
      WriteMulticart(address, value);
      break;
    case Mapper::Tpp1:
      tpp1_.Write(address, value, ram_);
      break;
    case Mapper::Huc1:
      bankRegistersWritten_ = true;
      if (address < 0x2000)
        mode_ = (value & 15) == 14;
      else if (address < 0x4000)
        lowBank_ = value & 63;
      else if (address < 0x6000)
        ramBank_ = value & 7;
      break;
    case Mapper::Huc3:
      bankRegistersWritten_ = true;
      if (address < 0x2000) {
        huc3Mode_ = value & 15;
        ramEnabled_ = huc3Mode_ == 0xa;
      } else if (address < 0x4000) {
        lowBank_ = value & 127;
      } else if (address < 0x6000) {
        ramBank_ = value & 15;
      }
      break;
    case Mapper::Camera:
      if (address < 0x2000)
        ramEnabled_ = (value & 15) == 10;
      else if (address < 0x4000)
        lowBank_ = value;
      else if (address < 0x6000) {
        ramBank_ = value & 15;
        cameraRegisters_ = (value & 16) != 0;
      }
      break;
    case Mapper::Mbc7:
      if (address < 0x2000)
        ramEnabled_ = value == 10;
      else if (address < 0x4000)
        lowBank_ = value;
      else if (address < 0x6000)
        motionEnabled_ = value == 0x40;
      break;
    default:
      break;
    }
    return;
  }
  if (mapper_ == Mapper::Tpp1 && address >= 0xa000 && address < 0xc000) {
    tpp1_.Write(address, value, ram_);
    return;
  }
  if (mapper_ == Mapper::Mbc7 && address >= 0xa000 && address < 0xc000) {
    if (ramEnabled_ && motionEnabled_ && address < 0xb000)
      motion_.Write(address, value, ram_);
    return;
  }
  if (mapper_ == Mapper::Camera && address >= 0xa000 && address < 0xc000) {
    if (cameraRegisters_) {
      camera_.Write(address, value);
      return;
    }
    if (camera_.Busy())
      return;
  }
  if (mapper_ == Mapper::Huc3 && address >= 0xa000 && address < 0xc000) {
    if (huc3Mode_ == 0xb)
      huc3Clock_.Write(value);
    else if (huc3Mode_ == 0xe)
      infrared_.SetOutput((value & 1) != 0);
    // Upper RAM selections ignore writes.
    if (huc3Mode_ != 0xa || (ramBank_ & 8))
      return;
  }
  if (address < 0xa000 || address >= 0xc000 || !ramEnabled_)
    return;
  if (mapper_ == Mapper::Huc1 && mode_) {
    infrared_.SetOutput((value & 1) != 0);
    return;
  }
  if (clock_ && rtcMapped_) {
    if (ramBank_ < rtc_.size()) {
      rtc_[ramBank_] = value;
      if (ramBank_ == 0)
        rtcClocks_ = 0;
    }
    return;
  }
  const auto bank = RamBank();
  if (bank && !ram_.empty())
    ram_[(size_t(*bank) * 0x2000 + (address & 0x1fff)) & (ram_.size() - 1)] = value;
}

void Cartridge::WriteMulticart(uint16_t address, uint8_t value) noexcept {
  auto &m = multicartRegisters_;
  switch (address >> 13) {
  case 0:
    ramEnabled_ = (value & 15) == 10;
    if (!m.locked) {
      m.ramMask = (value >> 4) & 3;
      m.locked = value & 0x40;
    }
    break;
  case 1: {
    if (!m.locked)
      m.romMid = (value >> 5) & 3;
    const unsigned retained = unsigned(m.romMask) << 1;
    m.romLow = uint8_t(((m.romLow & retained) | (value & ~retained)) & 31);
    break;
  }
  case 2:
    m.ramLow = uint8_t((value | ~m.ramMask) & 3);
    if (!m.locked) {
      m.ramHigh = (value >> 2) & 3;
      m.romHigh = (value >> 4) & 3;
      m.modeDisabled = value & 0x40;
    }
    break;
  case 3:
    if (!m.modeDisabled)
      m.mode = value & 1;
    if (!m.locked) {
      m.romMask = (value >> 2) & 15;
      m.multiplex = value & 0x40;
    }
    break;
  }
}

void Cartridge::SetClockRate(uint32_t clocksPerSecond) noexcept {
  rtcClockRate_ = std::max(clocksPerSecond, 32u);
}

void Cartridge::Advance(unsigned clocks, bool sleeping) {
  while (clocks) {
    const auto batch = std::min(clocks, 64u);
    AdvanceCamera(batch, sleeping);
    AdvanceClock(batch);
    clocks -= batch;
  }
}

void Cartridge::AdvanceCamera(unsigned clocks, bool sleeping) {
  if (mapper_ == Mapper::Camera)
    camera_.Advance(clocks, sleeping);
}

void Cartridge::AdvanceClock(unsigned clocks) {
  while (clocks) {
    const auto batch = std::min(clocks, 64u);
    if (mapper_ == Mapper::Huc1 || mapper_ == Mapper::Huc3)
      infrared_.Advance(batch, mapper_ == Mapper::Huc1 ? mode_ : huc3Mode_ == 0xe);
    AdvanceRtc(batch);
    clocks -= batch;
  }
}

void Cartridge::AdvanceRtc(unsigned clocks) {
  if (!clock_)
    return;
  if (mapper_ == Mapper::Tpp1) {
    if (tpp1_.Advance(clocks, rtcClockRate_))
      tpp1_.Synchronize(Now());
    return;
  }
  rtcClocks_ += uint64_t(clocks) * 2;
  const auto interval = rtcClockRate_ / 16;
  if (rtcClocks_ >= interval) {
    rtcClocks_ %= interval;
    SynchronizeRtc(Now());
  }
}

void Cartridge::AdvanceDays(uint64_t days) noexcept {
  const unsigned before = rtc_[3] | ((rtc_[4] & 1u) << 8);
  const auto total = before + days;
  rtc_[3] = uint8_t(total);
  rtc_[4] = uint8_t((rtc_[4] & 0xfe) | ((total >> 8) & 1) | (total >= 512 ? 0x80 : 0));
}

void Cartridge::SynchronizeRtc(uint64_t timestamp) {
  if (mapper_ == Mapper::Tpp1) {
    tpp1_.Synchronize(timestamp);
    return;
  }
  if (mapper_ == Mapper::Huc3) {
    huc3Clock_.Synchronize(timestamp);
    return;
  }
  if (!clock_ || (rtc_[4] & 0x40) || timestamp <= lastTime_)
    return;
  auto seconds = timestamp - lastTime_;
  lastTime_ = timestamp;
  const auto days = (seconds - 1) / 86400;
  AdvanceDays(days);
  seconds -= days * 86400;
  const auto minutes = AdvanceField(rtc_[0], 60, seconds);
  const auto hours = AdvanceField(rtc_[1], 60, minutes);
  AdvanceDays(AdvanceField(rtc_[2], 24, hours));
}

std::vector<uint8_t> Cartridge::SaveBattery() const {
  if (!battery_ || (ram_.empty() && !clock_))
    return {};
  std::vector<uint8_t> bytes = ram_;
  if (mapper_ == Mapper::Tpp1) {
    const auto clock = tpp1_.Save(Now());
    bytes.insert(bytes.end(), clock.begin(), clock.end());
    return bytes;
  }
  if (mapper_ == Mapper::Huc3) {
    const auto clock = huc3Clock_.Save();
    bytes.insert(bytes.end(), clock.begin(), clock.end());
    return bytes;
  }
  if (clock_) {
    const auto offset = bytes.size();
    bytes.resize(offset + 48);
    for (unsigned i = 0; i < 5; ++i) {
      bytes[offset + i * 4] = rtc_[i];
      bytes[offset + 20 + i * 4] = latched_[i];
    }
    const auto timestamp = Now();
    for (unsigned i = 0; i < 8; ++i)
      bytes[offset + 40 + i] = uint8_t(timestamp >> (i * 8));
  }
  return bytes;
}

void Cartridge::LoadBattery(std::span<const uint8_t> bytes) {
  const auto ramSize = ram_.size();
  std::copy_n(bytes.begin(), std::min(bytes.size(), ramSize), ram_.begin());
  const auto now = Now();
  if (mapper_ == Mapper::Tpp1) {
    tpp1_.Load(bytes.subspan(std::min(bytes.size(), ramSize)), now);
    return;
  }
  if (mapper_ == Mapper::Huc3) {
    huc3Clock_.Load(bytes.subspan(std::min(bytes.size(), ramSize)), now);
    return;
  }
  bool validClock = false;
  if (bytes.size() > ramSize) {
    const auto data = bytes.subspan(ramSize);
    if (data.size() == 44 || data.size() == 48) {
      for (unsigned i = 0; i < 5; ++i) {
        rtc_[i] = data[i * 4];
        latched_[i] = data[20 + i * 4];
      }
      lastTime_ = LittleEndian(data.subspan(40));
      validClock = true;
    } else if (data.size() == 13) {
      std::copy_n(data.begin(), 5, rtc_.begin());
      latched_ = rtc_;
      lastTime_ = LittleEndian(data.subspan(5, 8));
      validClock = true;
    }
  }
  if (!validClock || lastTime_ < 852076800 || lastTime_ > now) {
    lastTime_ = now;
    rtc_[4] |= 0x80;
  } else {
    SynchronizeRtc(now);
  }
}

} // namespace snes::core::gameboy
