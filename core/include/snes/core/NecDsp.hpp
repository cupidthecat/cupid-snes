#pragma once

#include "snes/core/CartridgeFirmware.hpp"
#include "snes/core/NecDspBoard.hpp"
#include "snes/core/UpdDsp.hpp"
#include <optional>

namespace snes::core {
class MemoryBus;

class NecDsp {
    friend class MemoryInspector;

  public:
    using Board = NecDspBoard::Layout;
    NecDsp(FirmwareKind firmware, Board board);
    bool LoadFirmware(std::span<const uint8_t> bytes);
    void Reset();
    void SerializeState(detail::StateArchive& archive);
    void SetPal(bool pal);
    void Advance(uint32_t masterClocks);
    void MapCpu(MemoryBus& bus, uint8_t slot) const;
    bool SelectsDevice(uint32_t address) const noexcept;
    std::optional<uint32_t> RomAddress(uint32_t address) const noexcept;
    std::optional<uint32_t> SramAddress(uint32_t address) const noexcept;
    uint8_t Read(uint32_t address);
    void Write(uint32_t address, uint8_t value);
    std::span<const uint8_t> SaveRam() const noexcept;
    void LoadRam(std::span<const uint8_t> bytes);

    bool HasSaveRam() const noexcept {
        return board_.Kind() == Board::Seta;
    }

    uint64_t Cycles() const noexcept {
        return processor_.Cycles();
    }

  private:
    bool SelectsDataRam(uint32_t address) const noexcept;
    UpdDsp processor_;
    NecDspBoard board_;
    uint32_t frequency_, masterFrequency_;
    int64_t clockBalance_ = 0;
    mutable std::array<uint8_t, 4096> savedRam_{};
};
} // namespace snes::core
