// SA-1 cartridge processor, memory controller, and transfer engines.
#pragma once

#include "snes/core/Processor65816.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace snes::core {

class BsMemory;

namespace detail {
class ExecutionContext;
}

class Sa1 final : private Processor65816 {
    friend class MemoryInspector;

  public:
    // The cartridge owns both spans. They must remain valid until Reset(rom, ram).
    Sa1(std::span<const uint8_t> rom, std::span<uint8_t> bwram);
    ~Sa1();
    void Reset();
    void Reset(std::span<const uint8_t> rom, std::span<uint8_t> bwram);

    uint8_t ReadCpu(uint32_t address, uint8_t openBus = 0);
    void WriteCpu(uint32_t address, uint8_t value);
    // The console address latch determines contention with cartridge memory.
    // After the first Advance, advancement, Reset and destruction use that thread.
    // Deferred calls without contention retain work until shared access or a handoff.
    void Advance(uint32_t masterClocks, uint32_t cpuAddress = 0, bool cpuRefreshing = false,
                 bool synchronize = true);
    bool CpuIrqPending() const noexcept;
    void SetPal(bool enabled) noexcept;

    void SetBusContention(bool enabled) noexcept {
        busContention_ = enabled;
    }

    // Populated memory slots supply ROM pages 4-7. An empty slot retains
    // program-ROM mirroring. Reset() preserves the attached storage.
    void SetExpansionRom(std::span<const uint8_t> bytes,
                         const BsMemory* device = nullptr) noexcept {
        expansion_ = bytes;
        expansionDevice_ = device;
    }

    // Untimed accesses to the coprocessor's bus, useful to a debugger. Register
    // reads still have their normal side effects; these are not peek methods.
    uint8_t ReadSa1(uint32_t address, uint8_t openBus = 0);
    void WriteSa1(uint32_t address, uint8_t value);

    const Processor65816::Registers& CpuRegisters() const noexcept {
        return regs();
    }

    uint64_t MasterClocks() const noexcept {
        return requestedClocks_;
    }

    bool DmaActive() const noexcept {
        return dmaActive_;
    }

    // Finish retained work at a portable operation boundary, preserving clock lead.
    void SynchronizeState();
    void SerializeState(detail::StateArchive& archive);

  private:
    void RunPending();
    enum class Side { Cpu, Sa1 };
    static constexpr uint64_t AccumulatorMask = (uint64_t{1} << 40) - 1;

    std::span<const uint8_t> rom_;
    std::span<const uint8_t> expansion_;
    const BsMemory* expansionDevice_ = nullptr;
    std::span<uint8_t> bwram_;
    std::array<uint8_t, 2048> iram_{};
    std::array<uint8_t, 256> registers_{};
    uint8_t cpuFlags_ = 0;
    uint8_t sa1Flags_ = 0;
    bool cpuIrqLevel_ = false;
    bool interruptPending_ = false;

    uint64_t requestedClocks_ = 0;
    uint64_t executedClocks_ = 0;
    uint32_t cpuAddress_ = 0;
    bool cpuRefreshing_ = false;
    bool busContention_ = true;
    uint16_t hClocks_ = 0;
    uint16_t vCounter_ = 0;
    uint16_t latchedH_ = 0;
    uint16_t latchedV_ = 0;
    uint16_t videoLines_ = 262;

    uint64_t arithmeticResult_ = 0;
    bool arithmeticOverflow_ = false;
    uint8_t bitOffset_ = 0;

    bool dmaActive_ = false;
    bool conversionActive_ = false;
    uint8_t conversionRows_ = 0;
    uint8_t conversionBuffer_ = 0;
    bool synchronizingState_ = false, atStateBoundary_ = true;

    // Destroy the suspended stack before the memory and register state it uses.
    std::unique_ptr<detail::ExecutionContext> execution_;

    void idle() override;
    void idleBranch() override;
    void idleJump() override;
    uint8_t read(uint32_t address) override;
    void write(uint32_t address, uint8_t value) override;
    void lastCycle() override;
    bool interruptPending() const override;
    void instructionStop() override;
    void instructionWait() override;
    void Clock(uint32_t masterClocks);
    void StepProcessor();
    void ResetProcessor();
    void InterruptProcessor();
    bool Sa1IrqPending() const noexcept;
    bool Sa1NmiPending() const noexcept;
    void RaiseSa1Interrupt(uint8_t source) noexcept;
    void RaiseCpuInterrupt(uint8_t source) noexcept;
    void ClockAccess(uint32_t address);
    bool CpuRomConflict() const noexcept;
    bool CpuBwramConflict() const noexcept;
    bool CpuIramConflict() const noexcept;
    uint8_t ReadBitByte(uint32_t address) const;

    uint8_t ReadMemory(uint32_t address, uint8_t openBus, Side side, bool io = true);
    void WriteMemory(uint32_t address, uint8_t value, Side side);
    uint8_t ReadRegister(uint16_t address, uint8_t openBus, Side side);
    void WriteRegister(uint16_t address, uint8_t value, Side side);
    static bool WritableRegister(uint16_t address, Side side) noexcept;
    uint16_t RegisterWord(uint8_t offset) const noexcept;
    uint32_t RegisterAddress(uint8_t offset) const noexcept;
    void StoreAddress(uint8_t offset, uint32_t address) noexcept;
    uint8_t ReadRom(uint32_t offset, uint8_t openBus) const;
    uint8_t ReadRomPort(uint32_t address, uint8_t openBus) const;
    static size_t Mirror(size_t address, size_t size) noexcept;
    uint8_t ReadBwram(uint32_t offset, uint8_t openBus = 0) const;
    void WriteBwram(uint32_t offset, uint8_t value, Side side, bool dma = false);
    void WriteIram(uint32_t offset, uint8_t value, Side side);
    bool BwramWritable(uint32_t offset, Side side) const noexcept;
    uint8_t BitmapDepth() const noexcept;
    uint8_t ReadBitmap(uint32_t pixel, uint8_t openBus) const;
    void WriteBitmap(uint32_t pixel, uint8_t value);

    void AdvanceTimer(uint32_t masterClocks);
    void StartArithmetic();
    uint16_t ReadBitResult() const;
    void AdvanceBits();
    void StartDma();
    void StepDma();
    uint8_t ConversionDepth() const noexcept;
    void StartConversion();
    void ConvertBitmapTile(uint32_t tile);
    uint8_t ReadConverted(uint32_t offset);
    void PushConversionRows();
    void WritePlanarTile(const std::array<uint8_t, 64>& pixels, uint16_t destination);
};

} // namespace snes::core
