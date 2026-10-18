// SM83 instructions, machine cycles, and interrupt entry.
#include "snes/core/gameboy/Cpu.hpp"

#include <bit>

namespace snes::core::gameboy {

void Cpu::Reset() noexcept {
  state_ = {};
}

void Cpu::SetState(const State &state) noexcept {
  state_ = state;
  state_.f &= 0xf0;
}

uint8_t Cpu::ReadByte(uint16_t address) {
  const auto value = bus_.Read(address);
  bus_.Advance(4);
  return value;
}

void Cpu::WriteByte(uint16_t address, uint8_t value) {
  bus_.Write(address, value);
  bus_.Advance(4);
}

uint8_t Cpu::Fetch() {
  const auto address = state_.pc;
  ++state_.pc;
  return ReadByte(address);
}

uint16_t Cpu::FetchWord() {
  const uint16_t low = Fetch();
  return static_cast<uint16_t>(low | (uint16_t(Fetch()) << 8));
}

void Cpu::Idle() {
  bus_.Advance(4);
}

void Cpu::IdleAt(uint16_t address) {
  bus_.DriveAddress(address);
  Idle();
}

uint16_t Cpu::Pair(unsigned pair) const {
  switch (pair & 3) {
  case 0:
    return uint16_t(state_.b) << 8 | state_.c;
  case 1:
    return uint16_t(state_.d) << 8 | state_.e;
  case 2:
    return uint16_t(state_.h) << 8 | state_.l;
  default:
    return state_.sp;
  }
}

void Cpu::SetPair(unsigned pair, uint16_t value) {
  switch (pair & 3) {
  case 0:
    state_.b = uint8_t(value >> 8);
    state_.c = uint8_t(value);
    break;
  case 1:
    state_.d = uint8_t(value >> 8);
    state_.e = uint8_t(value);
    break;
  case 2:
    state_.h = uint8_t(value >> 8);
    state_.l = uint8_t(value);
    break;
  default:
    state_.sp = value;
    break;
  }
}

uint16_t Cpu::StackPair(unsigned pair) const {
  return pair == 3 ? uint16_t(state_.a) << 8 | state_.f : Pair(pair);
}

void Cpu::SetStackPair(unsigned pair, uint16_t value) {
  if (pair == 3) {
    state_.a = uint8_t(value >> 8);
    state_.f = uint8_t(value & 0xf0);
  } else {
    SetPair(pair, value);
  }
}

uint8_t Cpu::Register(unsigned index) {
  switch (index & 7) {
  case 0:
    return state_.b;
  case 1:
    return state_.c;
  case 2:
    return state_.d;
  case 3:
    return state_.e;
  case 4:
    return state_.h;
  case 5:
    return state_.l;
  case 6:
    return ReadByte(Pair(2));
  default:
    return state_.a;
  }
}

void Cpu::SetRegister(unsigned index, uint8_t value) {
  switch (index & 7) {
  case 0:
    state_.b = value;
    break;
  case 1:
    state_.c = value;
    break;
  case 2:
    state_.d = value;
    break;
  case 3:
    state_.e = value;
    break;
  case 4:
    state_.h = value;
    break;
  case 5:
    state_.l = value;
    break;
  case 6:
    WriteByte(Pair(2), value);
    break;
  default:
    state_.a = value;
    break;
  }
}

bool Cpu::Condition(unsigned condition) const {
  const bool flag = state_.f & (condition < 2 ? Zero : Carry);
  return flag == bool(condition & 1);
}

uint16_t Cpu::Pop() {
  const uint16_t low = ReadByte(state_.sp++);
  return static_cast<uint16_t>(low | uint16_t(ReadByte(state_.sp++)) << 8);
}

void Cpu::Push(uint16_t value) {
  WriteByte(--state_.sp, uint8_t(value >> 8));
  WriteByte(--state_.sp, uint8_t(value));
}

void Cpu::JumpRelative() {
  const unsigned bits = Fetch();
  const int displacement = bits < 128 ? int(bits) : int(bits) - 256;
  Idle();
  state_.pc = static_cast<uint16_t>(int(state_.pc) + displacement);
}

void Cpu::AddHl(uint16_t value) {
  const uint16_t before = Pair(2);
  const uint32_t result = uint32_t(before) + value;
  Idle();
  state_.f = uint8_t((state_.f & Zero) | (((before & 0xfff) + (value & 0xfff) > 0xfff) ? Half : 0) |
                     ((result > 0xffff) ? Carry : 0));
  SetPair(2, uint16_t(result));
}

void Cpu::AddStackOffset(bool toHl) {
  const unsigned bits = Fetch();
  const int displacement = bits < 128 ? int(bits) : int(bits) - 256;
  const uint16_t before = state_.sp;
  const auto result = static_cast<uint16_t>(int(before) + displacement);
  Idle();
  state_.f = uint8_t((((before & 15) + (bits & 15) > 15) ? Half : 0) |
                     (((before & 255) + bits > 255) ? Carry : 0));
  if (toHl) {
    SetPair(2, result);
  } else {
    Idle();
    state_.sp = result;
  }
}

uint8_t Cpu::Increment(uint8_t value) {
  const auto result = uint8_t(value + 1);
  state_.f =
      uint8_t((state_.f & Carry) | (result == 0 ? Zero : 0) | ((value & 15) == 15 ? Half : 0));
  return result;
}

uint8_t Cpu::Decrement(uint8_t value) {
  const auto result = uint8_t(value - 1);
  state_.f = uint8_t((state_.f & Carry) | Subtract | (result == 0 ? Zero : 0) |
                     ((value & 15) == 0 ? Half : 0));
  return result;
}

void Cpu::Arithmetic(unsigned operation, uint8_t value) {
  const unsigned before = state_.a;
  const unsigned carry = ((operation == 1 || operation == 3) && (state_.f & Carry)) ? 1 : 0;
  if (operation < 2) {
    const unsigned result = before + value + carry;
    state_.a = uint8_t(result);
    state_.f = uint8_t((state_.a == 0 ? Zero : 0) |
                       (((before & 15) + (value & 15) + carry > 15) ? Half : 0) |
                       ((result > 255) ? Carry : 0));
  } else if (operation < 4 || operation == 7) {
    const int result = int(before) - int(value) - int(carry);
    state_.f =
        uint8_t(Subtract | (uint8_t(result) == 0 ? Zero : 0) |
                (((before & 15) < (value & 15) + carry) ? Half : 0) | ((result < 0) ? Carry : 0));
    if (operation != 7)
      state_.a = uint8_t(result);
  } else {
    if (operation == 4)
      state_.a &= value;
    if (operation == 5)
      state_.a ^= value;
    if (operation == 6)
      state_.a |= value;
    state_.f = uint8_t((state_.a == 0 ? Zero : 0) | (operation == 4 ? Half : 0));
  }
}

void Cpu::DecimalAdjust() {
  unsigned adjustment = 0;
  bool carry = state_.f & Carry;
  if (!(state_.f & Subtract)) {
    if ((state_.f & Half) || (state_.a & 15) > 9)
      adjustment |= 6;
    if (carry || state_.a > 0x99) {
      adjustment |= 0x60;
      carry = true;
    }
    state_.a = uint8_t(state_.a + adjustment);
  } else {
    if (state_.f & Half)
      adjustment |= 6;
    if (carry)
      adjustment |= 0x60;
    state_.a = uint8_t(state_.a - adjustment);
  }
  state_.f = uint8_t((state_.f & Subtract) | (state_.a == 0 ? Zero : 0) | (carry ? Carry : 0));
}

void Cpu::AccumulatorRotate(unsigned operation) {
  const uint8_t value = state_.a;
  const unsigned previousCarry = (state_.f & Carry) ? 1 : 0;
  const bool right = operation & 1;
  const unsigned carry = right ? value & 1 : value >> 7;
  const unsigned input = operation < 2 ? carry : previousCarry;
  state_.a = right ? uint8_t((value >> 1) | (input << 7)) : uint8_t((unsigned(value) << 1) | input);
  state_.f = carry ? Carry : 0;
}

void Cpu::Extended(uint8_t opcode) {
  const unsigned group = opcode >> 6, operation = opcode >> 3 & 7, index = opcode & 7;
  const uint8_t value = Register(index);
  if (group == 1) {
    state_.f = uint8_t((state_.f & Carry) | Half | ((value & (1u << operation)) == 0 ? Zero : 0));
    return;
  }
  uint8_t result = value;
  if (group == 2)
    result = uint8_t(value & ~(1u << operation));
  if (group == 3)
    result = uint8_t(value | (1u << operation));
  if (group == 0) {
    const unsigned previousCarry = (state_.f & Carry) ? 1 : 0;
    unsigned carry = 0;
    switch (operation) {
    case 0:
      carry = value >> 7;
      result = uint8_t((value << 1) | carry);
      break;
    case 1:
      carry = value & 1;
      result = uint8_t((value >> 1) | (carry << 7));
      break;
    case 2:
      carry = value >> 7;
      result = uint8_t((value << 1) | previousCarry);
      break;
    case 3:
      carry = value & 1;
      result = uint8_t((value >> 1) | (previousCarry << 7));
      break;
    case 4:
      carry = value >> 7;
      result = uint8_t(value << 1);
      break;
    case 5:
      carry = value & 1;
      result = uint8_t((value >> 1) | (value & 0x80));
      break;
    case 6:
      result = uint8_t((value << 4) | (value >> 4));
      break;
    case 7:
      carry = value & 1;
      result = uint8_t(value >> 1);
      break;
    }
    state_.f = uint8_t((result == 0 ? Zero : 0) | (carry ? Carry : 0));
  }
  SetRegister(index, result);
}

void Cpu::Interrupt() {
  const uint16_t returnAddress = state_.pc;
  ReadByte(returnAddress);
  IdleAt(uint16_t(returnAddress + 1));
  IdleAt(state_.sp);
  WriteByte(--state_.sp, uint8_t(returnAddress >> 8));
  const uint8_t enabled = bus_.InterruptEnable();
  const uint8_t previousFlags = bus_.InterruptFlags();
  const uint16_t address = --state_.sp;
  bus_.Write(address, uint8_t(returnAddress));
  const uint8_t flags = address == 0xff0f ? previousFlags : bus_.InterruptFlags();
  const uint8_t pending = enabled & flags & 31;
  bus_.Advance(2);
  if (pending) {
    const unsigned bit = std::countr_zero(unsigned(pending));
    bus_.AcknowledgeInterrupt(uint8_t(1u << bit));
    state_.pc = uint16_t(0x40 + bit * 8);
  } else {
    state_.pc = 0;
  }
  bus_.Advance(2);
  state_.ime = false;
  state_.enablePending = false;
  state_.halted = false;
}

void Cpu::Execute(uint8_t opcode) {
  const unsigned group = opcode >> 6, y = opcode >> 3 & 7, z = opcode & 7;
  const unsigned pair = y >> 1;
  const bool odd = y & 1;
  if (group == 1) {
    if (opcode == 0x76) {
      bus_.Read(state_.pc);
      if (bus_.InterruptEnable() & bus_.InterruptFlags() & 31) {
        if (state_.ime)
          --state_.pc;
        else
          state_.haltBug = true;
      } else {
        state_.halted = true;
      }
      state_.firstHaltWait = true;
    } else {
      const auto value = Register(z);
      SetRegister(y, value);
    }
    return;
  }
  if (group == 2) {
    Arithmetic(y, Register(z));
    return;
  }
  if (group == 0) {
    switch (z) {
    case 0:
      if (y == 0)
        return;
      if (y == 1) {
        const auto address = FetchWord();
        WriteByte(address, uint8_t(state_.sp));
        WriteByte(uint16_t(address + 1), uint8_t(state_.sp >> 8));
      } else if (y == 2) {
        bus_.Read(state_.pc);
        const bool pressed = (bus_.Read(0xff00) & 15) != 15;
        const bool pending = bus_.InterruptEnable() & bus_.InterruptFlags() & 31;
        state_.stopped = !pressed;
        if (state_.stopped)
          bus_.SetStopped(true);
        if (!pending)
          Fetch();
        if (pressed) {
          bus_.SetStopped(false);
          if (!pending)
            state_.halted = state_.firstHaltWait = true;
        }
      } else if (y == 3) {
        JumpRelative();
      } else if (Condition(y - 4)) {
        JumpRelative();
      } else {
        Fetch();
      }
      return;
    case 1:
      if (odd)
        AddHl(Pair(pair));
      else
        SetPair(pair, FetchWord());
      return;
    case 2: {
      const uint16_t address = Pair(pair < 2 ? pair : 2);
      if (odd)
        state_.a = ReadByte(address);
      else
        WriteByte(address, state_.a);
      if (pair >= 2) {
        bus_.AddressIncremented(address);
        SetPair(2, uint16_t(address + (pair == 2 ? 1 : -1)));
      }
      return;
    }
    case 3: {
      const uint16_t value = Pair(pair);
      IdleAt(value);
      SetPair(pair, uint16_t(value + (odd ? -1 : 1)));
      return;
    }
    case 4: {
      const auto value = Register(y);
      SetRegister(y, Increment(value));
      return;
    }
    case 5: {
      const auto value = Register(y);
      SetRegister(y, Decrement(value));
      return;
    }
    case 6:
      SetRegister(y, Fetch());
      return;
    case 7:
      if (y < 4)
        AccumulatorRotate(y);
      if (y == 4)
        DecimalAdjust();
      if (y == 5) {
        state_.a ^= 0xff;
        state_.f |= Subtract | Half;
      }
      if (y == 6)
        state_.f = uint8_t((state_.f & Zero) | Carry);
      if (y == 7)
        state_.f = uint8_t((state_.f & Zero) | ((state_.f & Carry) ? 0 : Carry));
      return;
    }
  }
  switch (z) {
  case 0:
    if (y < 4) {
      Idle();
      if (Condition(y)) {
        const auto target = Pop();
        Idle();
        state_.pc = target;
      }
    } else if (y == 4) {
      const auto offset = Fetch();
      WriteByte(uint16_t(0xff00 | offset), state_.a);
    } else if (y == 5) {
      AddStackOffset(false);
    } else if (y == 6) {
      const auto offset = Fetch();
      state_.a = ReadByte(uint16_t(0xff00 | offset));
    } else {
      AddStackOffset(true);
    }
    return;
  case 1:
    if (!odd)
      SetStackPair(pair, Pop());
    else if (pair < 2) {
      const auto target = Pop();
      Idle();
      state_.pc = target;
      if (pair == 1) {
        state_.ime = true;
        state_.enablePending = false;
      }
    } else if (pair == 2) {
      state_.pc = Pair(2);
    } else {
      Idle();
      state_.sp = Pair(2);
    }
    return;
  case 2:
    if (y < 4) {
      const auto target = FetchWord();
      if (Condition(y)) {
        Idle();
        state_.pc = target;
      }
    } else if (y == 4) {
      WriteByte(uint16_t(0xff00 | state_.c), state_.a);
    } else if (y == 5) {
      const auto address = FetchWord();
      WriteByte(address, state_.a);
    } else if (y == 6) {
      state_.a = ReadByte(uint16_t(0xff00 | state_.c));
    } else {
      const auto address = FetchWord();
      state_.a = ReadByte(address);
    }
    return;
  case 3:
    if (y == 0) {
      const auto target = FetchWord();
      Idle();
      state_.pc = target;
      return;
    }
    if (y == 1) {
      Extended(Fetch());
      return;
    }
    if (y == 6) {
      state_.ime = state_.enablePending = false;
      return;
    }
    if (y == 7) {
      if (!state_.ime)
        state_.enablePending = true;
      return;
    }
    break;
  case 4:
    if (y < 4) {
      const auto target = FetchWord();
      if (Condition(y)) {
        IdleAt(state_.sp);
        Push(state_.pc);
        state_.pc = target;
      }
      return;
    }
    break;
  case 5:
    if (!odd) {
      IdleAt(state_.sp);
      Push(StackPair(pair));
      return;
    }
    if (pair == 0) {
      const auto target = FetchWord();
      IdleAt(state_.sp);
      Push(state_.pc);
      state_.pc = target;
      return;
    }
    break;
  case 6:
    Arithmetic(y, Fetch());
    return;
  case 7:
    IdleAt(state_.sp);
    Push(state_.pc);
    state_.pc = uint16_t(y * 8);
    return;
  }
  bus_.Write(0xffff, 0);
  state_.halted = true;
}

void Cpu::RunInstruction() {
  if (state_.stopped) {
    Idle();
    if ((bus_.Read(0xff00) & 15) != 15) {
      state_.stopped = false;
      bus_.SetStopped(false);
      bus_.Advance(8);
    }
    return;
  }
  if (state_.halted && !state_.firstHaltWait)
    bus_.Advance(2);
  const uint8_t pending = bus_.InterruptEnable() & bus_.InterruptFlags() & 31;
  if (state_.halted)
    bus_.Advance(state_.firstHaltWait ? 4 : 2);
  state_.firstHaltWait = false;
  const bool effectiveIme = state_.ime;
  if (state_.enablePending) {
    state_.ime = !state_.ime;
    state_.enablePending = false;
  }
  if (state_.halted && !effectiveIme && pending) {
    state_.halted = false;
    return;
  }
  if (effectiveIme && pending) {
    Interrupt();
    return;
  }
  if (state_.halted)
    return;
  const auto opcode = Fetch();
  if (state_.haltBug) {
    --state_.pc;
    state_.haltBug = false;
  }
  Execute(opcode);
  state_.f &= 0xf0;
}

} // namespace snes::core::gameboy
