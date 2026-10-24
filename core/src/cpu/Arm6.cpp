#include "snes/core/Arm6.hpp"

#include <bit>
#include <cassert>
#include <stdexcept>

namespace snes::core {
namespace {
int Bank(unsigned mode) {
    switch (mode & 31) {
    case 0x11:
        return 0;
    case 0x12:
        return 1;
    case 0x13:
        return 2;
    case 0x17:
        return 3;
    case 0x1b:
        return 4;
    default:
        return -1;
    }
}
} // namespace

void Arm6::Reset() {
    state_ = {};
    state_.status = 0x40;
    fetched_ = decoded_ = decodedAddress_ = 0;
    reload_ = true;
    sequential_ = false;
    carry_ = false;
    Exception(0x13, 0);
}

void Arm6::SetState(const State& state) {
    state_ = state;
    reload_ = true;
    sequential_ = false;
    fetched_ = decoded_ = decodedAddress_ = 0;
    carry_ = false;
}

uint32_t& Arm6::Register(unsigned index) {
    assert(index < 16);
    const int bank = Bank(state_.status);
    if (index >= 8 && index < 15 && bank == 0)
        return state_.fiq[index - 8];
    if (index >= 13 && index < 15 && bank > 0)
        return state_.bank[bank - 1][index - 13];
    return state_.common[index];
}

uint32_t& Arm6::SavedStatus() {
    const int bank = Bank(state_.status);
    if (bank < 0)
        throw std::runtime_error("Invalid ARM exception mode");
    return state_.saved[bank];
}

bool Arm6::ExceptionMode() const {
    const unsigned mode = state_.status & 31;
    return mode != 0x10 && mode != 0x1f;
}

void Arm6::Put(unsigned index, uint32_t value) {
    Register(index) = value;
    if (index == 15)
        reload_ = true;
}

void Arm6::Exception(unsigned mode, uint32_t vector) {
    const auto old = state_.status;
    state_.status = (old & ~31u) | mode;
    SavedStatus() = old;
    state_.status = (state_.status & ~32u) | 0x80 | (mode == 0x11 ? 0x40 : 0);
    Put(14, decodedAddress_);
    Put(15, vector);
}

void Arm6::Prefetch() {
    decodedAddress_ = state_.common[15] & ~3u;
    decoded_ = fetched_;
    state_.common[15] += 4;
    const auto access = Word | Fetch | (sequential_ ? unsigned(Sequential) : 0u);
    sequential_ = true;
    fetched_ = ReadArm(access, state_.common[15] & ~3u);
}

void Arm6::RunInstruction() {
    // This cartridge processor always enters the next instruction in ARM mode.
    state_.status &= ~32u;
    if (reload_) {
        reload_ = false;
        state_.common[15] &= ~3u;
        fetched_ = ReadArm(Word | Fetch, state_.common[15]);
        Prefetch();
    }
    const uint32_t instruction = decoded_;
    Prefetch();
    if (Condition(instruction >> 28))
        Execute(instruction);
}

void Arm6::Idle() {
    sequential_ = false;
    IdleArm();
}

bool Arm6::Condition(unsigned condition) const {
    const bool n = state_.status & N, z = state_.status & Z;
    const bool c = state_.status & C, v = state_.status & V;
    switch (condition) {
    case 0:
        return z;
    case 1:
        return !z;
    case 2:
        return c;
    case 3:
        return !c;
    case 4:
        return n;
    case 5:
        return !n;
    case 6:
        return v;
    case 7:
        return !v;
    case 8:
        return c && !z;
    case 9:
        return !c || z;
    case 10:
        return n == v;
    case 11:
        return n != v;
    case 12:
        return !z && n == v;
    case 13:
        return z || n != v;
    case 14:
        return true;
    default:
        return false;
    }
}

uint32_t Arm6::Shift(uint32_t value, unsigned type, unsigned count, bool immediate) {
    carry_ = state_.status & C;
    if (immediate && count == 0) {
        if (type == 3) {
            const uint32_t result = (uint32_t(carry_) << 31) | (value >> 1);
            carry_ = value & 1;
            return result;
        }
        if (type != 0)
            count = 32;
    }
    if (count == 0)
        return value;
    switch (type) {
    case 0:
        carry_ = count <= 32 && ((value >> (32 - count)) & 1);
        return count < 32 ? value << count : 0;
    case 1:
        carry_ = count <= 32 && ((value >> (count - 1)) & 1);
        return count < 32 ? value >> count : 0;
    case 2:
        carry_ = (value >> (count < 32 ? count - 1 : 31)) & 1;
        return count < 32 ? uint32_t(int32_t(value) >> count) : (value & N ? ~0u : 0);
    default:
        value = std::rotr(value, int(count & 31));
        carry_ = value & N;
        return value;
    }
}

void Arm6::ResultFlags(uint32_t value) {
    state_.status = (state_.status & ~(N | Z)) | (value & N) | (value == 0 ? Z : 0);
}

uint32_t Arm6::Add(uint32_t a, uint32_t b, bool carry, bool flags) {
    const uint64_t sum = uint64_t(a) + b + carry;
    const auto result = uint32_t(sum);
    if (flags) {
        ResultFlags(result);
        state_.status = (state_.status & ~(C | V)) | (sum >> 32 ? C : 0) |
                        ((~(a ^ b) & (a ^ result) & N) ? V : 0);
    }
    return result;
}

void Arm6::Data(uint32_t op) {
    const unsigned mode = (op >> 21) & 15, target = (op >> 12) & 15;
    uint32_t value;
    if (op & (1u << 25)) {
        carry_ = state_.status & C;
        value = op & 255;
        const unsigned rotation = ((op >> 8) & 15) * 2;
        if (rotation) {
            value = std::rotr(value, int(rotation));
            carry_ = value & N;
        }
    } else {
        const unsigned source = op & 15;
        const bool byRegister = op & 16;
        const unsigned countRegister = (op >> 8) & 15;
        const unsigned count =
            byRegister ? uint8_t(Register(countRegister) + (countRegister == 15 ? 4 : 0))
                       : (op >> 7) & 31;
        value = Shift(Register(source) + (byRegister && source == 15 ? 4 : 0), (op >> 5) & 3, count,
                      !byRegister);
    }
    const uint32_t a = Register((op >> 16) & 15);
    const bool flags = op & (1u << 20), oldCarry = state_.status & C;
    uint32_t result = 0;
    bool logical = false;
    switch (mode) {
    case 0:
    case 8:
        result = a & value;
        logical = true;
        break;
    case 1:
    case 9:
        result = a ^ value;
        logical = true;
        break;
    case 2:
    case 10:
        result = Add(a, ~value, true, flags);
        break;
    case 3:
        result = Add(value, ~a, true, flags);
        break;
    case 4:
    case 11:
        result = Add(a, value, false, flags);
        break;
    case 5:
        result = Add(a, value, oldCarry, flags);
        break;
    case 6:
        result = Add(a, ~value, oldCarry, flags);
        break;
    case 7:
        result = Add(value, ~a, oldCarry, flags);
        break;
    case 12:
        result = a | value;
        logical = true;
        break;
    case 13:
        result = value;
        logical = true;
        break;
    case 14:
        result = a & ~value;
        logical = true;
        break;
    case 15:
        result = ~value;
        logical = true;
        break;
    }
    if (logical && flags) {
        ResultFlags(result);
        state_.status = (state_.status & ~C) | (carry_ ? C : 0);
    }
    if (mode < 8 || mode > 11)
        Put(target, result);
    if (target == 15 && flags && ExceptionMode())
        state_.status = SavedStatus();
}

uint32_t Arm6::LoadValue(unsigned width, bool sign, uint32_t address) {
    sequential_ = false;
    uint32_t value = ReadArm(width | Load, address);
    unsigned rotation = (address & 3) * 8;
    if (width == Byte) {
        value = sign ? uint32_t(int32_t(int8_t(value))) : uint8_t(value);
        rotation = 0;
    } else if (width == Half) {
        value = sign ? uint32_t(int32_t(int16_t(value))) : uint16_t(value);
        rotation = (address & 1) * 8;
    }
    if (sign)
        value = uint32_t(int32_t(value) >> rotation);
    else
        value = std::rotr(value, int(rotation));
    Idle();
    return value;
}

void Arm6::StoreValue(unsigned width, uint32_t address, uint32_t value) {
    sequential_ = false;
    if (width == Byte)
        value = uint32_t(uint8_t(value)) * 0x01010101u;
    else if (width == Half)
        value = uint32_t(uint16_t(value)) * 0x00010001u;
    WriteArm(width | Store, address, value);
}

void Arm6::Transfer(uint32_t op, bool extended) {
    const unsigned base = (op >> 16) & 15, target = (op >> 12) & 15;
    const bool pre = op & (1u << 24), up = op & (1u << 23);
    const bool load = op & (1u << 20), writeback = op & (1u << 21);
    unsigned width = op & (1u << 22) ? Byte : Word;
    bool sign = false;
    uint32_t offset;
    if (extended) {
        offset = op & (1u << 22) ? ((op >> 4) & 0xf0) | (op & 15) : Register(op & 15);
        width = op & 32 ? Half : Byte;
        sign = op & 64;
    } else if (op & (1u << 25)) {
        offset = Shift(Register(op & 15), (op >> 5) & 3, (op >> 7) & 31, true);
    } else {
        offset = op & 0xfff;
    }
    const auto initial = Register(base);
    const auto updated = up ? initial + offset : initial - offset;
    const auto address = pre ? updated : initial;
    uint32_t value = Register(target);
    if (load)
        value = LoadValue(width, sign, address);
    else
        StoreValue(width, address, value);
    if (!pre || writeback)
        Put(base, updated);
    if (load)
        Put(target, value);
}

void Arm6::Multiple(uint32_t op) {
    const unsigned base = (op >> 16) & 15, count = std::popcount(op & 0xffff);
    const bool load = op & (1u << 20), up = op & (1u << 23);
    const bool pre = op & (1u << 24), writeback = op & (1u << 21), user = op & (1u << 22);
    const uint32_t delta = count * 4;
    const uint32_t initial = Register(base);
    uint32_t address = up ? initial + (pre ? 4 : 0) : initial - delta + (pre ? 0 : 4);
    if (writeback && load)
        Put(base, up ? initial + delta : initial - delta);
    const unsigned oldMode = state_.status & 31;
    const bool userBank = user && (!load || !(op & 0x8000));
    if (userBank)
        state_.status = (state_.status & ~31u) | 0x10;
    bool sequential = false;
    for (unsigned index = 0; index < 16; ++index) {
        if (!(op & (1u << index)))
            continue;
        const unsigned access = Word | (sequential ? unsigned(Sequential) : 0u);
        if (load)
            Put(index, ReadArm(access, address));
        else {
            sequential_ = false;
            WriteArm(access, address, Register(index));
        }
        address += 4;
        sequential = true;
    }
    if (userBank)
        state_.status = (state_.status & ~31u) | oldMode;
    if (load) {
        Idle();
        if (user && (op & 0x8000) && ExceptionMode())
            state_.status = SavedStatus();
    } else {
        sequential_ = false;
    }
    if (writeback && !load)
        Put(base, up ? Register(base) + delta : Register(base) - delta);
}

void Arm6::Multiply(uint32_t op, bool wide) {
    const auto a = Register(op & 15), b = Register((op >> 8) & 15);
    const unsigned high = (op >> 16) & 15, low = (op >> 12) & 15;
    const bool accumulate = op & (1u << 21), sign = !wide || (op & (1u << 22));
    Idle();
    if (wide)
        Idle();
    if (accumulate)
        Idle();
    for (unsigned bits = 8; bits < 32; bits += 8) {
        const auto upper = b >> bits;
        if (upper != 0 && (!sign || upper != (~0u >> bits)))
            Idle();
    }
    uint64_t result =
        wide && sign ? uint64_t(int64_t(int32_t(a)) * int64_t(int32_t(b))) : uint64_t(a) * b;
    if (accumulate)
        result += wide ? (uint64_t(Register(high)) << 32) | Register(low) : Register(low);
    if (wide) {
        Put(high, uint32_t(result >> 32));
        Put(low, uint32_t(result));
    } else {
        Put(high, uint32_t(result));
    }
    if (op & (1u << 20)) {
        if (!wide)
            result = uint32_t(result);
        state_.status = (state_.status & ~(N | Z)) | (result == 0 ? Z : 0) |
                        ((result >> (wide ? 63 : 31)) & 1 ? N : 0);
    }
}

void Arm6::Status(uint32_t op, bool read) {
    const bool saved = op & (1u << 22);
    if (saved && !ExceptionMode())
        return;
    if (read) {
        Put((op >> 12) & 15, saved ? SavedStatus() : state_.status);
        return;
    }
    uint32_t value =
        op & (1u << 25) ? std::rotr(op & 255, int(((op >> 8) & 15) * 2)) : Register(op & 15);
    uint32_t mask = 0;
    if ((op & (1u << 16)) && (saved || (state_.status & 31) != 0x10))
        mask |= 255;
    if (op & (1u << 19))
        mask |= N | Z | C | V;
    auto& status = saved ? SavedStatus() : state_.status;
    status = (status & ~mask) | (value & mask);
    if (!saved && (mask & 255) && (status & 32))
        state_.common[15] += 2;
}

void Arm6::Execute(uint32_t op) {
    const uint32_t shape = op & 0x0ff000f0;
    const unsigned operation = (op >> 21) & 15;
    if ((op & 0x0e000000) == 0x0a000000) {
        if (op & (1u << 24))
            Put(14, state_.common[15] - 4);
        const int32_t offset = int32_t(op << 8) >> 6;
        Put(15, state_.common[15] + uint32_t(offset));
    } else if ((op & 0x0f000000) == 0x0f000000) {
        Exception(0x13, 8);
    } else if (shape == 0x01200010) {
        const uint32_t target = Register(op & 15);
        state_.status = (state_.status & ~32u) | ((target & 1) << 5);
        Put(15, target);
    } else if ((shape & 0x0fb000f0) == 0x01000000) {
        Status(op, true);
    } else if ((shape & 0x0fb000f0) == 0x01200000 || (shape & 0x0fb00000) == 0x03200000) {
        Status(op, false);
    } else if ((shape & 0x0fc000f0) == 0x00000090) {
        Multiply(op, false);
    } else if ((shape & 0x0f8000f0) == 0x00800090) {
        Multiply(op, true);
    } else if ((shape & 0x0fb000f0) == 0x01000090) {
        const unsigned width = op & (1u << 22) ? Byte : Word;
        const uint32_t value = LoadValue(width, false, Register((op >> 16) & 15));
        StoreValue(width, Register((op >> 16) & 15), Register(op & 15));
        Put((op >> 12) & 15, value);
    } else if ((shape & 0x0e0000f0) == 0x000000b0 || (shape & 0x0e1000d0) == 0x001000d0) {
        Transfer(op, true);
    } else if ((op & 0x0e000000) == 0x08000000) {
        Multiple(op);
    } else if ((op & 0x0e000000) == 0x04000000 || (op & 0x0e000010) == 0x06000000) {
        Transfer(op, false);
    } else if ((op & 0x0c000000) == 0 && (!(op & 16) || (op & (1u << 25)) || !(op & 128)) &&
               (operation < 8 || operation > 11 || (op & (1u << 20)))) {
        Data(op);
    } else {
        Exception(0x1b, 4);
    }
}

} // namespace snes::core
