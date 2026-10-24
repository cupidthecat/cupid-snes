#include "snes/core/Arm6.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

using snes::core::Arm6;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

struct Machine : Arm6 {
    std::array<uint32_t, 256> memory{};
    unsigned reads = 0, writes = 0, idles = 0;

    Machine() {
        Reset();
    }

    uint32_t ReadArm(unsigned access, uint32_t address) override {
        ++reads;
        auto value = memory[(address & 1023) / 4];
        if ((access & Width) == Byte)
            value = (value >> ((address & 3) * 8)) & 255;
        if ((access & Width) == Half)
            value = (value >> ((address & 2) * 8)) & 65535;
        return value;
    }

    void WriteArm(unsigned, uint32_t address, uint32_t value) override {
        ++writes;
        memory[(address & 1023) / 4] = value;
    }

    void IdleArm() override {
        ++idles;
    }
};

void Arithmetic() {
    Machine cpu;
    cpu.memory[0] = 0xe3a00001;
    cpu.memory[1] = 0xe3e01102;
    cpu.memory[2] = 0xe0902001;
    cpu.memory[3] = 0xe3500001;
    cpu.memory[4] = 0xe2a13000;
    cpu.RunInstruction();
    cpu.RunInstruction();
    cpu.RunInstruction();
    Check(cpu.GetState().common[2] == 0x80000000 && cpu.GetState().status == 0x900000d3,
          "Signed overflow sets N and V without setting carry");
    cpu.RunInstruction();
    Check(cpu.GetState().status == 0x600000d3, "Equal comparison sets zero and carry");
    cpu.RunInstruction();
    Check(cpu.GetState().common[3] == 0x80000000,
          "ADC consumes carry left by a comparison without overwriting flags");
}

void BankedException() {
    Machine cpu;
    cpu.memory[0] = 0xe321f0d2;
    cpu.memory[1] = 0xe3a0d00b;
    cpu.memory[2] = 0xef000000;
    cpu.RunInstruction();
    cpu.RunInstruction();
    cpu.RunInstruction();
    const auto& state = cpu.GetState();
    Check(state.status == 0xd3 && state.saved[2] == 0xd2,
          "Software interrupt saves IRQ status in the supervisor bank");
    Check(state.bank[0][0] == 11 && state.bank[1][0] == 0 && state.bank[1][1] == 12,
          "Exception entry keeps the IRQ stack and records the following instruction address");
    Check(state.common[15] == 8, "Software interrupt reloads the vector pipeline");
}

void PrefetchRetention() {
    Machine cpu;
    cpu.memory[0] = 0xe3a00001;
    cpu.memory[1] = 0xe3a01007;
    cpu.memory[2] = 0xeafffffd;
    cpu.RunInstruction();
    Check(cpu.reads == 3 && cpu.GetState().common[15] == 8,
          "The first instruction fills the pipeline and exposes PC plus eight");
    cpu.memory[1] = 0xe3a01009;
    cpu.RunInstruction();
    Check(cpu.GetState().common[1] == 7, "Already prefetched instructions survive a memory write");
    cpu.RunInstruction();
    cpu.RunInstruction();
    Check(cpu.GetState().common[1] == 9, "A branch refills the pipeline from current memory");
}

void ConditionalAccess() {
    Machine cpu;
    cpu.memory[0] = 0x05801000;
    cpu.memory[1] = 0x15801000;
    auto state = cpu.GetState();
    state.common[0] = 128;
    state.common[1] = 0x12345678;
    cpu.SetState(state);
    cpu.RunInstruction();
    Check(cpu.writes == 0, "A failed condition performs instruction fetches without a data write");
    cpu.RunInstruction();
    Check(cpu.writes == 1 && cpu.memory[32] == 0x12345678,
          "A passing condition performs the store through the memory interface");
}
} // namespace

int main() {
    try {
        Arithmetic();
        BankedException();
        PrefetchRetention();
        ConditionalAccess();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("ARM processor tests passed");
}
