#include "audio/SpcBusExpected.hpp"
#include "audio/SpcBusFixture.hpp"
#include "snes/core/Smp.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace snes::core;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Event {
    unsigned kind;
    uint16_t address;
    uint8_t data;
    bool operator==(const Event&) const = default;
};

struct Bus : Spc700 {
    std::array<uint8_t, 65536> memory{};
    std::vector<Event> events;
    uint64_t digest = spc_bus_test::InitialDigest;

    void Record(unsigned kind, uint16_t address, uint8_t data) {
        events.push_back({kind, address, data});
        spc_bus_test::Record(digest, kind, address, data, r);
        ++cycles_;
    }

    void Idle() override {
        Record(0, 0, 0);
    }

    uint8_t Read(uint16_t address) override {
        const uint8_t data = memory[address];
        Record(1, address, data);
        return data;
    }

    void Write(uint16_t address, uint8_t data) override {
        Record(2, address, data);
        memory[address] = data;
    }
};

void ImpliedRead() {
    for (uint8_t opcode : {0x00, 0x5d, 0x7d, 0x9d, 0xbd}) {
        Bus cpu;
        cpu.Power();
        cpu.r.pc = 0x200;
        cpu.memory[0x200] = opcode;
        cpu.memory[0x201] = 0x71;
        cpu.Step();
        Check(cpu.events == std::vector<Event>{{1, 0x200, opcode}, {1, 0x201, 0x71}},
              "Two-cycle implied instructions read the next PC address after fetching the opcode");
    }
}

void WaitClasses() {
    constexpr std::array<unsigned, 4> duration{1, 2, 5, 10};

    struct Timing {
        uint8_t opcode;
        unsigned externalCycles, idleCycles;
    };

    constexpr std::array cases{
        Timing{0x00, 2, 0}, Timing{0x5d, 2, 0}, Timing{0x20, 2, 0}, Timing{0x40, 2, 0},
        Timing{0x60, 2, 0}, Timing{0x80, 2, 0}, Timing{0xe0, 2, 0}, Timing{0xa0, 2, 1},
        Timing{0xc0, 2, 1}, Timing{0xed, 2, 1}, Timing{0xcf, 2, 7}, Timing{0x9e, 2, 10},
        Timing{0x9f, 2, 3}, Timing{0xbf, 3, 1}, Timing{0x79, 4, 1}, Timing{0x69, 5, 1},
        Timing{0x78, 4, 1}, Timing{0x0d, 3, 1}, Timing{0x2d, 3, 1}, Timing{0xae, 3, 1},
        Timing{0x8e, 3, 1}, Timing{0x6f, 4, 1}, Timing{0x7f, 5, 1}, Timing{0x0f, 7, 1},
        Timing{0x01, 6, 2}, Timing{0x3f, 5, 3}, Timing{0x4f, 4, 2}, Timing{0x06, 3, 0},
        Timing{0xe6, 3, 0}, Timing{0xc6, 4, 0}, Timing{0xaf, 3, 1},
    };
    for (unsigned internal = 0; internal < 4; ++internal) {
        for (unsigned external = 0; external < 4; ++external) {
            for (const auto& test : cases) {
                Smp smp;
                smp.IoState().iplRomEnable = false;
                smp.IoState().internalWaitStates = uint8_t(internal);
                smp.IoState().externalWaitStates = uint8_t(external);
                smp.r.pc = 0x200;
                smp.r.x = 7;
                smp.r.y = 0x39;
                smp.Ram()[0x200] = test.opcode;
                smp.Ram()[0x201] = 0x40;
                smp.Ram()[0x202] = 0x20;
                smp.Step();
                Check(smp.CycleCount() == test.externalCycles * duration[external] +
                                              test.idleCycles * duration[internal],
                      "Instruction reads and true idle cycles retain their independently selected "
                      "wait "
                      "classes");
            }
        }
    }
}

void StackAndRegisterBoundaries() {
    Bus push;
    push.Power();
    push.r.pc = 0x200;
    push.r.a = 0x5a;
    push.memory[0x200] = 0x2d;
    push.memory[0x201] = 0x71;
    push.Step();
    Check(push.events ==
              std::vector<Event>{{1, 0x200, 0x2d}, {1, 0x201, 0x71}, {2, 0x1ef, 0x5a}, {0, 0, 0}},
          "PUSH stores the stack byte before its final idle cycle");

    Bus interrupt;
    interrupt.Power();
    interrupt.r.pc = 0x200;
    interrupt.r.p.i = true;
    interrupt.memory[0x200] = 0x0f;
    interrupt.memory[0x201] = 0x71;
    interrupt.memory[0xffde] = 0x34;
    interrupt.memory[0xffdf] = 0x92;
    interrupt.Step();
    Check(interrupt.events == std::vector<Event>{{1, 0x200, 0x0f},
                                                 {1, 0x201, 0x71},
                                                 {2, 0x1ef, 2},
                                                 {2, 0x1ee, 1},
                                                 {2, 0x1ed, 6},
                                                 {0, 0, 0},
                                                 {1, 0xffde, 0x34},
                                                 {1, 0xffdf, 0x92}} &&
              interrupt.r.pc == 0x9234 && !interrupt.r.p.i && interrupt.r.p.b,
          "BRK pushes the old status and return PC before reading its vector");

    Bus increment;
    increment.Power();
    increment.r.pc = 0x200;
    increment.r.x = 7;
    increment.memory[0x200] = 0xbf;
    increment.memory[7] = 0x80;
    for (unsigned cycle = 0; cycle < 3; ++cycle)
        increment.StepCycle();
    Check(increment.r.a == 0x80 && increment.r.x == 8 && increment.r.p.z && !increment.r.p.n &&
              increment.InstructionInProgress(),
          "MOV A,(X++) loads and increments before its last idle cycle");
    increment.StepCycle();
    Check(!increment.r.p.z && increment.r.p.n && !increment.InstructionInProgress(),
          "MOV A,(X++) publishes result flags after its last idle cycle");
}

void TimerReadSideEffects() {
    Smp nop;
    nop.r.pc = 0xfc; // Timer target reads as zero, the NOP opcode.
    nop.GetTimer0().stage3 = 7;
    nop.RunUntil(1);
    Check(nop.GetTimer0().stage3 == 7, "NOP has not reached its dummy timer-output read yet");
    nop.RunUntil(2);
    Check(nop.r.pc == 0xfd && nop.GetTimer0().stage3 == 0,
          "NOP's dummy read clears a timer output at the next PC address");

    Smp modify;
    modify.IoState().iplRomEnable = false;
    modify.r.pc = 0x200;
    modify.r.a = 0x80;
    modify.Ram()[0x200] = 0x0e; // TSET1 $00FD
    modify.Ram()[0x201] = 0xfd;
    auto& timer = modify.GetTimer0();
    timer.stage0 = 118;
    timer.stage1 = 1;
    timer.line = timer.enable = true;
    timer.target = 1;
    timer.stage3 = 7;
    modify.RunUntil(4);
    Check(timer.stage3 == 0, "The first read clears the old timer output");
    modify.RunUntil(5);
    Check(timer.stage3 == 0,
          "The repeated read clears the divider edge between operand read and write");
    modify.RunUntil(6);
    Check(modify.Ram()[0xfd] == 0x87 && timer.stage3 == 0,
          "TSET1 writes its first operand value after both timer read side effects");
}

void OpcodeTraces() {
    unsigned failures = 0;
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        for (unsigned scenario = 0; scenario < 4; ++scenario) {
            const auto fixture = spc_bus_test::Prepare(uint8_t(opcode), scenario);
            Bus cpu;
            cpu.Power();
            cpu.r = fixture.registers;
            cpu.memory = fixture.memory;
            cpu.Step();
            spc_bus_test::Record(cpu.digest, 3, 0, 0, cpu.r);
            const auto expected = spc_bus_test::ExpectedDigests[opcode * 4 + scenario];
            if (cpu.digest != expected) {
                if (failures < 32)
                    std::fprintf(
                        stderr,
                        "Opcode %02X scenario %u: bus/state digest %016llx, expected %016llx\n",
                        opcode, scenario, static_cast<unsigned long long>(cpu.digest),
                        static_cast<unsigned long long>(expected));
                ++failures;
            }
        }
    }
    Check(failures == 0,
          "All 256 sound opcodes preserve complete bus traces and register boundaries");
}
} // namespace

int main() {
    unsigned failures = 0;
    for (auto test : {ImpliedRead, WaitClasses, StackAndRegisterBoundaries, TimerReadSideEffects,
                      OpcodeTraces}) {
        try {
            test();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            ++failures;
        }
    }
    if (failures)
        return 1;
    std::puts("Sound instruction dummy reads, wait classes, and timer side effects passed");
}
