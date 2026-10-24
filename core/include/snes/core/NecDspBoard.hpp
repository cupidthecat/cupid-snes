#pragma once

#include <cstdint>
#include <optional>

namespace snes::core {
class MemoryBus;

// Physical cartridge wiring shared by processor execution and command models.
class NecDspBoard {
  public:
    enum class Layout { LoRom, LoRomSmallRam, LoRomLargeRam, HiRom, Seta };

    explicit NecDspBoard(Layout layout) : layout_(layout) {
    }

    Layout Kind() const noexcept {
        return layout_;
    }

    void MapCpu(MemoryBus& bus, uint8_t slot) const;
    void MapDevice(MemoryBus& bus, uint8_t slot) const;
    bool SelectsDevice(uint32_t address) const noexcept;
    bool StatusPort(uint32_t address) const noexcept;
    std::optional<uint32_t> RomAddress(uint32_t address) const noexcept;
    std::optional<uint32_t> SramAddress(uint32_t address) const noexcept;

  private:
    bool SelectsRom(uint32_t address) const noexcept;
    Layout layout_;
};
} // namespace snes::core
