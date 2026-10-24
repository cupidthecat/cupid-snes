#include "snes/core/St018Processor.hpp"
#include "snes/core/PowerOnState.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/detail/ExecutionContext.hpp"
#include "snes/core/detail/Sha256.hpp"

#include <algorithm>
#include <stdexcept>

namespace snes::core {

St018Processor::St018Processor(std::span<const uint8_t> firmware) {
    if (firmware.size() != firmware_.size())
        throw std::invalid_argument("Invalid ST018 firmware size");
    std::copy(firmware.begin(), firmware.end(), firmware_.begin());
    firmwareDigest_ = detail::Sha256(firmware_);
    Reset();
}

St018Processor::~St018Processor() = default;

void St018Processor::Restart() {
    execution_.reset();
    phase_ = Phase::Begin;
    instruction_ = {};
    replaying_ = false;
    recording_ = true;
    replayIndex_ = 0;
    Arm6::Reset();
    ready_ = signal_ = inputReady_ = outputReady_ = false;
    timer_ = timerLatch_ = 0;
    balance_ = 0;
}

void St018Processor::Reset() {
    PowerOnState state(EntropyMode::None, 0);
    Power(state);
}

void St018Processor::Power(PowerOnState& state) {
    reset_ = false;
    Restart();
    state.Fill(ram_);
}

void St018Processor::SetPal(bool pal) {
    masterFrequency_ = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
}

void St018Processor::Advance(uint32_t masterClocks, bool synchronize) {
    balance_ -= int64_t(masterClocks) * 21'440'000;
    // Only the bridge is shared with the CPU. Batch independent execution between
    // bridge accesses and scanline handoffs, with a bounded clock debt.
    constexpr int64_t maximumDebt = int64_t(4096) * 21'440'000;
    if (synchronize || balance_ < -maximumDebt)
        Synchronize();
}

void St018Processor::Synchronize() {
    if (balance_ >= 0)
        return;
    if (!execution_) {
        execution_ = std::make_unique<detail::ExecutionContext>([this] { RunWorker(); });
    }
    execution_->Resume();
}

void St018Processor::Cycle(unsigned clocks) {
    if (timer_)
        --timer_;
    balance_ += int64_t(clocks) * masterFrequency_;
    if (balance_ >= 0)
        execution_->Suspend();
}

void St018Processor::IdleArm() {
    if (recording_)
        BeginAccess(AccessKind::Idle);
    else
        Cycle(1);
}

uint8_t St018Processor::Status() const {
    return uint8_t((outputReady_ ? 1 : 0) | (signal_ ? 4 : 0) | (inputReady_ ? 8 : 0) |
                   (ready_ ? 128 : 0));
}

uint8_t St018Processor::Read(uint32_t address) {
    Synchronize();
    switch (address & 6) {
    case 0:
        if (outputReady_) {
            outputReady_ = false;
            return output_;
        }
        break;
    case 2:
        signal_ = false;
        break;
    case 4:
        return Status();
    }
    return 0;
}

void St018Processor::Write(uint32_t address, uint8_t value) {
    Synchronize();
    switch (address & 6) {
    case 2:
        input_ = value;
        inputReady_ = true;
        break;
    case 4: {
        const bool reset = value & 1;
        if (!reset_ && reset)
            Restart();
        reset_ = reset;
        break;
    }
    }
}

uint32_t St018Processor::ReadArm(unsigned access, uint32_t address) {
    if (!recording_) {
        Cycle(1);
        return ReadMemory(access, address);
    }
    if (const auto* record = BeginAccess(AccessKind::Read, access, address))
        return record->result;
    const auto result = ReadMemory(access, address);
    instruction_.accesses[instruction_.count - 1].result = result;
    return result;
}

uint32_t St018Processor::ReadMemory(unsigned access, uint32_t address) {
    const unsigned width = access & Width;
    const auto memory = [&](std::span<const uint8_t> bytes, unsigned offset) -> uint32_t {
        if (width == Byte)
            return bytes[offset];
        if (width != Word)
            return 0;
        offset &= ~3u;
        return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
               (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
    };
    switch (address >> 29) {
    case 0:
        return memory(std::span(firmware_).first(0x20000), address & 0x1ffff);
    case 1:
    case 4:
    case 6:
        return PrefetchedWord();
    case 3:
        return 0x40404001;
    case 5:
        return memory(std::span(firmware_).subspan(0x20000), address & 0x7fff);
    case 7:
        return memory(ram_, address & 0x3fff);
    default:
        break;
    }
    switch (address & 63) {
    case 0x10:
        if (inputReady_) {
            inputReady_ = false;
            return input_;
        }
        break;
    case 0x20:
        return Status();
    }
    return 0;
}

void St018Processor::WriteArm(unsigned access, uint32_t address, uint32_t value) {
    if (!recording_) {
        Cycle(1);
        WriteMemory(access, address, value);
        return;
    }
    if (BeginAccess(AccessKind::Write, access, address, value))
        return;
    WriteMemory(access, address, value);
}

void St018Processor::WriteMemory(unsigned access, uint32_t address, uint32_t value) {
    if ((address >> 29) == 7) {
        const unsigned width = access & Width;
        if (width == Byte)
            ram_[address & 0x3fff] = uint8_t(value);
        else if (width == Word) {
            const unsigned offset = address & 0x3ffc;
            for (unsigned byte = 0; byte < 4; ++byte)
                ram_[offset + byte] = uint8_t(value >> (byte * 8));
        }
        return;
    }
    if ((address >> 29) != 2)
        return;
    value &= 255;
    switch (address & 63) {
    case 0x00:
        output_ = uint8_t(value);
        outputReady_ = true;
        break;
    case 0x10:
        signal_ = true;
        break;
    case 0x20:
        timerLatch_ = (timerLatch_ & 0xffff00) | value;
        break;
    case 0x24:
        timerLatch_ = (timerLatch_ & 0xff00ff) | (value << 8);
        break;
    case 0x28:
        timerLatch_ = (timerLatch_ & 0x00ffff) | (value << 16);
        break;
    case 0x2c:
        timer_ = timerLatch_;
        break;
    }
}

void St018Processor::RunWorker() {
    // A restored worker starts just after the clock yield retained in its phase.
    if (phase_ == Phase::ResetClock)
        phase_ = Phase::Begin;
    else if (phase_ == Phase::StartupClock) {
        ready_ = true;
        phase_ = Phase::Instruction;
    } else if (phase_ == Phase::Instruction) {
        RestoreExecutionState(instruction_.start);
        replayIndex_ = 0;
        replaying_ = true;
        recording_ = true;
        RunInstruction();
    }
    for (;;) {
        if (phase_ == Phase::Begin) {
            if (reset_) {
                phase_ = Phase::ResetClock;
                Cycle(1);
                phase_ = Phase::Begin;
                continue;
            }
            phase_ = Phase::StartupClock;
            Cycle(65536);
            ready_ = true;
            phase_ = Phase::Instruction;
        }
        instruction_.count = 0;
        replaying_ = false;
        // Every access costs one clock. Only instructions close enough to yield
        // need a starting pipeline and a journal for rebuilding suspended work.
        recording_ = balance_ >= -int64_t(MaximumAccesses) * masterFrequency_;
        if (recording_)
            instruction_.start = CaptureExecutionState();
        RunInstruction();
    }
}

const St018Processor::AccessRecord* St018Processor::BeginAccess(AccessKind kind, unsigned access,
                                                                uint32_t address, uint32_t data) {
    if (replaying_) {
        const auto& record = instruction_.accesses[replayIndex_++];
        if (record.kind != kind || record.access != access || record.address != address ||
            record.data != data)
            throw std::logic_error("Invalid ARM instruction continuation");
        if (replayIndex_ < instruction_.count)
            return &record;
        // The final access already paid its clock but has not touched the bus yet.
        replaying_ = false;
        return nullptr;
    }
    if (instruction_.count == MaximumAccesses)
        throw std::logic_error("ARM instruction exceeds its bus access limit");
    instruction_.accesses[instruction_.count++] = {kind, access, address, data, 0};
    Cycle(1);
    return nullptr;
}
} // namespace snes::core
