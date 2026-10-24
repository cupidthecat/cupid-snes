#include "snes/core/Emulator.hpp"

#include <stdexcept>

namespace snes::core {
CheatSystem Emulator::CheatsSystem() const noexcept {
    return cartridge_ && cartridge_->Header().chip == EnhancementChip::SuperGameBoy
               ? CheatSystem::Handheld
               : CheatSystem::Console;
}

void Emulator::SetCheats(std::span<const CheatCode> codes) {
    if (!initialized_)
        throw std::runtime_error("Cheats require a loaded game");
    if (CheatsSystem() == CheatSystem::Handheld) {
        cartridge_->SetHandheldCheats(codes);
        return;
    }
    for (const auto& code : codes)
        if (code.address > 0xffffff)
            throw std::invalid_argument("Cheat address is outside the console bus");
    if (!codes.empty())
        cartridge_->PrepareCheats();
    cheats_.Set(
        codes, [this](uint32_t address) { return bus_.ReadCpu(address); },
        [this](uint32_t address, uint8_t value) { bus_.WriteCheat(address, value); });
}
} // namespace snes::core
