// Regression coverage for CPU instructions, interrupts, and stops.

#include "snes/core/CpuIoRegisters.hpp"
#include "snes/core/Dma.hpp"
#include "snes/core/MemoryBus.hpp"
#include "snes/core/SnesCpu.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

using namespace snes::core;

namespace {
int failures = 0;
int checks = 0;

void Check(const char* label, unsigned actual, unsigned expected) {
    ++checks;
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s: actual=$%X expected=$%X\n", label, actual, expected);
        ++failures;
    }
}

struct Bus : ICpuBus {
    std::array<uint8_t, 65536> ram{};
    std::vector<unsigned> writes;
    unsigned reads = 0;

    Bus() {
        ram.fill(0xea);
        ram[0xfffc] = 0x00;
        ram[0xfffd] = 0x80;
        for (unsigned vector : {0xfff4u, 0xfffau, 0xfffeu, 0xffe4u, 0xffe6u, 0xffeau, 0xffeeu}) {
            ram[vector] = 0x00;
            ram[vector + 1] = 0x90;
        }
    }

    uint8_t Read(uint32_t address) override {
        ++reads;
        return ram[address & 0xffff];
    }

    void Write(uint32_t address, uint8_t data) override {
        writes.push_back(address);
        ram[address & 0xffff] = data;
    }
};

void Boot(SnesCpu& cpu, Bus& bus) {
    cpu.Reset();
    cpu.Step();
    bus.writes.clear();
    bus.reads = 0;
}

void Boot(SnesCpu& cpu, MemoryBus&) {
    cpu.Reset();
    cpu.Step();
}

void CallBusState() {
    struct ObservedBus : Bus {
        SnesCpu* cpu = nullptr;
        std::vector<std::array<uint32_t, 2>> state;

        void Write(uint32_t address, uint8_t value) override {
            if (cpu)
                state.push_back({cpu->regs().pc, cpu->regs().s});
            Bus::Write(address, value);
        }
    };

    for (bool emulation : {false, true})
        for (bool longCall : {false, true}) {
            ObservedBus bus;
            SnesCpu cpu(bus);
            Boot(cpu, bus);
            cpu.regs().e = emulation;
            cpu.regs().s = 0x1ff;
            bus.ram[0x8000] = longCall ? 0x22 : 0x20;
            bus.ram[0x8001] = 0;
            bus.ram[0x8002] = 0x81;
            bus.ram[0x8003] = 0x12;
            bus.cpu = &cpu;
            cpu.Step();
            Check("Call stack bus samples", unsigned(bus.state.size()), longCall ? 3 : 2);
            for (unsigned index = 0; index < bus.state.size(); ++index) {
                Check("Call return PC is visible during stack writes", bus.state[index][0],
                      longCall ? 0x8003 : 0x8002);
                Check("Long-call native pushes decrement SP before the bus write",
                      bus.state[index][1], 0x1ff - index - (longCall ? 1 : 0));
            }
        }
    for (uint8_t opcode : {0x00, 0x02}) {
        ObservedBus bus;
        SnesCpu cpu(bus);
        Boot(cpu, bus);
        cpu.regs().e = false;
        cpu.regs().pb = 0x12;
        cpu.regs().s = 0x1ff;
        bus.ram[0x8000] = opcode;
        bus.cpu = &cpu;
        cpu.Step();
        Check("Software interrupt bank pushes retain SP during the bus write", bus.state[0][1],
              0x1ff);
    }
}

void StackPullBusState() {
    struct ObservedBus : Bus {
        SnesCpu* cpu = nullptr;
        std::vector<std::array<uint16_t, 5>> state;

        uint8_t Read(uint32_t address) override {
            if (cpu && (address & 0xffff) >= 0x1f1 && (address & 0xffff) <= 0x1f4) {
                const auto& r = cpu->regs();
                state.push_back({r.a, r.x, r.y, r.d, r.pc});
            }
            return Bus::Read(address);
        }
    };

    for (uint8_t opcode : {0x68, 0xfa, 0x7a, 0x2b, 0x40}) {
        for (bool emulation : {false, true}) {
            if (emulation && opcode != 0x2b && opcode != 0x40)
                continue;
            ObservedBus bus;
            SnesCpu cpu(bus);
            Boot(cpu, bus);
            auto& r = cpu.regs();
            r.e = emulation;
            r.p = emulation ? 0x34 : 0;
            r.a = r.x = r.y = r.d = 0xabcd;
            r.s = 0x1f0;
            bus.ram[0x8000] = opcode;
            bus.ram[0x1f1] = opcode == 0x40 ? 0 : 0x34;
            bus.ram[0x1f2] = opcode == 0x40 ? 0x34 : 0x12;
            bus.ram[0x1f3] = 0x12;
            bus.ram[0x1f4] = 0x23;
            bus.cpu = &cpu;
            cpu.Step();
            const unsigned count = opcode == 0x40 ? (emulation ? 3 : 4) : 2;
            Check("Pull bus sample count", unsigned(bus.state.size()), count);
            if (bus.state.size() != count)
                continue;
            if (opcode == 0x40) {
                Check("RTI publishes PC low before reading PC high", bus.state[2][4], 0x8034);
                if (!emulation)
                    Check("Native RTI publishes both PC bytes before reading PB", bus.state[3][4],
                          0x1234);
            } else {
                const unsigned reg = opcode == 0x68   ? 0
                                     : opcode == 0xfa ? 1
                                     : opcode == 0x7a ? 2
                                                      : 3;
                Check("A word pull retains its register before the first read", bus.state[0][reg],
                      0xabcd);
                Check("A word pull publishes its low byte before the second read",
                      bus.state[1][reg], 0xab34);
            }
        }
    }
}

void ModifyFlagSampling() {
    for (uint8_t opcode :
         {0x06, 0x0e, 0x16, 0x1e, 0x26, 0x2e, 0x36, 0x3e, 0x46, 0x4e, 0x56, 0x5e, 0x66, 0x6e,
          0x76, 0x7e, 0xc6, 0xce, 0xd6, 0xde, 0xe6, 0xee, 0xf6, 0xfe, 0x04, 0x0c, 0x14, 0x1c})
        for (uint8_t value : {0, 1, 0x80, 0xff})
            for (bool emulation : {false, true}) {
                Bus bus;
                SnesCpu cpu(bus);
                Boot(cpu, bus);
                cpu.regs().e = emulation;
                cpu.regs().p = 0x34;
                cpu.regs().a = 0xaa;
                cpu.regs().x = 3;
                cpu.regs().d = 0x200;
                bus.ram[0x8000] = opcode;
                bus.ram[0x8001] = 0x10;
                bus.ram[0x8002] = 2;
                bus.ram[0x210] = bus.ram[0x213] = value;
                unsigned samples = 0, status = 0;
                cpu.SetInterruptPollCallback([&] {
                    ++samples;
                    status = cpu.regs().p;
                    return false;
                });
                cpu.Step();
                Check("Memory modification samples one interrupt phase", samples, 1);
                Check("Byte memory modification publishes flags before interrupt sampling", status,
                      cpu.regs().p);
            }
}

void IndexedIndirectPointerBank() {
    struct ObservedBus : Bus {
        uint32_t lowAddress = ~0u, highAddress = ~0u;
        std::vector<uint32_t> addresses;

        uint8_t Read(uint32_t address) override {
            addresses.push_back(address);
            if (address == lowAddress)
                return 0x34;
            if (address == highAddress)
                return 0x12;
            return Bus::Read(address);
        }
    };

    struct Case {
        bool emulation;
        uint16_t direct;
        uint8_t operand, index;
        uint32_t low, high;
    };

    const Case cases[] = {
        {true, 0xffff, 2, 0xff, 0x10100, 0x10101}, {true, 0xffff, 1, 0, 0x10000, 0x10001},
        {true, 0xfeff, 1, 0xff, 0xffff, 0xff00},   {true, 0xff00, 0xff, 0xff, 0xfffe, 0xffff},
        {false, 0xffff, 2, 0xff, 0x100, 0x101},    {true, 0, 0xff, 1, 0, 1}};
    for (uint8_t opcode : {0x01, 0x21, 0x41, 0x61, 0x81, 0xa1, 0xc1, 0xe1})
        for (const auto& test : cases) {
            ObservedBus bus;
            SnesCpu cpu(bus);
            Boot(cpu, bus);
            auto& r = cpu.regs();
            r.e = test.emulation;
            r.p = 0x35;
            r.a = 0xab42;
            r.x = test.index;
            r.d = test.direct;
            bus.lowAddress = test.low;
            bus.highAddress = test.high;
            bus.ram[0x8000] = opcode;
            bus.ram[0x8001] = test.operand;
            bus.addresses.clear();
            cpu.Step();
            Check("Indexed indirect operand-read count", unsigned(bus.addresses.size()),
                  opcode == 0x81 ? 4 : 5);
            if (bus.addresses.size() >= 4) {
                Check("Indexed indirect pointer low retains its address carry", bus.addresses[2],
                      test.low);
                Check("Indexed indirect pointer high retains its selected page", bus.addresses[3],
                      test.high);
            }
            const auto address = opcode == 0x81 ? (bus.writes.empty() ? 0 : bus.writes.back())
                                                : bus.addresses.back();
            Check("Indexed indirect data uses the fetched pointer", address, 0x1234);
        }
}

void InterruptVectorBusState() {
    struct ObservedBus : Bus {
        SnesCpu* cpu = nullptr;
        bool replaceLow = false;
        std::vector<uint32_t> pc;

        uint8_t Read(uint32_t address) override {
            if (cpu && address >= 0xffe0 && address <= 0xffff) {
                pc.push_back((uint32_t(cpu->regs().pb) << 16) | cpu->regs().pc);
                if (replaceLow && (address & 1))
                    cpu->regs().pc = static_cast<uint16_t>((cpu->regs().pc & 0xff00) | 0x78);
            }
            return Bus::Read(address);
        }
    };

    for (unsigned kind = 0; kind < 4; ++kind)
        for (bool emulation : {false, true})
            for (bool replaceLow : {false, true})
                for (uint8_t bank : {0, 0x12}) {
                    ObservedBus bus;
                    SnesCpu cpu(bus);
                    Boot(cpu, bus);
                    auto& r = cpu.regs();
                    r.e = emulation;
                    r.p = 0x39;
                    r.pb = bank;
                    bus.ram[0x8000] = kind == 0 ? 0x00 : kind == 1 ? 0x02 : 0xea;
                    const uint16_t vectors[2][4] = {{0xffe6, 0xffe4, 0xffea, 0xffee},
                                                    {0xfffe, 0xfff4, 0xfffa, 0xfffe}};
                    const uint16_t vector = vectors[emulation][kind];
                    bus.ram[vector] = 0x34;
                    bus.ram[vector + 1] = 0x12;
                    if (kind == 2)
                        cpu.RequestNmi();
                    if (kind == 3)
                        cpu.SetIrqLevel(true);
                    if (kind >= 2) {
                        cpu.Step();
                        cpu.SetIrqLevel(false);
                    }
                    const unsigned returnPc = kind < 2 ? 0x8002 : 0x8001;
                    bus.cpu = &cpu;
                    bus.replaceLow = replaceLow;
                    std::vector<uint32_t> sampledPc;
                    cpu.SetInterruptPollCallback([&] {
                        sampledPc.push_back((uint32_t(r.pb) << 16) | r.pc);
                        return false;
                    });
                    cpu.Step();
                    Check("Interrupt vector-read count", unsigned(bus.pc.size()), 2);
                    if (bus.pc.size() == 2) {
                        Check("The low vector read retains the program bank and return PC",
                              bus.pc[0], (uint32_t(bank) << 16) | returnPc);
                        Check("The high vector read observes the fetched PC low byte", bus.pc[1],
                              (uint32_t(bank) << 16) | 0x8034);
                    }
                    Check("Interrupt sample count", unsigned(sampledPc.size()), 1);
                    if (sampledPc.size() == 1)
                        Check(
                            "Interrupt sampling observes the partial vector and live program bank",
                            sampledPc[0], (uint32_t(bank) << 16) | 0x8034);
                    Check("Interrupt clears its program bank after fetching the vector", r.pb, 0);
                    Check("A high vector read preserves the live low PC byte", r.pc,
                          replaceLow ? 0x1278 : 0x1234);
                }
    ObservedBus bus;
    SnesCpu cpu(bus);
    cpu.Reset();
    bus.ram[0xfffc] = 0x34;
    bus.ram[0xfffd] = 0x12;
    bus.cpu = &cpu;
    std::vector<uint32_t> sampledPc;
    cpu.SetInterruptPollCallback([&] {
        sampledPc.push_back((uint32_t(cpu.regs().pb) << 16) | cpu.regs().pc);
        return false;
    });
    cpu.Step();
    Check("Reset vector-read count", unsigned(bus.pc.size()), 2);
    if (bus.pc.size() == 2) {
        Check("Reset retains its PC before the low vector read", bus.pc[0], 0);
        Check("Reset publishes PC low before the high vector read", bus.pc[1], 0x34);
    }
    Check("Reset interrupt sample count", unsigned(sampledPc.size()), 1);
    if (sampledPc.size() == 1)
        Check("Reset sampling observes the fetched PC low byte", sampledPc[0], 0x34);
    Check("Reset publishes the complete vector", cpu.regs().pc, 0x1234);
}

void InterruptStack() {
    // Exercise every byte of an interrupt frame across the page boundary.
    for (bool emulation : {false, true}) {
        for (unsigned stack : {0x100u, 0x101u, 0x102u, 0x1ffu}) {
            for (unsigned kind = 0; kind < 4; ++kind) {
                Bus bus;
                SnesCpu cpu(bus);
                Boot(cpu, bus);
                auto& r = cpu.regs();
                r.e = emulation;
                r.s = static_cast<uint16_t>(stack);
                r.p = 0x39; // Decimal and carry set, IRQ unmasked.
                r.pb = emulation ? 0 : 0x12;
                bus.ram[0x8000] = kind == 0 ? 0x00 : kind == 1 ? 0x02 : 0xea;
                bus.ram[0x9000] = 0x40; // RTI
                if (kind == 2)
                    cpu.RequestNmi();
                if (kind == 3)
                    cpu.SetIrqLevel(true);
                cpu.Step();
                if (kind >= 2) {
                    cpu.SetIrqLevel(false);
                    cpu.Step();
                }
                Check("Interrupt vector", r.pc, 0x9000);
                Check("Interrupt clears decimal and masks IRQ", r.p & 0x0c, 0x04);
                const unsigned frameSize = emulation ? 3 : 4;
                Check("Interrupt frame size", unsigned(bus.writes.size()), frameSize);
                for (unsigned i = 0; i < bus.writes.size(); ++i) {
                    const unsigned address =
                        emulation ? 0x100 | ((stack - i) & 0xff) : (stack - i) & 0xffff;
                    Check("Interrupt stack write address", bus.writes[i], address);
                }
                const unsigned expectedStack =
                    emulation ? 0x100 | ((stack - frameSize) & 0xff) : (stack - frameSize) & 0xffff;
                Check("Interrupt stack pointer", r.s, expectedStack);
                const unsigned statusAddress =
                    emulation ? 0x100 | ((stack - 2) & 0xff) : (stack - 3) & 0xffff;
                Check("Saved interrupt status", bus.ram[statusAddress],
                      emulation && kind >= 2 ? 0x29 : 0x39);
                cpu.Step();
                Check("RTI restores PC", r.pc, kind < 2 ? 0x8002 : 0x8001);
                Check("RTI restores stack", r.s, stack);
                Check("RTI restores bank", r.pb, emulation ? 0 : 0x12);
                Check("RTI restores flags", r.p, 0x39);
            }
        }
    }
}

void ExtendedStackInstructions() {
    for (bool emulation : {false, true}) {
        for (unsigned opcode : {0x0bu, 0x2bu, 0xf4u, 0xd4u, 0x62u, 0x22u, 0x6bu, 0xfcu}) {
            Bus bus;
            SnesCpu cpu(bus);
            Boot(cpu, bus);
            auto& r = cpu.regs();
            r.e = emulation;
            const bool pull = opcode == 0x2b || opcode == 0x6b;
            const unsigned bytes = opcode == 0x22 || opcode == 0x6b ? 3 : 2;
            r.s = pull ? 0x1ff : 0x100;
            r.d = opcode == 0xd4 ? 0x3400 : 0x5678;
            bus.ram[0x8000] = static_cast<uint8_t>(opcode);
            bus.ram[0x8001] = opcode == 0xd4 ? 0x10 : 0x78;
            bus.ram[0x8002] = 0x56;
            bus.ram[0x8003] = 0x12;
            bus.ram[0x3410] = 0x78;
            bus.ram[0x3411] = 0x56;
            bus.ram[0x5678] = 0x00;
            bus.ram[0x5679] = 0x90;
            bus.ram[0x200] = 0x78;
            bus.ram[0x201] = 0x56;
            bus.ram[0x202] = 0x12;
            cpu.Step();
            const unsigned fullStack = pull ? 0x1ff + bytes : 0x100 - bytes;
            const unsigned finalStack = emulation ? 0x100 | (fullStack & 0xff) : fullStack;
            Check("Extended instruction restores stack page", r.s, finalStack);
            if (pull) {
                Check("Extended pull crosses stack page", opcode == 0x2b ? r.d : r.pc,
                      opcode == 0x2b ? 0x5678 : 0x5679);
                if (opcode == 0x6b)
                    Check("RTL restores bank", r.pb, 0x12);
            } else {
                Check("Extended push byte count", unsigned(bus.writes.size()), bytes);
                for (unsigned i = 0; i < bus.writes.size(); ++i)
                    Check("Extended push crosses stack page", bus.writes[i], 0x100 - i);
            }
            // A following ordinary push must use the restored page.
            bus.ram[r.pc] = 0x48; // PHA
            cpu.Step();
            Check("Following push uses restored stack", bus.writes.back(), finalStack);
        }
    }
}

void WaitInterruptTiming() {
    struct WaitBus : Bus {
        uint8_t Speed(uint32_t) const override {
            return 8;
        }
    };

    for (bool emulation : {false, true})
        for (unsigned kind = 0; kind < 3; ++kind)
            for (unsigned arrival = 0; arrival <= 64; arrival += 2)
                for (unsigned lockedUntil : {0u, 19u, 32u, 65u}) {
                    WaitBus bus;
                    SnesCpu cpu(bus);
                    Boot(cpu, bus);
                    cpu.regs().e = emulation;
                    if (kind != 2)
                        cpu.regs().p &= ~Processor65816::FlagI;
                    bus.ram[0x8000] = 0xcb;
                    unsigned clocks = 0;
                    bool sent = false;
                    const auto signal = [&] {
                        sent = true;
                        if (kind == 0)
                            cpu.RequestNmi();
                        else
                            cpu.SetIrqLevel(true);
                    };
                    cpu.SetClockCallback([&](uint32_t amount) {
                        clocks += amount;
                        if (!sent && clocks >= arrival)
                            signal();
                    });
                    cpu.SetInterruptPollCallback([&] { return clocks >= lockedUntil; });
                    if (!arrival)
                        signal();
                    for (unsigned step = 0; step < 64; ++step) {
                        cpu.Step();
                        if ((kind == 2 && !cpu.regs().wai) ||
                            (kind != 2 && cpu.regs().pc == 0x9000))
                            break;
                    }
                    const unsigned available = std::max(arrival, lockedUntil);
                    const unsigned sample = 8 + (available > 8 ? (available - 8 + 5) / 6 * 6 : 0);
                    const unsigned expected = sample + 12 + (kind == 2 ? 0 : emulation ? 54 : 62);
                    Check("WAI samples before an idle and retains its final wake cycle", clocks,
                          expected);
                    Check("WAI clears after the accepted sample", cpu.regs().wai, false);
                    Check("Masked WAI wake preserves the next opcode", cpu.regs().pc,
                          kind == 2 ? 0x8001 : 0x9000);
                    Check("Masked WAI wake leaves the stack untouched", unsigned(bus.writes.size()),
                          kind == 2   ? 0
                          : emulation ? 3
                                      : 4);
                }
}

void WaitInterrupt() {
    for (bool nmi : {false, true}) {
        for (bool beforeWait : {false, true}) {
            Bus bus;
            SnesCpu cpu(bus);
            Boot(cpu, bus);
            cpu.regs().p &= ~Processor65816::FlagI;
            bus.ram[0x8000] = 0xcb; // WAI
            bus.ram[0x9000] = 0xa9; // LDA #$57
            bus.ram[0x9001] = 0x57;
            bus.ram[0x9002] = 0x40; // RTI
            auto signal = [&] {
                if (nmi)
                    cpu.RequestNmi();
                else
                    cpu.SetIrqLevel(true);
            };
            if (beforeWait)
                signal();
            cpu.Step();
            if (!beforeWait) {
                signal();
                cpu.Step(); // Wake and schedule the interrupt.
            }
            cpu.SetIrqLevel(false);
            cpu.Step();
            Check("WAI interrupt vector", cpu.regs().pc, 0x9000);
            Check("Interrupt releases WAI", cpu.regs().wai, false);
            cpu.Step();
            Check("WAI handler executes", cpu.regs().a, 0x57);
            cpu.Step();
            Check("WAI RTI resumes next instruction", cpu.regs().pc, 0x8001);
        }
    }
    Bus bus;
    SnesCpu cpu(bus);
    Boot(cpu, bus); // IRQ remains masked.
    bus.ram[0x8000] = 0xcb;
    bus.ram[0x8001] = 0xa9;
    bus.ram[0x8002] = 0x63;
    cpu.Step();
    cpu.SetIrqLevel(true);
    cpu.Step();
    Check("Masked IRQ releases WAI", cpu.regs().wai, false);
    cpu.Step();
    Check("Masked IRQ resumes without vectoring", cpu.regs().a, 0x63);
    Check("Masked IRQ leaves stack alone", unsigned(bus.writes.size()), 0);
}

void PullBankBoundaries() {
    struct Case {
        bool emulation;
        unsigned stack, address, finalStack;
    };

    for (const auto& test : {Case{true, 0x1ff, 0x200, 0x100}, Case{true, 0x1fe, 0x1ff, 0x1ff},
                             Case{false, 0x1ff, 0x200, 0x200}, Case{false, 0xffff, 0, 0}}) {
        for (uint8_t value : {uint8_t{0}, uint8_t{0x3d}, uint8_t{0x80}}) {
            Bus bus;
            SnesCpu cpu(bus);
            Boot(cpu, bus);
            auto& r = cpu.regs();
            r.e = test.emulation;
            r.s = uint16_t(test.stack);
            r.p = 0x7f;
            bus.ram[0x8000] = 0xab;
            bus.ram[test.address] = value;
            Check("PLB takes four cycles at the stack boundary", cpu.Step(), 24);
            Check("PLB reads the full-width incremented stack address", r.db, value);
            Check("PLB restores the emulation stack page after the read", r.s, test.finalStack);
            Check("PLB updates only negative and zero flags", r.p,
                  (0x7f & ~0x82) | (value & 0x80) | (value == 0 ? 2 : 0));
            bus.ram[r.pc] = 0x48;
            cpu.Step();
            Check("Push following PLB uses the restored stack pointer", bus.writes.back(),
                  test.finalStack);
        }
    }
}

void StackInstructionTiming() {
    // W65C816S table 5-4: (d,S),Y takes seven cycles with an 8-bit
    // accumulator, eight with a 16-bit accumulator; PER always takes six.
    for (unsigned flags : {0u, 0x10u, 0x20u, 0x30u}) {
        for (unsigned opcode : {0x13u, 0x33u, 0x53u, 0x73u, 0x93u, 0xb3u, 0xd3u, 0xf3u}) {
            for (unsigned index : {0u, 0xffu}) {
                Bus bus;
                SnesCpu cpu(bus);
                Boot(cpu, bus);
                auto& r = cpu.regs();
                r.e = false;
                r.p = uint8_t(flags | Processor65816::FlagI);
                r.s = 0x1f0;
                r.y = uint16_t(index);
                r.a = 0x2153;
                bus.ram[0x8000] = uint8_t(opcode);
                bus.ram[0x8001] = 0x10;
                bus.ram[0x200] = 0x80;
                bus.ram[0x201] = 0x20;
                bus.ram[0x2080 + index] = 0x35;
                bus.ram[0x2081 + index] = 0x42;
                const unsigned width = (flags & 0x20) ? 1 : 2;
                const unsigned beforeReads = bus.reads;
                Check("Indirect stack instruction clocks", cpu.Step(), (6 + width) * 6);
                Check("Indirect stack bus reads", bus.reads - beforeReads,
                      opcode == 0x93 ? 4 : 4 + width);
                Check("Indirect stack bus writes", unsigned(bus.writes.size()),
                      opcode == 0x93 ? width : 0);
                Check("Indirect stack preserves stack pointer", r.s, 0x1f0);
            }
        }
        for (bool emulation : {false, true}) {
            Bus bus;
            SnesCpu cpu(bus);
            Boot(cpu, bus);
            cpu.regs().e = emulation;
            cpu.regs().p = uint8_t(flags | Processor65816::FlagI | (emulation ? 0x30 : 0));
            bus.ram[0x8000] = 0x62;
            bus.ram[0x8001] = 0xfd;
            bus.ram[0x8002] = 0xff;
            const unsigned stack = cpu.regs().s;
            Check("PER instruction clocks", cpu.Step(), 36);
            Check("PER pushes the relative target high byte", bus.ram[stack], 0x80);
            Check("PER pushes the relative target low byte", bus.ram[(stack - 1) & 0xffff], 0x00);
        }
    }
}

void CpuIoOpenBus() {
    auto bus = std::make_unique<MemoryBus>();
    CpuIoRegisters io;
    DmaController dma;
    bus->MapWram();
    bus->MapCpuIo(io);
    bus->MapDma(dma);
    dma.Write(0x4300, 0x96);
    io.SetJoypadDataCallback([](int port) { return uint8_t(port + 1); });

    for (unsigned bank : {0x00u, 0x3fu, 0x80u, 0xbfu}) {
        const uint32_t base = bank << 16;
        for (uint8_t seed : {uint8_t{0}, uint8_t{0x3f}, uint8_t{0xa5}, uint8_t{0xff}}) {
            bus->Write(0x7e0001, uint8_t(seed ^ 0xff));
            bus->WramData()[0] = seed;
            bus->Read(0x7e0000);
            Check("RDIO drives all eight bits", bus->Read(base | 0x4213), io.pio());
            Check("CPU I/O read retains the memory latch", bus->OpenBus(), seed);
            Check("Write-only I/O reads the memory latch", bus->Read(base | 0x4201), seed);
            Check("DMA control read drives all eight bits", bus->Read(base | 0x4300), 0x96);
            Check("Unused DMA register reads the memory latch", bus->Read(base | 0x430c), seed);
            io.setNmiFlag(true);
            io.setIrqFlag(false);
            Check("RDNMI preserves memory open-bus bits", bus->Read(base | 0x4210),
                  (seed & 0x70) | 0x82);
            Check("TIMEUP does not inherit RDNMI bits", bus->Read(base | 0x4211), seed & 0x7f);
            Check("HVBJOY preserves memory open-bus bits", bus->Read(base | 0x4212), seed & 0x3e);
            Check("Joypad zero preserves memory open-bus bits", bus->Read(base | 0x4016),
                  (seed & 0xfc) | 1);
            Check("Joypad one preserves memory open-bus bits", bus->Read(base | 0x4017),
                  (seed & 0xe0) | 0x1e);
            Check("Unmapped internal I/O reads the memory latch", bus->Read(base | 0x43ff), seed);
            Check("Consecutive internal reads preserve MDR", bus->OpenBus(), seed);
            bus->Write(base | 0x4201, uint8_t(seed ^ 0xff));
            Check("Internal I/O writes update MDR", bus->Read(base | 0x430c), uint8_t(seed ^ 0xff));
        }
    }

    for (uint8_t bank : {uint8_t{0x40}, uint8_t{0x7f}, uint8_t{0xc0}, uint8_t{0xff}}) {
        bus->Map(
            bank, bank, 0x4210, 0x4210, [](uint32_t, uint8_t) { return uint8_t(0x5c); },
            [](uint32_t, uint8_t) {});
        bus->SetOpenBus(0xaa);
        Check("Non-I/O banks return mapped data", bus->Read((uint32_t(bank) << 16) | 0x4210), 0x5c);
        Check("Non-I/O banks latch mapped data", bus->OpenBus(), 0x5c);
    }
}

void CpuIoInstructionReads() {
    auto bus = std::make_unique<MemoryBus>();
    CpuIoRegisters io;
    DmaController dma;
    bus->MapCpuIo(io);
    bus->MapDma(dma);
    std::array<uint8_t, 0x8000> rom{};
    rom[0x7ffd] = 0x80;
    bus->Map(
        0, 0, 0x8000, 0xffff, [&](uint32_t address, uint8_t) { return rom[address & 0x7fff]; },
        [](uint32_t, uint8_t) {});
    io.SetJoypadDataCallback([](int port) { return uint8_t(port + 1); });
    SnesCpu cpu(*bus);

    struct ReadCase {
        uint16_t address;
        uint8_t expected;
    };

    for (uint8_t bank : {uint8_t{0}, uint8_t{0x3f}, uint8_t{0x80}, uint8_t{0xbf}}) {
        for (auto test : {ReadCase{0x4210, 0xc2}, ReadCase{0x4211, 0xc2}, ReadCase{0x4212, 0x02},
                          ReadCase{0x4016, 0x41}, ReadCase{0x4017, 0x5e}, ReadCase{0x430c, 0x43},
                          ReadCase{0x4201, 0x42}, ReadCase{0x4000, 0x40}, ReadCase{0x43ff, 0x43}}) {
            Boot(cpu, *bus);
            cpu.regs().db = bank;
            io.setNmiFlag(true);
            io.setIrqFlag(true);
            rom[0] = 0xad; // LDA absolute: final operand byte remains on the bus.
            rom[1] = uint8_t(test.address);
            rom[2] = uint8_t(test.address >> 8);
            cpu.Step();
            Check("LDA I/O uses the last address byte for open bus", cpu.regs().a, test.expected);
            Check("LDA I/O preserves bus MDR", bus->OpenBus(), rom[2]);
            Check("LDA I/O preserves CPU MDR", cpu.regs().mdr, rom[2]);
        }

        Boot(cpu, *bus);
        cpu.regs().e = false;
        cpu.regs().p = Processor65816::FlagI; // 16-bit accumulator.
        io.setNmiFlag(true);
        io.setIrqFlag(true);
        rom[0] = 0xaf; // LDA long $bb4210 reads both RDNMI and TIMEUP.
        rom[1] = 0x10;
        rom[2] = 0x42;
        rom[3] = bank;
        cpu.Step();
        const unsigned expected = ((bank & 0x70) | 0x82) | (((bank & 0x7f) | 0x80) << 8);
        Check("Both halves of a 16-bit I/O read use the bank operand", cpu.regs().a, expected);
        Check("16-bit I/O read retains bus MDR", bus->OpenBus(), bank);
        Check("16-bit I/O read retains CPU MDR", cpu.regs().mdr, bank);
    }
}

void Stop() {
    for (bool pending : {false, true}) {
        Bus bus;
        SnesCpu cpu(bus);
        Boot(cpu, bus);
        cpu.regs().p &= ~Processor65816::FlagI;
        bus.ram[0x8000] = 0xdb; // STP
        if (pending) {
            cpu.RequestNmi();
            cpu.SetIrqLevel(true);
        }
        Check("STP instruction clocks", cpu.Step(), 18);
        Check("STP sets stopped state", cpu.regs().stp, true);
        cpu.RequestNmi();
        cpu.SetIrqLevel(true);
        const unsigned reads = bus.reads;
        for (unsigned i = 0; i < 4; ++i)
            Check("Stopped step clocks", cpu.Step(), 6);
        Check("STP ignores interrupt vectoring", cpu.regs().pc, 0x8001);
        Check("STP does not read bus", bus.reads, reads);
        Check("STP does not push interrupt frames", unsigned(bus.writes.size()), 0);
        bus.ram[0x8000] = 0xea;
        Boot(cpu, bus);
        Check("Reset clears stopped state", cpu.regs().stp, false);
        cpu.Step();
        Check("Reset resumes execution", cpu.regs().pc, 0x8001);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "stop")
        Stop();
    else {
        CallBusState();
        StackPullBusState();
        ModifyFlagSampling();
        IndexedIndirectPointerBank();
        InterruptVectorBusState();
        InterruptStack();
        ExtendedStackInstructions();
        WaitInterruptTiming();
        WaitInterrupt();
        StackInstructionTiming();
        PullBankBoundaries();
        CpuIoOpenBus();
        CpuIoInstructionReads();
    }
    std::printf("%d CPU checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
