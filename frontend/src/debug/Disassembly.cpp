#include "Disassembly.hpp"
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace snes::frontend {
namespace {
enum Mode {
  Imp,
  Acc,
  Imm8,
  ImmM,
  ImmX,
  Dp,
  DpX,
  DpY,
  Ind,
  IndX,
  IndY,
  LongInd,
  LongIndY,
  Stack,
  StackY,
  Abs,
  AbsX,
  AbsY,
  Long,
  LongX,
  AbsInd,
  AbsIndX,
  AbsLongInd,
  Rel8,
  Rel16,
  Move
};
struct Op {
  std::string_view name;
  Mode mode;
};
constexpr std::array<Op, 256> ops{
    {{"BRK", Imm8},       {"ORA", IndX}, {"COP", Imm8},  {"ORA", Stack},
     {"TSB", Dp},         {"ORA", Dp},   {"ASL", Dp},    {"ORA", LongInd},
     {"PHP", Imp},        {"ORA", ImmM}, {"ASL", Acc},   {"PHD", Imp},
     {"TSB", Abs},        {"ORA", Abs},  {"ASL", Abs},   {"ORA", Long},
     {"BPL", Rel8},       {"ORA", IndY}, {"ORA", Ind},   {"ORA", StackY},
     {"TRB", Dp},         {"ORA", DpX},  {"ASL", DpX},   {"ORA", LongIndY},
     {"CLC", Imp},        {"ORA", AbsY}, {"INC", Acc},   {"TCS", Imp},
     {"TRB", Abs},        {"ORA", AbsX}, {"ASL", AbsX},  {"ORA", LongX},
     {"JSR", Abs},        {"AND", IndX}, {"JSL", Long},  {"AND", Stack},
     {"BIT", Dp},         {"AND", Dp},   {"ROL", Dp},    {"AND", LongInd},
     {"PLP", Imp},        {"AND", ImmM}, {"ROL", Acc},   {"PLD", Imp},
     {"BIT", Abs},        {"AND", Abs},  {"ROL", Abs},   {"AND", Long},
     {"BMI", Rel8},       {"AND", IndY}, {"AND", Ind},   {"AND", StackY},
     {"BIT", DpX},        {"AND", DpX},  {"ROL", DpX},   {"AND", LongIndY},
     {"SEC", Imp},        {"AND", AbsY}, {"DEC", Acc},   {"TSC", Imp},
     {"BIT", AbsX},       {"AND", AbsX}, {"ROL", AbsX},  {"AND", LongX},
     {"RTI", Imp},        {"EOR", IndX}, {"WDM", Imm8},  {"EOR", Stack},
     {"MVP", Move},       {"EOR", Dp},   {"LSR", Dp},    {"EOR", LongInd},
     {"PHA", Imp},        {"EOR", ImmM}, {"LSR", Acc},   {"PHK", Imp},
     {"JMP", Abs},        {"EOR", Abs},  {"LSR", Abs},   {"EOR", Long},
     {"BVC", Rel8},       {"EOR", IndY}, {"EOR", Ind},   {"EOR", StackY},
     {"MVN", Move},       {"EOR", DpX},  {"LSR", DpX},   {"EOR", LongIndY},
     {"CLI", Imp},        {"EOR", AbsY}, {"PHY", Imp},   {"TCD", Imp},
     {"JML", Long},       {"EOR", AbsX}, {"LSR", AbsX},  {"EOR", LongX},
     {"RTS", Imp},        {"ADC", IndX}, {"PER", Rel16}, {"ADC", Stack},
     {"STZ", Dp},         {"ADC", Dp},   {"ROR", Dp},    {"ADC", LongInd},
     {"PLA", Imp},        {"ADC", ImmM}, {"ROR", Acc},   {"RTL", Imp},
     {"JMP", AbsInd},     {"ADC", Abs},  {"ROR", Abs},   {"ADC", Long},
     {"BVS", Rel8},       {"ADC", IndY}, {"ADC", Ind},   {"ADC", StackY},
     {"STZ", DpX},        {"ADC", DpX},  {"ROR", DpX},   {"ADC", LongIndY},
     {"SEI", Imp},        {"ADC", AbsY}, {"PLY", Imp},   {"TDC", Imp},
     {"JMP", AbsIndX},    {"ADC", AbsX}, {"ROR", AbsX},  {"ADC", LongX},
     {"BRA", Rel8},       {"STA", IndX}, {"BRL", Rel16}, {"STA", Stack},
     {"STY", Dp},         {"STA", Dp},   {"STX", Dp},    {"STA", LongInd},
     {"DEY", Imp},        {"BIT", ImmM}, {"TXA", Imp},   {"PHB", Imp},
     {"STY", Abs},        {"STA", Abs},  {"STX", Abs},   {"STA", Long},
     {"BCC", Rel8},       {"STA", IndY}, {"STA", Ind},   {"STA", StackY},
     {"STY", DpX},        {"STA", DpX},  {"STX", DpY},   {"STA", LongIndY},
     {"TYA", Imp},        {"STA", AbsY}, {"TXS", Imp},   {"TXY", Imp},
     {"STZ", Abs},        {"STA", AbsX}, {"STZ", AbsX},  {"STA", LongX},
     {"LDY", ImmX},       {"LDA", IndX}, {"LDX", ImmX},  {"LDA", Stack},
     {"LDY", Dp},         {"LDA", Dp},   {"LDX", Dp},    {"LDA", LongInd},
     {"TAY", Imp},        {"LDA", ImmM}, {"TAX", Imp},   {"PLB", Imp},
     {"LDY", Abs},        {"LDA", Abs},  {"LDX", Abs},   {"LDA", Long},
     {"BCS", Rel8},       {"LDA", IndY}, {"LDA", Ind},   {"LDA", StackY},
     {"LDY", DpX},        {"LDA", DpX},  {"LDX", DpY},   {"LDA", LongIndY},
     {"CLV", Imp},        {"LDA", AbsY}, {"TSX", Imp},   {"TYX", Imp},
     {"LDY", AbsX},       {"LDA", AbsX}, {"LDX", AbsY},  {"LDA", LongX},
     {"CPY", ImmX},       {"CMP", IndX}, {"REP", Imm8},  {"CMP", Stack},
     {"CPY", Dp},         {"CMP", Dp},   {"DEC", Dp},    {"CMP", LongInd},
     {"INY", Imp},        {"CMP", ImmM}, {"DEX", Imp},   {"WAI", Imp},
     {"CPY", Abs},        {"CMP", Abs},  {"DEC", Abs},   {"CMP", Long},
     {"BNE", Rel8},       {"CMP", IndY}, {"CMP", Ind},   {"CMP", StackY},
     {"PEI", Ind},        {"CMP", DpX},  {"DEC", DpX},   {"CMP", LongIndY},
     {"CLD", Imp},        {"CMP", AbsY}, {"PHX", Imp},   {"STP", Imp},
     {"JML", AbsLongInd}, {"CMP", AbsX}, {"DEC", AbsX},  {"CMP", LongX},
     {"CPX", ImmX},       {"SBC", IndX}, {"SEP", Imm8},  {"SBC", Stack},
     {"CPX", Dp},         {"SBC", Dp},   {"INC", Dp},    {"SBC", LongInd},
     {"INX", Imp},        {"SBC", ImmM}, {"NOP", Imp},   {"XBA", Imp},
     {"CPX", Abs},        {"SBC", Abs},  {"INC", Abs},   {"SBC", Long},
     {"BEQ", Rel8},       {"SBC", IndY}, {"SBC", Ind},   {"SBC", StackY},
     {"PEA", Abs},        {"SBC", DpX},  {"INC", DpX},   {"SBC", LongIndY},
     {"SED", Imp},        {"SBC", AbsY}, {"PLX", Imp},   {"XCE", Imp},
     {"JSR", AbsIndX},    {"SBC", AbsX}, {"INC", AbsX},  {"SBC", LongX}}};
unsigned Length(Mode mode, CpuDecodeWidth width) {
  switch (mode) {
  case Imp:
  case Acc:
    return 1;
  case ImmM:
    return width.emulation || width.accumulator8 ? 2 : 3;
  case ImmX:
    return width.emulation || width.index8 ? 2 : 3;
  case Abs:
  case AbsX:
  case AbsY:
  case AbsInd:
  case AbsIndX:
  case AbsLongInd:
  case Rel16:
  case Move:
    return 3;
  case Long:
  case LongX:
    return 4;
  default:
    return 2;
  }
}
std::string Hex(uint32_t value, unsigned digits) {
  char text[16];
  std::snprintf(text, sizeof(text), "$%0*X", int(digits), unsigned(value));
  return text;
}
uint32_t Next(uint32_t address, unsigned length) {
  return (address & 0xff0000) | uint16_t(address + length);
}
} // namespace
CpuInstruction Decode65816(uint32_t address, CpuDecodeWidth width,
                           const std::function<std::optional<uint8_t>(uint32_t)> &reader) {
  if (address > 0xffffff)
    throw std::out_of_range("CPU addresses must be between 000000 and FFFFFF");
  CpuInstruction result;
  result.address = address;
  result.bytes[0] = reader(address);
  result.next = Next(address, 1);
  if (!result.bytes[0]) {
    result.mnemonic = "Unavailable";
    return result;
  }
  const auto op = ops[*result.bytes[0]];
  result.mnemonic = op.name;
  result.length = Length(op.mode, width);
  result.next = Next(address, result.length);
  result.available = true;
  uint32_t value = 0;
  for (unsigned index = 1; index < result.length; ++index) {
    result.bytes[index] = reader(Next(address, index));
    if (result.bytes[index])
      value |= uint32_t(*result.bytes[index]) << ((index - 1) * 8);
    else
      result.available = false;
  }
  if (!result.available) {
    result.operand = "??";
    return result;
  }
  const auto byte = Hex(value & 0xff, 2), word = Hex(value & 0xffff, 4), full = Hex(value, 6);
  switch (op.mode) {
  case Imp:
    break;
  case Acc:
    result.operand = "A";
    break;
  case Imm8:
  case ImmM:
  case ImmX:
    result.operand = "#" + Hex(value, (result.length - 1) * 2);
    break;
  case Dp:
    result.operand = byte;
    break;
  case DpX:
    result.operand = byte + ",X";
    break;
  case DpY:
    result.operand = byte + ",Y";
    break;
  case Ind:
    result.operand = "(" + byte + ")";
    break;
  case IndX:
    result.operand = "(" + byte + ",X)";
    break;
  case IndY:
    result.operand = "(" + byte + "),Y";
    break;
  case LongInd:
    result.operand = "[" + byte + "]";
    break;
  case LongIndY:
    result.operand = "[" + byte + "],Y";
    break;
  case Stack:
    result.operand = byte + ",S";
    break;
  case StackY:
    result.operand = "(" + byte + ",S),Y";
    break;
  case Abs:
    result.operand = word;
    break;
  case AbsX:
    result.operand = word + ",X";
    break;
  case AbsY:
    result.operand = word + ",Y";
    break;
  case Long:
    result.operand = full;
    break;
  case LongX:
    result.operand = full + ",X";
    break;
  case AbsInd:
    result.operand = "(" + word + ")";
    break;
  case AbsIndX:
    result.operand = "(" + word + ",X)";
    break;
  case AbsLongInd:
    result.operand = "[" + word + "]";
    break;
  case Rel8:
  case Rel16: {
    const int displacement = op.mode == Rel8 ? (value & 0x80 ? int(value) - 256 : int(value))
                                             : (value & 0x8000 ? int(value) - 65536 : int(value));
    result.operand = Hex((address & 0xff0000) | uint16_t(result.next + displacement), 6);
    break;
  }
  case Move:
    result.operand = Hex((value >> 8) & 0xff, 2) + "," + byte;
    break;
  }
  return result;
}
} // namespace snes::frontend
