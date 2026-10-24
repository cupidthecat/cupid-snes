// Super FX graphics processor and cartridge bus interface.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace snes::core {
namespace detail {
class StateArchive;
}

class SuperFx {
  public:
    static constexpr uint64_t ClockFrequency = 21'440'000;
    // Storage belongs to the cartridge and must remain valid until reset/destruction.
    // RAM is shared with the cartridge's save storage; resetting never erases it.
    SuperFx(std::span<const uint8_t> rom, std::span<uint8_t> ram);
    ~SuperFx();
    SuperFx(SuperFx&&) noexcept;
    SuperFx& operator=(SuperFx&&) noexcept;
    SuperFx(const SuperFx&) = delete;
    SuperFx& operator=(const SuperFx&) = delete;

    void Reset();
    void Reset(std::span<const uint8_t> rom, std::span<uint8_t> ram);

    // Addresses are full 24-bit S-CPU addresses. Synchronize before CPU accesses.
    // Union of supported cartridge windows; disconnected windows read open bus.
    static bool Selects(uint32_t address) noexcept;
    static bool SelectsRegisters(uint32_t address) noexcept;
    uint8_t ReadCpu(uint32_t address, uint8_t openBus = 0xff);
    void WriteCpu(uint32_t address, uint8_t value);
    std::optional<size_t> CheatRomOffset(uint32_t address) const;
    std::span<const uint8_t> InspectCache() const noexcept;
    // Elapsed cartridge oscillator clocks, not console clocks or an absolute timestamp.
    void Advance(uint64_t clocks);
    // Console master-clock timing. The selected region is retained across resets.
    void SetPal(bool pal);
    void AdvanceMasterClocks(uint32_t clocks);
    // STOP and acknowledge reads drive this output independently of writable SFR bits.
    bool CpuIrqPending() const noexcept;
    void SerializeState(detail::StateArchive& archive);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snes::core
