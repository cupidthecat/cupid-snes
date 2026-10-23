#pragma once

#include "snes/core/Arm6.hpp"

#include <array>
#include <memory>
#include <span>

namespace snes::core {
class PowerOnState;

namespace detail {
class ExecutionContext;
}

class St018Processor : private Arm6 {
  public:
    explicit St018Processor(std::span<const uint8_t> firmware);
    ~St018Processor();
    void Reset();
    void SerializeState(detail::StateArchive& archive);
    void Power(PowerOnState& state);
    void SetPal(bool pal);
    void Advance(uint32_t masterClocks, bool synchronize = false);
    uint8_t Read(uint32_t address);
    void Write(uint32_t address, uint8_t value);

    std::span<const uint8_t> RamData() const noexcept {
        return ram_;
    }

    static bool Selects(uint32_t address) noexcept {
        return (address & 0x40ff00) == 0x003800;
    }

  private:
    enum class Phase : uint8_t { Begin, ResetClock, StartupClock, Instruction };
    enum class AccessKind : uint8_t { Idle, Read, Write };

    struct AccessRecord {
        AccessKind kind = AccessKind::Idle;
        uint32_t access = 0, address = 0, data = 0, result = 0;
        bool operator==(const AccessRecord&) const = default;
    };

    // A full block transfer uses at most 16 data accesses plus refill and idle clocks.
    static constexpr unsigned MaximumAccesses = 32;

    struct Instruction {
        ExecutionState start;
        std::array<AccessRecord, MaximumAccesses> accesses{};
        uint32_t count = 0;
    };

    void RunWorker();
    const AccessRecord* BeginAccess(AccessKind kind, unsigned access = 0, uint32_t address = 0,
                                    uint32_t data = 0);
    uint32_t ReadMemory(unsigned access, uint32_t address);
    void WriteMemory(unsigned access, uint32_t address, uint32_t value);
    void ValidateInstruction(detail::StateArchive& archive, const Instruction& instruction,
                             const ExecutionState& current) const;
    uint8_t Status() const;
    void Synchronize();
    void Restart();
    void Cycle(unsigned clocks);
    uint32_t ReadArm(unsigned access, uint32_t address) override;
    void WriteArm(unsigned access, uint32_t address, uint32_t value) override;
    void IdleArm() override;
    std::array<uint8_t, 0x28000> firmware_{};
    std::array<uint8_t, 0x4000> ram_{};
    std::unique_ptr<detail::ExecutionContext> execution_;
    Phase phase_ = Phase::Begin;
    Instruction instruction_{};
    bool replaying_ = false;
    bool recording_ = true;
    uint32_t replayIndex_ = 0;
    std::array<uint8_t, 32> firmwareDigest_{};
    uint32_t masterFrequency_ = 21'477'272;
    int64_t balance_ = 0;
    uint32_t timer_ = 0, timerLatch_ = 0;
    uint8_t input_ = 0, output_ = 0;
    bool inputReady_ = false, outputReady_ = false;
    bool ready_ = false, signal_ = false, reset_ = false;
};

} // namespace snes::core
