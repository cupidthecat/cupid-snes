#include "ConsoleConfig.hpp"
#include "debug/CpuDebugger.hpp"
#include "debug/Disassembly.hpp"
#include "snes/core/Processor65816.hpp"
#include "state/EmulatorBoards.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace snes::frontend;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Processor : snes::core::Processor65816 {
    std::array<uint8_t, 4> program;
    unsigned fetched = 0;

    explicit Processor(uint8_t opcode, CpuDecodeWidth width) : program{opcode, 0x12, 0x34, 0x56} {
        power();
        regs().pb = 0x34;
        regs().pc = 0x8000;
        regs().db = 0x55;
        regs().d = 0x1000;
        regs().x = regs().y = 0x100;
        regs().s = 0x3000;
        regs().a = 0;
        regs().e = width.emulation;
        regs().p = (width.accumulator8 ? FlagM : 0) | (width.index8 ? FlagX : 0);
    }

    uint8_t read(uint32_t address) override {
        if (fetched < 4 && address == 0x348000 + fetched)
            return program[fetched++];
        return 0;
    }

    void write(uint32_t, uint8_t) override {
    }

    void idle() override {
    }

    void lastCycle() override {
    }

    bool interruptPending() const override {
        return false;
    }
};

void AllOpcodes() {
    for (bool e : {false, true})
        for (bool m : {false, true})
            for (bool x : {false, true})
                for (unsigned opcode = 0; opcode < 256; ++opcode) {
                    const CpuDecodeWidth width{e, m, x};
                    Processor cpu(uint8_t(opcode), width);
                    std::vector<uint32_t> reads;
                    const auto row = Decode65816(
                        0x348000, width, [&](uint32_t address) -> std::optional<uint8_t> {
                            reads.push_back(address);
                            Check(address >= 0x348000 && address < 0x348004,
                                  "Decoder fetch stays within one instruction");
                            return cpu.program[address - 0x348000];
                        });
                    cpu.instruction();
                    if (cpu.fetched != row.length) {
                        std::fprintf(
                            stderr,
                            "Length mismatch opcode=%02X E=%u M=%u X=%u decoded=%u executed=%u\n",
                            opcode, unsigned(e), unsigned(m), unsigned(x), row.length, cpu.fetched);
                        throw std::runtime_error(
                            "Disassembly length agrees with actual processor operand fetches");
                    }
                    Check(row.available && !row.mnemonic.empty() &&
                              row.next == 0x348000 + row.length && reads.size() == row.length,
                          "Every opcode decodes with the selected widths and only necessary bytes "
                          "are peeked");
                }
}

auto Decode(std::initializer_list<uint8_t> bytes, CpuDecodeWidth width = {},
            uint32_t address = 0x348000) {
    const std::vector<uint8_t> program(bytes);
    return Decode65816(address, width, [&](uint32_t target) -> std::optional<uint8_t> {
        const size_t offset = uint16_t(target - address);
        Check((target & 0xff0000) == (address & 0xff0000),
              "Instruction operands never cross program banks");
        return offset < program.size() ? std::optional(program[offset]) : std::nullopt;
    });
}

void Operands() {
    Check(Decode({0xa9, 0x12, 0x34}, {false, false, true}).operand == "#$3412",
          "Accumulator immediate respects M");
    Check(Decode({0xa2, 0x12, 0x34}, {false, true, false}).operand == "#$3412",
          "Index immediate respects X");
    Check(Decode({0xa9, 0x12, 0x34}, {true, false, false}).length == 2,
          "Emulation forces byte immediates");
    Check(Decode({0x80, 0xfc}, {}, 0x340000).operand == "$34FFFE",
          "Negative relative branches wrap inside the program bank");
    Check(Decode({0x82, 0xfe, 0xff}, {}, 0x34ffff).operand == "$340000",
          "Long relative targets wrap without carrying into the bank");
    Check(Decode({0xa9, 0x12}, {}, 0x34ffff).next == 0x340001,
          "Operand fetch and next PC wrap at FFFF");
    Check(Decode({0x54, 0x12, 0x34}).operand == "$34,$12",
          "Block move operands show source then destination");
    Check(Decode({0xb3, 0x12}).operand == "($12,S),Y", "Stack indirect indexing is explicit");
    Check(Decode({0x17, 0x12}).operand == "[$12],Y", "Long indirect indexing retains brackets");
    Check(Decode({0xfc, 0x12, 0x34}).operand == "($3412,X)",
          "Indexed indirect calls retain their addressing mode");
    Check(Decode({0xdc, 0x12, 0x34}).operand == "[$3412]",
          "Long indirect jumps retain their addressing mode");
    Check(Decode({0x0a}).operand == "A", "Accumulator operations name their register");
    Check(Decode({0x8f, 0x12, 0x34, 0x56}).operand == "$563412",
          "Long addresses use all three operand bytes");
    Check(Decode({0x8f, 0x12}).operand == "??" && !Decode({0x8f, 0x12}).available,
          "Missing operand bytes remain unavailable");
    Check(!Decode({}).available && Decode({}).length == 1,
          "Unavailable opcode never becomes a fabricated instruction");
}

void Mapping() {
    using namespace snes::tests::emulator_state;
    auto machine =
        std::make_unique<snes::core::Emulator>(snes::tests::DeterministicConsoleConfig());
    Load(*machine, Board::Lo);
    auto& bus = machine->GetBus();
    const std::array<uint8_t, 8> program{0xa9, 0x5a, 0x8f, 0x34, 0x12, 0x7e, 0xea, 0xdb};
    for (size_t index = 0; index < program.size(); ++index)
        bus.WramData()[0x100 + index] = program[index];
    const auto before = machine->CaptureState();
    CpuDebugger debugger(*machine);
    const auto rows = debugger.Disassemble(0x7e0100, 4, {});
    Check(rows.size() == 4 && rows[0].mnemonic == "LDA" && rows[1].mnemonic == "STA" &&
              rows[2].address == 0x7e0106 && rows[3].mnemonic == "STP",
          "Mapped disassembly follows actual work RAM bytes");
    const auto unavailable = debugger.Disassemble(0x2139, 8, {});
    Check(unavailable.size() == 1 && !unavailable[0].available,
          "A device port ends the preview without a bus read");
    Check(machine->CaptureState() == before, "Disassembly preserves exact machine state");
}
} // namespace

int main() {
    try {
        AllOpcodes();
        Operands();
        Mapping();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("Disassembly checks passed");
    return 0;
}
