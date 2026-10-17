#include "snes/core/Bsx.hpp"
#include "snes/core/BsMemory.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <utility>
#include <vector>

namespace snes::core {
namespace {
size_t Fold(size_t address, size_t size) {
  if (!size || address < size)
    return size ? address : 0;
  const size_t top = std::bit_floor(address);
  return size > top ? top + Fold(address - top, size - top) : Fold(address - top, size);
}
} // namespace

struct Bsx::Impl {
  enum class Region { Open, Mapper, Radio, Bios, Pack, Psram, Sram };
  struct Location {
    Region kind = Region::Open;
    size_t offset = 0;
  };
  using StreamKey = std::pair<uint16_t, uint8_t>;
  struct Receiver {
    bool prefixEnabled = false, dataEnabled = false, loaded = false, first = false;
    uint8_t nextSequence = 0;
    StreamKey asset{};
    size_t cursor = 0, remaining = 0;
  };

  std::span<const uint8_t> bios;
  std::span<uint8_t> pack, sram;
  BsMemory flash;
  std::vector<uint8_t> psram = std::vector<uint8_t>(0x80000, 0xff);
  std::array<uint8_t, 16> pending{}, active{};
  std::array<uint8_t, 24> radio{};
  bool recordedStreams = false;
  std::array<Receiver, 2> receivers{};
  std::map<StreamKey, std::vector<uint8_t>> assets;
  std::function<BroadcastTime()> timeSource;
  std::array<uint8_t, 23> clockRecord{};
  unsigned clockIndex = 0;

  Impl(std::span<const uint8_t> image, std::span<uint8_t> cartridge, std::span<uint8_t> save)
      : bios(image), pack(cartridge), sram(save),
        flash(cartridge, cartridge.size() > 0xff06 && cartridge[0xff00] == 'M' &&
                             cartridge[0xff02] == 'P' && (cartridge[0xff06] & 0xf0) == 0x70) {
    Reset();
  }
  void Reset() {
    flash.Reset();
    flash.SetWritable(false);
    pending = {};
    for (unsigned bit : {2u, 3u, 5u, 6u, 7u, 8u, 9u, 11u})
      pending[bit] = 0x80;
    active = pending;
    radio = {};
    receivers = {};
    clockIndex = 0;
    clockRecord = {};
  }

  Location Locate(uint32_t address) const {
    const unsigned bank = address >> 16;
    const unsigned halfBank = bank & 0x7f;
    const unsigned offset = address & 0xffff;
    if (bank == 0x7e || bank == 0x7f)
      return {};
    if (bank < 16 && (offset & 0xf000) == 0x5000)
      return {Region::Mapper, bank};
    if (bank >= 0x10 && bank <= 0x17 && (offset & 0xf000) == 0x5000)
      return {Region::Sram, (bank << 12) | (offset & 0xfff)};
    if (halfBank < 0x40 && offset >= 0x2188 && offset <= 0x219f)
      return {Region::Radio, offset - 0x2188};

    const bool high = active[2] != 0;
    const bool ramEnabled = active[bank < 0x80 ? 3 : 4] != 0;
    if (halfBank < 0x40 && offset < 0x8000) {
      if (high && ramEnabled && halfBank >= 0x20 && offset >= 0x6000)
        return {Region::Psram, ((bank & 0x3f) << 13) | (offset & 0x1fff)};
      return {};
    }
    if (halfBank < 0x40 && active[bank < 0x80 ? 7 : 8])
      return {Region::Bios, ((bank & 0x3f) << 15) | (offset & 0x7fff)};

    if (ramEnabled) {
      const unsigned position = unsigned(active[5] != 0) + 2 * unsigned(active[6] != 0);
      if (high) {
        if ((bank & 0x3f) >= position * 16 && (bank & 0x3f) < position * 16 + 8)
          return {Region::Psram, ((bank & 7) << 16) | offset};
      } else if ((halfBank >= position * 32 && halfBank < position * 32 + 16) ||
                 (halfBank >= 0x70 && offset < 0x8000)) {
        return {Region::Psram, ((bank & 15) << 15) | (offset & 0x7fff)};
      }
    }
    if (active[bank < 0x80 ? 9 : 10]) {
      const unsigned position = unsigned(active[11] != 0);
      const bool hole = high ? (bank & 0x3f) >= position * 32 && (bank & 0x3f) < position * 32 + 16
                             : halfBank >= position * 64 && halfBank < position * 64 + 32;
      if (hole)
        return {};
    }
    const size_t packOffset = high ? address & 0x3fffff : ((bank & 0x3f) << 15) | (offset & 0x7fff);
    return {Region::Pack, packOffset};
  }

  uint8_t ReadMapper(unsigned index) const {
    if (index == 1)
      return pending[1];
    return index >= 2 && index < 14 ? active[index] : 0;
  }
  void WriteMapper(unsigned index, uint8_t value) {
    const uint8_t bit = value & 0x80;
    if (index >= 1 && index < 14)
      pending[index] = bit;
    if (index == 14 && bit) {
      for (unsigned reg = 2; reg <= 13; ++reg)
        active[reg] = pending[reg];
      flash.SetWritable(active[13] != 0);
    }
  }

  uint8_t ReadPack(size_t offset, uint8_t openBus) const {
    return pack.empty() ? openBus : flash.Read(uint32_t(Fold(offset, pack.size())), openBus);
  }
  void WritePack(size_t offset, uint8_t value) {
    if (active[12] && !pack.empty())
      flash.Write(uint32_t(Fold(offset, pack.size())), value);
  }

  uint16_t Channel(unsigned receiver) const {
    const unsigned base = receiver * 6;
    return uint16_t(radio[base] | (uint16_t(radio[base + 1]) << 8));
  }
  void SelectAsset(unsigned receiver) {
    auto &state = receivers[receiver];
    auto key = StreamKey{Channel(receiver), state.nextSequence++};
    auto asset = assets.find(key);
    if (asset == assets.end() && key.second != 0) {
      key.second = 0;
      state.nextSequence = 1;
      asset = assets.find(key);
    }
    state.loaded = asset != assets.end();
    state.remaining = 0;
    state.cursor = 0;
    state.first = false;
    radio[receiver * 6 + 3] = 0;
    if (!state.loaded)
      return;
    state.asset = key;
    state.remaining = (asset->second.size() + 21) / 22;
    state.first = true;
    radio[receiver * 6 + 5] = 0;
  }
  uint8_t ClockByte() {
    if (clockIndex == 0) {
      const BroadcastTime time = timeSource ? timeSource() : BroadcastTime{};
      clockRecord = {};
      if (recordedStreams)
        clockRecord[4] = 0x10;
      clockRecord[5] = clockRecord[6] = 1;
      clockRecord[10] = time.second;
      clockRecord[11] = time.minute;
      clockRecord[12] = time.hour;
      if (recordedStreams) {
        clockRecord[13] = time.weekday;
        clockRecord[14] = time.day;
        clockRecord[15] = time.month;
        clockRecord[16] = uint8_t(time.year);
        clockRecord[17] = uint8_t(time.year >> 8);
      }
    }
    const uint8_t value = clockRecord[clockIndex];
    clockIndex = (clockIndex + 1) % (recordedStreams ? 23u : 18u);
    return value;
  }
  uint8_t ReadRadio(unsigned reg, uint8_t openBus) {
    if (!recordedStreams) {
      switch (reg) {
      case 0:
      case 1:
      case 2:
      case 4:
      case 6:
      case 7:
      case 8:
      case 12:
      case 14:
      case 15:
      case 17:
        return radio[reg];
      case 10:
        return ClockByte();
      case 11:
        return radio[reg] & ~0x0c;
      default:
        return openBus;
      }
    }
    if (reg >= 18)
      return openBus;
    if (reg >= 12)
      return radio[reg];
    const unsigned receiver = reg / 6, field = reg % 6, base = receiver * 6;
    auto &state = receivers[receiver];
    const bool clock = Channel(receiver) == 0;
    switch (field) {
    case 0:
    case 1:
      return radio[reg];
    case 2:
      if (!state.prefixEnabled || !state.dataEnabled)
        return 0;
      if (clock)
        return 1;
      if (!state.remaining)
        SelectAsset(receiver);
      return uint8_t(std::min(state.remaining, size_t(127)));
    case 3:
      if (!state.prefixEnabled)
        return 0;
      if (clock)
        radio[reg] = 0x90;
      else {
        if (!state.loaded || !state.remaining)
          return 0;
        radio[reg] = uint8_t((state.first ? 0x10 : 0) | (state.remaining == 1 ? 0x80 : 0));
        state.first = false;
        --state.remaining;
      }
      radio[base + 5] |= radio[reg];
      return radio[reg];
    case 4: {
      if (!state.dataEnabled)
        return 0;
      if (clock)
        return ClockByte();
      if (!state.loaded)
        return 0;
      const auto asset = assets.find(state.asset);
      if (asset == assets.end() || state.cursor >= asset->second.size())
        return 0xff;
      return asset->second[state.cursor++];
    }
    case 5:
      return std::exchange(radio[reg], 0);
    }
    return 0;
  }
  void WriteRadio(unsigned reg, uint8_t value) {
    if (!recordedStreams) {
      switch (reg) {
      case 0:
      case 1:
      case 2:
      case 3:
      case 4:
      case 6:
      case 11:
      case 12:
      case 15:
      case 17:
        radio[reg] = value;
        break;
      case 7:
        radio[6] = uint8_t(radio[7] - (radio[6] >> 1));
        radio[7] >>= 1;
        break;
      case 9:
        radio[reg] = value;
        clockIndex = 0;
        break;
      case 10:
        radio[8] = 0x80;
        break;
      default:
        break;
      }
      return;
    }
    if (reg >= 18)
      return;
    if (reg >= 12) {
      if (reg == 12)
        radio[reg] = value & 15;
      if (reg == 15)
        radio[reg] = value;
      return;
    }
    const unsigned receiver = reg / 6, field = reg % 6;
    auto &state = receivers[receiver];
    switch (field) {
    case 0:
    case 1:
      radio[reg] = field == 1 ? value & 0x3f : value;
      state.loaded = false;
      state.cursor = state.remaining = state.nextSequence = 0;
      radio[receiver * 6 + 3] = radio[receiver * 6 + 5] = 0;
      break;
    case 3:
      state.prefixEnabled = value != 0;
      break;
    case 4:
      state.dataEnabled = value != 0;
      if (Channel(receiver) == 0)
        clockIndex = 0;
      break;
    default:
      break;
    }
  }
};

Bsx::Bsx(std::span<const uint8_t> bios, std::span<uint8_t> pack, std::span<uint8_t> sram)
    : impl_(std::make_unique<Impl>(bios, pack, sram)) {}
Bsx::~Bsx() = default;
Bsx::Bsx(Bsx &&) noexcept = default;
Bsx &Bsx::operator=(Bsx &&) noexcept = default;
void Bsx::Reset() {
  impl_->Reset();
}
void Bsx::SetPal(bool pal) {
  impl_->flash.SetPal(pal);
}
void Bsx::Advance(uint32_t masterClocks) {
  impl_->flash.Advance(masterClocks);
}
std::vector<uint8_t> Bsx::FlashMetadata() const {
  return impl_->flash.Metadata();
}
bool Bsx::LoadFlashMetadata(std::span<const uint8_t> bytes) {
  return impl_->flash.LoadMetadata(bytes);
}
bool Bsx::CpuIrqPending() const noexcept {
  return false;
}
std::span<uint8_t> Bsx::Psram() noexcept {
  return impl_->psram;
}

bool Bsx::Selects(uint32_t address) noexcept {
  address &= 0xffffff;
  const unsigned bank = address >> 16, halfBank = bank & 0x7f, offset = address & 0xffff;
  if (bank == 0x7e || bank == 0x7f)
    return false;
  if (halfBank >= 0x40)
    return true;
  if (offset >= 0x8000 || (offset >= 0x2188 && offset <= 0x219f))
    return true;
  if (bank < 0x18 && (offset & 0xf000) == 0x5000)
    return true;
  return halfBank >= 0x20 && offset >= 0x6000;
}

uint8_t Bsx::ReadCpu(uint32_t address, uint8_t openBus) {
  address &= 0xffffff;
  const auto target = impl_->Locate(address);
  switch (target.kind) {
  case Impl::Region::Mapper:
    return impl_->ReadMapper(unsigned(target.offset));
  case Impl::Region::Radio:
    return impl_->ReadRadio(unsigned(target.offset), openBus);
  case Impl::Region::Bios:
    return impl_->bios.empty() ? openBus : impl_->bios[Fold(target.offset, impl_->bios.size())];
  case Impl::Region::Psram:
    return impl_->psram[target.offset];
  case Impl::Region::Sram:
    return impl_->sram.empty() ? openBus : impl_->sram[Fold(target.offset, impl_->sram.size())];
  case Impl::Region::Pack:
    return impl_->ReadPack(target.offset, openBus);
  default:
    return openBus;
  }
}

void Bsx::WriteCpu(uint32_t address, uint8_t value) {
  address &= 0xffffff;
  const auto target = impl_->Locate(address);
  switch (target.kind) {
  case Impl::Region::Mapper:
    impl_->WriteMapper(unsigned(target.offset), value);
    break;
  case Impl::Region::Radio:
    impl_->WriteRadio(unsigned(target.offset), value);
    break;
  case Impl::Region::Psram:
    impl_->psram[target.offset] = value;
    break;
  case Impl::Region::Sram:
    if (!impl_->sram.empty())
      impl_->sram[Fold(target.offset, impl_->sram.size())] = value;
    break;
  case Impl::Region::Pack:
    impl_->WritePack(target.offset, value);
    break;
  default:
    break;
  }
}

bool Bsx::LoadStream(uint16_t channel, uint8_t sequence, std::span<const uint8_t> payload) {
  if (!channel || channel > 0x3fff || payload.size() > size_t(22) * 65535)
    return false;
  if (!payload.empty() && !impl_->recordedStreams) {
    impl_->recordedStreams = true;
    impl_->radio = {};
    impl_->receivers = {};
    impl_->clockIndex = 0;
  }
  const auto key = Impl::StreamKey{channel, sequence};
  if (payload.empty())
    impl_->assets.erase(key);
  else
    impl_->assets[key] = std::vector<uint8_t>(payload.begin(), payload.end());
  for (auto &receiver : impl_->receivers) {
    if (receiver.loaded && receiver.asset == key) {
      receiver.loaded = false;
      receiver.remaining = receiver.cursor = 0;
      receiver.nextSequence = sequence;
    }
  }
  return true;
}

void Bsx::ClearStreams() {
  impl_->assets.clear();
  impl_->recordedStreams = false;
  impl_->receivers = {};
  impl_->radio = {};
  impl_->clockIndex = 0;
}

void Bsx::SetTimeSource(std::function<BroadcastTime()> source) {
  impl_->timeSource = std::move(source);
}

} // namespace snes::core
