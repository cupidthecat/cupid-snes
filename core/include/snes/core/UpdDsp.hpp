#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace snes::core {
namespace detail {
class StateArchive;
}

// The two cartridge DSP revisions share an instruction set and host data port.
class UpdDsp {
  public:
    enum class Revision { Upd7725, Upd96050 };

    enum Flag : uint8_t {
        Overflow = 1,
        StickyOverflow = 2,
        Zero = 4,
        Carry = 8,
        Sign = 16,
        ExtendedSign = 32
    };

    struct Registers {
        std::array<uint16_t, 16> stack{};
        std::array<uint16_t, 2> accumulator{};
        std::array<uint8_t, 2> flags{};
        uint16_t pc = 0, romPointer = 0, ramPointer = 0;
        uint16_t serialInput = 0, serialOutput = 0;
        uint16_t temporary = 0, temporaryBuffer = 0;
        uint16_t data = 0, status = 0;
        int16_t factorX = 0, factorY = 0;
        uint16_t productHigh = 0, productLow = 0;
        uint8_t stackPointer = 0;
        bool inputAcknowledge = false, outputAcknowledge = false;
    };

    explicit UpdDsp(Revision revision = Revision::Upd7725);
    bool LoadFirmware(std::span<const uint8_t> bytes);
    void Reset(); // Processor RAM remains powered.
    void SerializeState(detail::StateArchive& archive);
    void Step();
    uint8_t ReadStatus() const noexcept;
    uint8_t ReadData();
    void WriteData(uint8_t value);
    uint8_t ReadRam(uint16_t byteAddress) const noexcept;
    void WriteRam(uint16_t byteAddress, uint8_t value) noexcept;

    Registers& State() noexcept {
        return state_;
    }

    const Registers& State() const noexcept {
        return state_;
    }

    std::span<uint32_t> Program() noexcept {
        return program_;
    }

    std::span<uint16_t> DataRom() noexcept {
        return dataRom_;
    }

    std::span<uint16_t> Ram() noexcept {
        return ram_;
    }

    uint64_t Cycles() const noexcept {
        return cycles_;
    }

    size_t FirmwareSize() const noexcept;

  private:
    uint16_t EffectiveStatus() const noexcept;
    uint16_t ReadOperand(unsigned operand);
    void StoreOperand(unsigned operand, uint16_t value);
    void Arithmetic(unsigned operation, unsigned accumulator, uint16_t operand);
    void Parallel(uint32_t instruction);
    void Branch(uint32_t instruction);
    bool Condition(unsigned condition) const noexcept;

    uint16_t programMask_, romMask_, ramMask_;
    Registers state_{};
    std::array<uint32_t, 16384> program_{};
    std::array<uint16_t, 2048> dataRom_{};
    std::array<uint16_t, 2048> ram_{};
    uint64_t cycles_ = 0;
};

} // namespace snes::core
