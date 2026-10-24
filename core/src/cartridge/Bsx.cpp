#include "snes/core/Bsx.hpp"
#include "snes/core/BsMemory.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
  std::span<const uint8_t> bios;
  std::span<uint8_t> pack, sram;
  BsMemory flash;
  std::vector<uint8_t> psram = std::vector<uint8_t>(0x80000, 0xff);
  std::array<uint8_t, 16> pending{}, active{};
  BroadcastReceiver receiver;

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
    receiver.Reset();
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
};

Bsx::Bsx(std::span<const uint8_t> bios, std::span<uint8_t> pack, std::span<uint8_t> sram)
    : impl_(std::make_unique<Impl>(bios, pack, sram)) {}
Bsx::~Bsx() = default;
Bsx::Bsx(Bsx &&) noexcept = default;
Bsx &Bsx::operator=(Bsx &&) noexcept = default;
void Bsx::Reset() {
  impl_->Reset();
}
void Bsx::SerializeState(detail::StateArchive &archive) {
  archive.Expect(uint64_t(impl_->bios.size()));
  archive.Expect(uint64_t(impl_->pack.size()));
  archive.Expect(uint64_t(impl_->sram.size()));
  archive.Fixed(std::span<uint8_t>(impl_->psram));
  for (auto *registers : {&impl_->pending, &impl_->active})
    for (auto &value : *registers) {
      const auto next = archive.Number(value);
      archive.Validate((next & 0x7f) == 0, "Invalid broadcast mapper bit");
    }
  impl_->flash.SerializeState(archive);
  impl_->receiver.SerializeState(archive);
}
void Bsx::InitializePageBuffers(PowerOnState &state) {
  impl_->flash.InitializePageBuffers(state);
}
void Bsx::SetPal(bool pal) {
  impl_->flash.SetPal(pal);
}
void Bsx::Advance(uint32_t masterClocks, bool synchronizeProcessor) {
  impl_->flash.Advance(masterClocks, synchronizeProcessor);
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
    return impl_->receiver.ReadCpu(uint32_t(0x2188 + target.offset), openBus);
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
    impl_->receiver.WriteCpu(uint32_t(0x2188 + target.offset), value);
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
  return impl_->receiver.LoadStream(channel, sequence, payload);
}
void Bsx::ClearStreams() {
  impl_->receiver.ClearStreams();
}
void Bsx::SetTimeSource(std::function<BroadcastTime()> source) {
  impl_->receiver.SetTimeSource(std::move(source));
}
} // namespace snes::core
