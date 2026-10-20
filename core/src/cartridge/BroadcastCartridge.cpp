#include "snes/core/Cartridge.hpp"

#include <algorithm>

namespace snes::core {
namespace {
bool MaskRomPack(std::span<const uint8_t> bytes) {
  return bytes.size() > 0xff06 && bytes[0xff00] == 'M' && bytes[0xff02] == 'P' &&
         (bytes[0xff06] & 0xf0) == 0x70;
}

bool ValidMemoryPack(std::span<const uint8_t> bytes) {
  return bytes.empty() || bytes.size() == 0x100000 || bytes.size() == 0x200000 ||
         bytes.size() == 0x400000 || (bytes.size() == 0x80000 && MaskRomPack(bytes));
}
} // namespace

void Cartridge::InitializeBroadcast(std::span<const uint8_t> pack) {
  bsx_.reset();
  memoryPack_.assign(pack.begin(), pack.end());
  memoryPackReadOnly_ = MaskRomPack(memoryPack_);
  bsx_ = std::make_unique<Bsx>(rom_, memoryPack_, sram_);
  bsx_->SetTimeSource(CurrentBroadcastTime);
}

bool Cartridge::LoadBroadcastStream(uint16_t channel, uint8_t sequence,
                                    std::span<const uint8_t> data) {
  return bsx_ && bsx_->LoadStream(channel, sequence, data);
}

bool Cartridge::SetBroadcastTimeSource(std::function<BroadcastTime()> source) {
  if (!bsx_)
    return false;
  bsx_->SetTimeSource(std::move(source));
  return true;
}

std::optional<Cartridge> Cartridge::FromBroadcastCartridge(std::span<const uint8_t> base,
                                                           std::span<const uint8_t> pack,
                                                           std::string *error) {
  auto cart = FromRomImage(base, nullptr, nullptr, error);
  if (!cart)
    return std::nullopt;
  const auto &header = cart->header_;
  if (header.mapping == MappingType::Bsx) {
    if (!cart->boardConnected_)
      return cart;
    auto normalizedPack = RemoveCopierHeader(pack, nullptr);
    if (!ValidMemoryPack(normalizedPack)) {
      if (error)
        *error = "Memory pack must be 1, 2 or 4 MiB flash, or a 512 KiB mask ROM";
      return std::nullopt;
    }
    cart->InitializeBroadcast(normalizedPack);
    return cart;
  }
  if (header.mapping == MappingType::BroadcastSa1) {
    if (!cart->boardConnected_)
      return cart;
    auto expansion = RemoveCopierHeader(pack, nullptr);
    if (expansion.size() != 0x80000 && !ValidMemoryPack(expansion)) {
      if (error)
        *error = "Invalid SA-1 memory pack capacity";
      return std::nullopt;
    }
    cart->memoryPack_ = std::move(expansion);
    cart->memoryPackReadOnly_ =
        cart->memoryPack_.size() == 0x80000 || MaskRomPack(cart->memoryPack_);
    if (!cart->memoryPack_.empty())
      cart->memoryPackDevice_ =
          std::make_unique<BsMemory>(cart->memoryPack_, cart->memoryPackReadOnly_);
    cart->sa1_->SetExpansionRom(cart->memoryPack_, cart->memoryPackDevice_.get());
    return cart;
  }
  const bool lo = header.mapping == MappingType::BroadcastLoRom;
  if ((!lo && header.mapping != MappingType::BroadcastHiRom) ||
      header.chip != EnhancementChip::None) {
    if (error)
      *error = "Expected a BS slot base cartridge without an enhancement processor";
    return std::nullopt;
  }
  if (!cart->boardConnected_)
    return cart;
  auto normalizedPack = RemoveCopierHeader(pack, nullptr);
  if (!ValidMemoryPack(normalizedPack)) {
    if (error)
      *error = "Memory pack must be 1, 2 or 4 MiB flash, or a 512 KiB mask ROM";
    return std::nullopt;
  }
  cart->header_.mapping = lo ? MappingType::BroadcastLoRom : MappingType::BroadcastHiRom;
  cart->memoryPack_ = std::move(normalizedPack);
  const auto &bytes = cart->memoryPack_;
  if (!bytes.empty()) {
    cart->memoryPackReadOnly_ = MaskRomPack(bytes);
    cart->memoryPackDevice_ =
        std::make_unique<BsMemory>(cart->memoryPack_, cart->memoryPackReadOnly_);
  }
  return cart;
}

std::span<const uint8_t> Cartridge::MemoryPackData() const noexcept {
  return memoryPackReadOnly_ ? std::span<const uint8_t>{} : memoryPack_;
}

bool Cartridge::LoadMemoryPack(std::span<const uint8_t> data) {
  if (memoryPackReadOnly_ || memoryPack_.empty() || data.size() != memoryPack_.size())
    return false;
  std::copy(data.begin(), data.end(), memoryPack_.begin());
  if (memoryPackDevice_)
    memoryPackDevice_->Reset();
  if (bsx_)
    bsx_->Reset();
  return true;
}

std::optional<size_t> Cartridge::ResolveMemoryPackOffset(uint32_t address) const {
  const unsigned bank = (address >> 16) & 0xff;
  const unsigned offset = address & 0xffff;
  if (memoryPack_.empty())
    return std::nullopt;
  if (header_.mapping == MappingType::BroadcastLoRom) {
    if (bank < 0xc0 || bank > 0xef)
      return std::nullopt;
    return address & (memoryPack_.size() - 1);
  }
  if (header_.mapping == MappingType::BroadcastHiRom) {
    if (bank == 0x7e || bank == 0x7f || !(bank & 0x20) || (!(bank & 0x40) && offset < 0x8000))
      return std::nullopt;
    return address & (memoryPack_.size() - 1);
  }
  return std::nullopt;
}

std::vector<uint8_t> Cartridge::MemoryPackMetadata() const {
  if (bsx_)
    return bsx_->FlashMetadata();
  return memoryPackDevice_ ? memoryPackDevice_->Metadata() : std::vector<uint8_t>{};
}

bool Cartridge::LoadMemoryPackMetadata(std::span<const uint8_t> bytes) {
  if (bsx_)
    return bsx_->LoadFlashMetadata(bytes);
  return memoryPackDevice_ && memoryPackDevice_->LoadMetadata(bytes);
}

uint8_t Cartridge::ReadMemoryPack(uint32_t, size_t offset) const {
  return memoryPackDevice_ ? memoryPackDevice_->Read(uint32_t(offset)) : memoryPack_[offset];
}

void Cartridge::WriteMemoryPack(uint32_t, size_t offset, uint8_t value) {
  if (memoryPackDevice_)
    memoryPackDevice_->Write(uint32_t(offset), value);
}

} // namespace snes::core
