#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace snes::frontend {
struct CpuDecodeWidth {
    bool emulation = true, accumulator8 = true, index8 = true;
};

struct CpuInstruction {
    uint32_t address = 0, next = 0;
    unsigned length = 1;
    std::array<std::optional<uint8_t>, 4> bytes;
    std::string mnemonic, operand;
    bool available = false;
};

CpuInstruction Decode65816(uint32_t address, CpuDecodeWidth width,
                           const std::function<std::optional<uint8_t>(uint32_t)>& reader);
} // namespace snes::frontend
