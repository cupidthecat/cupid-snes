#include "snes/core/Cartridge.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "snes/core/detail/StateArchive.hpp"
#include "snes/core/detail/StateDigest.hpp"
#include "snes/core/gameboy/Adapter.hpp"

#include <tuple>

namespace snes::core {
void Cartridge::SynchronizeState() {
  if (sa1_)
    sa1_->SynchronizeState();
}

void Cartridge::SerializeState(detail::StateArchive &archive) {
  archive.Expect(uint64_t(rom_.size()));
  if (!stateRomIdentity_)
    stateRomIdentity_ = std::make_unique<std::array<uint8_t, 32>>(detail::StateDigest(
        rom_, archive.Progress(), archive.Reading() ? StatePhase::Validate : StatePhase::Capture));
  for (auto byte : *stateRomIdentity_)
    archive.Expect(byte);
  archive.Expect(static_cast<uint32_t>(header_.mapping));
  archive.Expect(static_cast<uint32_t>(header_.chip));
  archive.Expect(static_cast<uint32_t>(header_.speed));
  archive.Expect(header_.romSizeShift);
  archive.Expect(boardConnected_);
  archive.Expect(uint64_t(sram_.size()));
  for (unsigned slot = 0; slot < 2; ++slot) {
    archive.Expect(uint64_t(slotSize_[slot]));
    archive.Expect(uint64_t(slotOffset_[slot]));
    archive.Expect(uint64_t(slotSramSize_[slot]));
    archive.Expect(uint64_t(slotSramOffset_[slot]));
  }
  archive.Expect(uint64_t(memoryPack_.size()));
  archive.Expect(memoryPackReadOnly_);
  // Flash writes and battery loads do not change the identity of the inserted pack.
  static const auto noPack = detail::Sha256(std::span<const uint8_t>{});
  for (auto byte : memoryPackIdentity_ ? *memoryPackIdentity_ : noPack)
    archive.Expect(byte);
  archive.Expect(bool(signalBoard_));
  if (signalBoard_)
    archive.Expect(static_cast<uint32_t>(signalBoard_->Kind()));
  auto children =
      std::tie(memoryPackDevice_, sdd1_, spc7110_, sa1_, superFx_, cx4_, cx4Processor_, st011_,
               st018_, armDsp_, bsx_, dsp3_, dsp4_, obc1_, necDsp_, competition_, gameBoy_);
  std::apply([&](auto &...child) { (archive.Expect(bool(child)), ...); }, children);
  archive.Number(memselFast_);
  // SA-1 stages its borrowed BWRAM; every other board leaves shared RAM to this owner.
  if (!sa1_)
    archive.Fixed(std::span<uint8_t>(sram_));
  if (!memoryPackReadOnly_)
    archive.Fixed(std::span<uint8_t>(memoryPack_));
  const auto state = [&](auto &child) {
    if (child)
      child->SerializeState(archive);
  };
  std::apply([&](auto &...child) { (state(child), ...); }, children);
}
} // namespace snes::core
