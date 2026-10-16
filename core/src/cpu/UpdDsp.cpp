#include "snes/core/UpdDsp.hpp"

#include <bit>

namespace snes::core {
namespace {
constexpr uint16_t Request = 0x8000, ByteMode = 0x0400, HighByte = 0x1000;
int16_t Signed(uint16_t value) {
  return std::bit_cast<int16_t>(value);
}
} // namespace

UpdDsp::UpdDsp(Revision revision)
    : programMask_(revision == Revision::Upd7725 ? 0x7ff : 0x3fff),
      romMask_(revision == Revision::Upd7725 ? 0x3ff : 0x7ff),
      ramMask_(revision == Revision::Upd7725 ? 0xff : 0x7ff) {}

size_t UpdDsp::FirmwareSize() const noexcept {
  return (size_t(programMask_) + 1) * 3 + (size_t(romMask_) + 1) * 2;
}

bool UpdDsp::LoadFirmware(std::span<const uint8_t> bytes) {
  if (bytes.size() != FirmwareSize())
    return false;
  size_t offset = 0;
  for (unsigned i = 0; i <= programMask_; ++i, offset += 3)
    program_[i] = uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
                  (uint32_t(bytes[offset + 2]) << 16);
  for (unsigned i = 0; i <= romMask_; ++i, offset += 2)
    dataRom_[i] = uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8);
  ram_.fill(0);
  Reset();
  return true;
}

void UpdDsp::Reset() {
  state_ = {};
  cycles_ = 0;
}

uint16_t UpdDsp::EffectiveStatus() const noexcept {
  return (state_.status & ByteMode) ? state_.status & ~HighByte : state_.status;
}

uint8_t UpdDsp::ReadStatus() const noexcept {
  return static_cast<uint8_t>(EffectiveStatus() >> 8);
}

uint8_t UpdDsp::ReadData() {
  const bool high = !(state_.status & ByteMode) && (state_.status & HighByte);
  const uint8_t value = static_cast<uint8_t>(state_.data >> (high ? 8 : 0));
  if ((state_.status & ByteMode) || high)
    state_.status &= ~Request;
  if (!(state_.status & ByteMode))
    state_.status ^= HighByte;
  return value;
}

void UpdDsp::WriteData(uint8_t value) {
  const bool high = !(state_.status & ByteMode) && (state_.status & HighByte);
  if (high)
    state_.data = (state_.data & 0x00ff) | (uint16_t(value) << 8);
  else
    state_.data = (state_.data & 0xff00) | value;
  if ((state_.status & ByteMode) || high)
    state_.status &= ~Request;
  if (!(state_.status & ByteMode))
    state_.status ^= HighByte;
}

uint8_t UpdDsp::ReadRam(uint16_t byteAddress) const noexcept {
  return static_cast<uint8_t>(ram_[(byteAddress >> 1) & 0x7ff] >> ((byteAddress & 1) * 8));
}

void UpdDsp::WriteRam(uint16_t byteAddress, uint8_t value) noexcept {
  auto &word = ram_[(byteAddress >> 1) & 0x7ff];
  if (byteAddress & 1)
    word = (word & 0x00ff) | (uint16_t(value) << 8);
  else
    word = (word & 0xff00) | value;
}

uint16_t UpdDsp::ReadOperand(unsigned operand) {
  switch (operand) {
  case 0:
    return state_.temporaryBuffer;
  case 1:
    return state_.accumulator[0];
  case 2:
    return state_.accumulator[1];
  case 3:
    return state_.temporary;
  case 4:
    return state_.ramPointer;
  case 5:
    return state_.romPointer;
  case 6:
    return dataRom_[state_.romPointer];
  case 7:
    return 0x8000 - unsigned(bool(state_.flags[0] & ExtendedSign));
  case 8:
    state_.status |= Request;
    return state_.data;
  case 9:
    return state_.data;
  case 10:
    return EffectiveStatus();
  case 11:
  case 12:
    return state_.serialInput;
  case 13:
    return static_cast<uint16_t>(state_.factorX);
  case 14:
    return static_cast<uint16_t>(state_.factorY);
  default:
    return ram_[state_.ramPointer];
  }
}

void UpdDsp::StoreOperand(unsigned operand, uint16_t value) {
  switch (operand) {
  case 0:
    break;
  case 1:
    state_.accumulator[0] = value;
    break;
  case 2:
    state_.accumulator[1] = value;
    break;
  case 3:
    state_.temporary = value;
    break;
  case 4:
    state_.ramPointer = value & ramMask_;
    break;
  case 5:
    state_.romPointer = value & romMask_;
    break;
  case 6:
    state_.data = value;
    state_.status |= Request;
    break;
  case 7:
    state_.status = ((EffectiveStatus() & 0x907c) | (value & ~0x907c)) & 0xff83;
    break;
  case 8:
  case 9:
    state_.serialOutput = value;
    break;
  case 10:
    state_.factorX = Signed(value);
    break;
  case 11:
    state_.factorX = Signed(value);
    state_.factorY = Signed(dataRom_[state_.romPointer]);
    break;
  case 12:
    state_.factorY = Signed(value);
    state_.factorX = Signed(ram_[state_.ramPointer | 0x40]);
    break;
  case 13:
    state_.factorY = Signed(value);
    break;
  case 14:
    state_.temporaryBuffer = value;
    break;
  case 15:
    ram_[state_.ramPointer] = value;
    break;
  }
}

void UpdDsp::Arithmetic(unsigned operation, unsigned accumulator, uint16_t operand) {
  const uint16_t before = state_.accumulator[accumulator];
  const auto previous = state_.flags[accumulator];
  const bool inputCarry = state_.flags[accumulator ^ 1] & Carry;
  uint16_t result = before;
  switch (operation) {
  case 1:
    result |= operand;
    break;
  case 2:
    result &= operand;
    break;
  case 3:
    result ^= operand;
    break;
  case 4:
    result -= operand;
    break;
  case 5:
    result += operand;
    break;
  case 6:
    result -= operand + unsigned(inputCarry);
    break;
  case 7:
    result += operand + unsigned(inputCarry);
    break;
  case 8:
    operand = 1;
    --result;
    break;
  case 9:
    operand = 1;
    ++result;
    break;
  case 10:
    result = ~before;
    break;
  case 11:
    result = (before >> 1) | (before & 0x8000);
    break;
  case 12:
    result = (before << 1) | unsigned(inputCarry);
    break;
  case 13:
    result = (before << 2) | 3;
    break;
  case 14:
    result = (before << 4) | 15;
    break;
  case 15:
    result = (before << 8) | (before >> 8);
    break;
  }

  const bool negative = result & 0x8000;
  const bool extended = (previous & StickyOverflow) ? bool(previous & ExtendedSign) : negative;
  bool carry = false, overflow = false, sticky = false;
  if (operation >= 4 && operation <= 9) {
    const bool addition = operation & 1;
    const bool sameSign = !((before ^ operand) & 0x8000);
    overflow = bool((before ^ result) & 0x8000) && (addition == sameSign);
    carry = addition ? result < before : result > before;
    const bool hadOverflow = previous & StickyOverflow;
    sticky = overflow && hadOverflow ? negative == extended : overflow || hadOverflow;
  } else if (operation == 11) {
    carry = before & 1;
  } else if (operation == 12) {
    carry = before & 0x8000;
  }
  state_.accumulator[accumulator] = result;
  state_.flags[accumulator] = uint8_t(overflow) | (uint8_t(sticky) << 1) |
                              (uint8_t(result == 0) << 2) | (uint8_t(carry) << 3) |
                              (uint8_t(negative) << 4) | (uint8_t(extended) << 5);
}

void UpdDsp::Parallel(uint32_t instruction) {
  const unsigned source = (instruction >> 4) & 15, destination = instruction & 15;
  const uint16_t moved = ReadOperand(source);
  const unsigned operation = (instruction >> 16) & 15;
  if (operation) {
    const std::array<uint16_t, 4> operands{ram_[state_.ramPointer], moved, state_.productHigh,
                                           state_.productLow};
    Arithmetic(operation, (instruction >> 15) & 1, operands[(instruction >> 20) & 3]);
  }
  StoreOperand(destination, moved);
  if (destination != 4) {
    const unsigned modify = (instruction >> 13) & 3;
    if (modify) {
      const unsigned low = modify == 3 ? 0 : (state_.ramPointer + (modify == 1 ? 1 : -1)) & 15;
      state_.ramPointer = (state_.ramPointer & 0xf0) | low;
    }
    state_.ramPointer ^= ((instruction >> 9) & 15) << 4;
  }
  if (destination != 5 && (instruction & 0x100))
    state_.romPointer = (state_.romPointer - 1) & romMask_;
}

bool UpdDsp::Condition(unsigned condition) const noexcept {
  if (condition >= 0x80 && condition < 0xb0 && !(condition & 1)) {
    constexpr std::array<uint8_t, 6> selectedFlag{Carry,          Zero, Overflow,
                                                  StickyOverflow, Sign, ExtendedSign};
    const auto flags = state_.flags[(condition >> 2) & 1];
    return bool(flags & selectedFlag[(condition - 0x80) >> 3]) == bool(condition & 2);
  }
  if (condition >= 0xb0 && condition <= 0xb3) {
    const bool match = (state_.ramPointer & 15) == ((condition & 2) ? 15 : 0);
    return match != bool(condition & 1);
  }
  switch (condition) {
  case 0xb4:
    return !state_.inputAcknowledge;
  case 0xb6:
    return state_.inputAcknowledge;
  case 0xb8:
    return !state_.outputAcknowledge;
  case 0xba:
    return state_.outputAcknowledge;
  case 0xbc:
    return !(state_.status & Request);
  case 0xbe:
    return state_.status & Request;
  default:
    return false;
  }
}

void UpdDsp::Branch(uint32_t instruction) {
  const unsigned condition = (instruction >> 13) & 0x1ff;
  const uint16_t target =
      (state_.pc & 0x2000) | ((instruction & 3) << 11) | ((instruction >> 2) & 0x7ff);
  if (condition == 0) {
    state_.pc = state_.serialOutput & programMask_;
  } else if (condition == 0x100 || condition == 0x101 || condition == 0x140 || condition == 0x141) {
    if (condition & 0x40) {
      state_.stack[state_.stackPointer] = state_.pc;
      state_.stackPointer = (state_.stackPointer + 1) & 15;
    }
    state_.pc = ((target & ~0x2000) | ((condition & 1) << 13)) & programMask_;
  } else if (Condition(condition)) {
    state_.pc = target & programMask_;
  }
}

void UpdDsp::Step() {
  const auto instruction = program_[state_.pc] & 0xffffff;
  state_.pc = (state_.pc + 1) & programMask_;
  const unsigned kind = instruction >> 22;
  if (kind < 2) {
    Parallel(instruction);
    if (kind == 1) {
      state_.stackPointer = (state_.stackPointer - 1) & 15;
      state_.pc = state_.stack[state_.stackPointer] & programMask_;
    }
  } else if (kind == 2) {
    Branch(instruction);
  } else {
    StoreOperand(instruction & 15, static_cast<uint16_t>(instruction >> 6));
  }
  const int32_t product = int32_t(state_.factorX) * state_.factorY;
  state_.productHigh = static_cast<uint16_t>(product >> 15);
  state_.productLow = static_cast<uint16_t>(uint32_t(product) << 1);
  ++cycles_;
}

} // namespace snes::core
