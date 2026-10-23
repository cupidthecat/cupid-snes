#include "snes/core/Cartridge.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "snes/core/gameboy/Adapter.hpp"
#include <stdexcept>

namespace snes::core {
void Cartridge::PrepareCheats() {
  // Checkpoints identify the inserted image even while its in-memory bytes are patched.
  if (!stateRomIdentity_)
    stateRomIdentity_ = std::make_unique<std::array<uint8_t, 32>>(detail::Sha256(rom_));
}

void Cartridge::WriteCheat(uint32_t address, uint8_t value) {
  address &= 0xffffff;
  if (!boardConnected_)
    return;
  std::optional<size_t> offset;
  if (superFx_)
    offset = superFx_->CheatRomOffset(address);
  else if (bsx_)
    offset = bsx_->CheatBiosOffset(address);
  else if (signalBoard_) {
    if (const auto linear = signalBoard_->RomAddress(address); linear && !rom_.empty())
      offset = MirrorOffset(*linear, rom_.size());
  } else if (!sa1_ && !sdd1_ && !spc7110_ && !cx4Processor_ && !competition_ &&
             !ResolveMemoryPackOffset(address) && !ResolveSramOffset(address)) {
    offset = ResolveRomOffset(address);
  }
  if (offset && *offset < rom_.size()) {
    PrepareCheats();
    rom_[*offset] = value;
  } else {
    Write(address, value);
  }
}

void Cartridge::SetHandheldCheats(std::span<const CheatCode> codes) {
  if (!gameBoy_)
    throw std::invalid_argument("Handheld cheats require a handheld cartridge");
  gameBoy_->SetCheats(codes);
}
} // namespace snes::core
