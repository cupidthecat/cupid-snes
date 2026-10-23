#include "snes/core/Emulator.hpp"
#include "snes/core/debug/MemoryInspection.hpp"
#include "snes/core/gameboy/Adapter.hpp"
#include <stdexcept>

namespace snes::core {
std::optional<uint8_t> MemoryInspector::PeekCpu(const Emulator &machine, uint32_t address) {
  if (address > 0xffffff)
    throw std::out_of_range("CPU addresses must be between 000000 and FFFFFF");
  if (!machine.initialized_)
    return std::nullopt;
  const auto &bus = machine.bus_;
  const auto slot = bus.lookup_[address];
  if (slot == 0)
    return std::nullopt;
  if (slot == bus.wramSlotLo_)
    return bus.wram_[address & 0x1fff];
  if (slot == bus.wramSlotFull_)
    return bus.wram_[address & 0x1ffff];
  if (slot == bus.cartridgeSlot_ && machine.cartridge_)
    return PeekCartridge(*machine.cartridge_, address, bus.mdr_);
  return std::nullopt;
}
std::optional<uint8_t> MemoryInspector::PeekCartridge(const Cartridge &cart, uint32_t address,
                                                      uint8_t openBus) {
  if (!cart.boardConnected_)
    return std::nullopt;
  if (cart.gameBoy_ && gameboy::Adapter::Selects(address))
    return std::nullopt;
  if (cart.competition_) {
    const auto &board = *cart.competition_;
    if (board.SelectsDsp(address))
      return std::nullopt;
    const auto status =
        board.settings_.board == CompetitionBoard::Board::CampusChallenge92 ? 0xc00000u : 0x106000u;
    if (address == status)
      return board.status_;
    if (const auto offset = board.SaveAddress(address))
      return board.save_[*offset];
    if (const auto rom = board.RomAddress(address))
      return board.images_[rom->first][rom->second];
    return openBus;
  }
  if (cart.cx4Processor_) {
    const auto &chip = *cart.cx4Processor_;
    if ((address & 0x408000) == 0x008000) {
      if (chip.Busy())
        return std::nullopt;
      const auto offset = ((address & 0xff0000) >> 1) | (address & 0x7fff);
      return chip.rom_.empty() ? openBus : chip.rom_[chip.Mirror(offset, chip.rom_.size())];
    }
    if ((address & 0xf88000) == 0x700000)
      return chip.save_.empty() ? 0 : chip.save_[chip.Mirror(address, chip.save_.size())];
    if ((address & 0x40e000) == 0x006000 && (address & 0xc00) != 0xc00)
      return chip.GetState().dataRam[address & 0xfff];
    return std::nullopt;
  }
  if (cart.sdd1_)
    return cart.sdd1_->Peek(address, openBus);
  if (cart.signalBoard_) {
    const auto &board = *cart.signalBoard_;
    if (board.SelectsDevice(address))
      return std::nullopt;
    if (const auto offset = board.SramAddress(address); offset && !cart.sram_.empty())
      return cart.sram_[Cartridge::MirrorOffset(*offset, cart.sram_.size())];
    if (const auto offset = board.RomAddress(address); offset && !cart.rom_.empty())
      return cart.rom_[Cartridge::MirrorOffset(*offset, cart.rom_.size())];
    return openBus;
  }
  if (cart.obc1_) {
    if (Obc1::Selects(address))
      return std::nullopt;
    if ((address & 0x408000) != 0x008000)
      return openBus;
  }
  if (cart.bsx_)
    return cart.bsx_->PeekCpu(address, openBus);
  if ((cart.st011_ && St010::Selects(address)) ||
      (cart.armDsp_ && St018Processor::Selects(address)) ||
      (cart.st018_ && (address & 0x40f000) == 0x003000) || (cart.cx4_ && Cx4::Selects(address)))
    return std::nullopt;
  const auto bank = uint8_t(address >> 16);
  const auto low = uint16_t(address);
  if (cart.sa1_) {
    const auto &chip = *cart.sa1_;
    if (!(bank & 0x40)) {
      if (low >= 0x2200 && low <= 0x23ff)
        return std::nullopt;
      if (low >= 0x3000 && low <= 0x37ff)
        return chip.iram_[low & 0x7ff];
      if (low >= 0x6000 && low <= 0x7fff)
        return chip.conversionActive_
                   ? std::nullopt
                   : std::optional(chip.ReadBwram(
                         uint32_t(chip.registers_[0x24] & 31) * 0x2000 + (low & 0x1fff), openBus));
    }
    if (bank >= 0x40 && bank <= 0x4f)
      return chip.conversionActive_ ? std::nullopt
                                    : std::optional(chip.ReadBwram(address & 0x3ffff, openBus));
    if ((!(bank & 0x40) && low >= 0x8000) || bank >= 0xc0)
      return chip.ReadRomPort(address, openBus);
    return openBus;
  }
  if (cart.superFx_) {
    if (SuperFx::SelectsRegisters(address))
      return std::nullopt;
    // The non-register path reads only currently driven ROM/RAM bytes.
    return cart.superFx_->ReadCpu(address, openBus);
  }
  if (cart.spc7110_) {
    const auto &chip = *cart.spc7110_;
    if (!(bank & 0x40) && low >= 0x6000 && low <= 0x7fff) {
      const auto offset = ((address & 0xff0000) >> 3) | (low & 0x1fff);
      return !(chip.registers_[0x30] & 0x80) || chip.save_.empty()
                 ? 0
                 : chip.save_[chip.Mirror(offset, chip.save_.size())];
    }
    if ((!(bank & 0x40) && low >= 0x8000) || bank >= 0xc0)
      return chip.Rom(address, openBus);
    if (chip.rom_.size() == 0x700000 && bank >= 0x40 && bank <= 0x4f)
      return chip.rom_[0x600000 + (address & 0xfffff)];
    return std::nullopt;
  }
  if (cart.header_.mapping == MappingType::St010 && St010::Selects(address))
    return std::nullopt;
  if (const auto offset = cart.ResolveMemoryPackOffset(address))
    return cart.ReadMemoryPack(address, *offset);
  if (const auto offset = cart.ResolveSramOffset(address); offset && !cart.sram_.empty())
    return cart.sram_[*offset];
  if (const auto offset = cart.ResolveRomOffset(address); offset && !cart.rom_.empty())
    return cart.rom_[*offset];
  return openBus;
}
} // namespace snes::core
