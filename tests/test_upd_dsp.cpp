#include "snes/core/UpdDsp.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using snes::core::UpdDsp;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

constexpr uint32_t Load(uint16_t value, unsigned destination) {
    return 0xc00000 | (uint32_t(value) << 6) | destination;
}

constexpr uint32_t Operation(unsigned arithmetic, unsigned accumulator, unsigned source,
                             unsigned destination) {
    return 0x100000 | (arithmetic << 16) | (accumulator << 15) | (source << 4) | destination;
}

void DataPort() {
    auto chip = std::make_unique<UpdDsp>();
    Check(chip->ReadStatus() == 0, "Reset does not request a host transaction before execution");
    chip->Program()[0] = Load(0x0400, 7);
    chip->Program()[1] = Load(0x1234, 6);
    chip->Program()[2] = Load(0, 7);
    chip->Program()[3] = Load(0xabcd, 6);
    chip->Step();
    chip->Step();
    Check(chip->ReadStatus() == 0x84, "Firmware requests an eight-bit data transaction");
    chip->WriteData(0x56);
    Check(chip->ReadStatus() == 4 && chip->State().data == 0x1256,
          "An eight-bit host write acknowledges the request and retains the upper byte");
    chip->Step();
    chip->Step();
    Check(chip->ReadData() == 0xcd && chip->ReadStatus() == 0x90,
          "The first word read retains the request and selects the high byte");
    Check(chip->ReadData() == 0xab && chip->ReadStatus() == 0,
          "Completing a word read acknowledges the request");
    chip->State().status = 0x8000;
    chip->WriteData(0x79);
    Check(chip->ReadStatus() == 0x90, "A low-byte write leaves a word request active");
    chip->WriteData(0xf2);
    Check(chip->State().data == 0xf279 && chip->ReadStatus() == 0,
          "A high-byte write completes the little-endian host word");
    chip->State().status = 0x9400;
    Check(chip->ReadStatus() == 0x84, "Byte mode hides a retained word-phase latch");
    chip->ReadData();
    Check(chip->State().status == 0x1400, "Byte mode acknowledges without changing that latch");
}

void ArithmeticAndParallelMoves() {
    struct Case {
        unsigned operation;
        uint16_t accumulator, operand;
        uint8_t beforeFlags, otherFlags;
        uint16_t result;
        uint8_t flags;
    };

    constexpr std::array cases{
        Case{5, 0x7fff, 1, 0, 0, 0x8000, 0x33},
        Case{5, 0xffff, 1, 0, 0, 0, 0x0c},
        Case{4, 0, 1, 0, 0, 0xffff, 0x38},
        Case{7, 0, 0xffff, 0, UpdDsp::Carry, 0, 4},
        Case{5, 0x8000, 0x8000, UpdDsp::StickyOverflow, 0, 0, 0x0f},
        Case{5, 0x8000, 0x8000, UpdDsp::StickyOverflow | UpdDsp::ExtendedSign, 0, 0, 0x2d},
        Case{12, 0x8001, 0, 0, UpdDsp::Carry, 3, 8},
        Case{11, 0x8001, 0, 0, 0, 0xc000, 0x38},
        Case{13, 0x4000, 0, 0, 0, 3, 0},
        Case{14, 0x1234, 0, 0, 0, 0x234f, 0},
        Case{15, 0x0180, 0, 0, 0, 0x8001, 0x30},
    };
    auto chip = std::make_unique<UpdDsp>();
    for (unsigned accumulator = 0; accumulator < 2; ++accumulator) {
        for (const auto& test : cases) {
            chip->Reset();
            auto& state = chip->State();
            state.accumulator[accumulator] = test.accumulator;
            state.flags[accumulator] = test.beforeFlags;
            state.flags[accumulator ^ 1] = test.otherFlags;
            state.temporary = test.operand;
            chip->Program()[0] = Operation(test.operation, accumulator, 3, 0);
            chip->Step();
            Check(state.accumulator[accumulator] == test.result &&
                      state.flags[accumulator] == test.flags,
                  "Both accumulators retain the hardware overflow, carry and sign rules");
        }
    }
    chip->Reset();
    chip->State().accumulator[0] = 0x1234;
    chip->Program()[0] = Operation(5, 0, 1, 3);
    chip->Step();
    Check(chip->State().accumulator[0] == 0x2468 && chip->State().temporary == 0x1234,
          "A parallel move samples its source before the arithmetic result is written");
    chip->State().factorX = -32768;
    chip->State().factorY = -32768;
    chip->Program()[1] = 0;
    chip->Step();
    Check(chip->State().productHigh == 0x8000 && chip->State().productLow == 0,
          "The signed multiplier publishes its split product on every instruction");
}

void PointersAndBranches() {
    auto chip = std::make_unique<UpdDsp>(UpdDsp::Revision::Upd96050);
    chip->State().ramPointer = 0x7ff;
    chip->Program()[0] = (1 << 13) | (9 << 9);
    chip->Step();
    Check(chip->State().ramPointer == 0x60,
          "Low-nibble pointer arithmetic precedes the high-nibble XOR");
    chip->State().temporary = 0x7ff;
    chip->Program()[1] = (3 << 13) | (15 << 9) | (3 << 4) | 4;
    chip->Step();
    Check(chip->State().ramPointer == 0x7ff, "Loading DP suppresses both pointer modifiers");
    chip->State().pc = 0x2fff;
    chip->State().stackPointer = 15;
    chip->Program()[0x2fff] = 0x800000 | (0x140 << 13) | (0x234 << 2) | 1;
    chip->Step();
    Check(chip->State().pc == 0xa34 && chip->State().stack[15] == 0x3000 &&
              chip->State().stackPointer == 0,
          "A low-bank call retains the return page and wraps the stack pointer");
    chip->Program()[0xa34] = 0x400000;
    chip->Step();
    Check(chip->State().pc == 0x3000 && chip->State().stackPointer == 15,
          "Return performs its parallel operation before restoring the program counter");
    chip->State().flags[1] = UpdDsp::ExtendedSign;
    chip->Program()[0x3000] = 0x800000 | (0xae << 13) | (0x123 << 2);
    chip->Step();
    Check(chip->State().pc == 0x2123, "A conditional branch preserves the current high page");
}

void FirmwareAndReset() {
    for (const auto revision : {UpdDsp::Revision::Upd7725, UpdDsp::Revision::Upd96050}) {
        auto chip = std::make_unique<UpdDsp>(revision);
        std::vector<uint8_t> bytes(chip->FirmwareSize());
        const auto instruction = Load(0xa579, 3);
        for (unsigned i = 0; i < 3; ++i)
            bytes[i] = static_cast<uint8_t>(instruction >> (8 * i));
        const size_t dataOffset = revision == UpdDsp::Revision::Upd7725 ? 6144 : 49152;
        bytes[dataOffset] = 0x35;
        bytes[dataOffset + 1] = 0x82;
        Check(chip->LoadFirmware(bytes), "A complete firmware image loads");
        chip->Step();
        Check(chip->State().temporary == 0xa579 && chip->DataRom()[0] == 0x8235,
              "Firmware words decode in little-endian order");
        chip->WriteRam(0x1fff, 0x65);
        chip->WriteRam(0x1ffe, 0x43);
        chip->Reset();
        Check(chip->Cycles() == 0 && chip->State().pc == 0 && chip->ReadStatus() == 0 &&
                  chip->ReadRam(0xffe) == 0x43 && chip->ReadRam(0xfff) == 0x65,
              "Processor reset clears execution state and retains the shared RAM address lines");
        Check(!chip->LoadFirmware(std::span<const uint8_t>(bytes).first(bytes.size() - 1)) &&
                  chip->ReadRam(0xffe) == 0x43,
              "An incomplete firmware image leaves the loaded device intact");
    }
}
} // namespace

int main() {
    try {
        DataPort();
        ArithmeticAndParallelMoves();
        PointersAndBranches();
        FirmwareAndReset();
        std::puts("Cartridge DSP instruction and data-port tests passed");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
