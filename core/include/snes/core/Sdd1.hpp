#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace snes::core {
namespace detail {
class StateArchive;
}

class MemoryBus;
class Sdd1Decoder;

class Sdd1 {
  public:
    Sdd1(std::span<const uint8_t> rom, std::span<uint8_t> ram);
    ~Sdd1();

    void Reset();
    // The cartridge owns the ROM identity and SRAM contents.
    void SerializeState(detail::StateArchive& archive);
    uint8_t Read(uint32_t address, uint8_t openBus);
    std::optional<uint8_t> Peek(uint32_t address, uint8_t openBus) const;
    void Write(uint32_t address, uint8_t value);
    void ObserveDmaWrite(uint32_t address, uint8_t value);
    static void MapCpu(MemoryBus& bus, uint8_t slot, bool hasRam);

  private:
    uint8_t ReadRom(uint32_t offset) const;
    uint8_t ReadMmc(uint32_t address) const;

    std::span<const uint8_t> rom_;
    std::span<uint8_t> ram_;
    uint8_t hardEnable_ = 0;
    uint8_t softEnable_ = 0;
    std::array<uint8_t, 4> banks_{0, 1, 2, 3};

    struct Channel {
        uint32_t address = 0;
        uint16_t remaining = 0;
    };

    std::array<Channel, 8> channels_{};
    bool streaming_ = false;
    std::unique_ptr<Sdd1Decoder> decoder_;
};

// The reader supplies mapped cartridge bytes without CPU bus side effects.
std::vector<uint8_t> DecompressSdd1(const std::function<uint8_t(uint32_t)>& reader,
                                    size_t outputSize);

} // namespace snes::core
