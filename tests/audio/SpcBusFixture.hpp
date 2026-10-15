#pragma once

#include "snes/core/Spc700.hpp"

#include <array>
#include <cstdint>

namespace spc_bus_test {
using Registers = snes::core::Spc700::Registers;

struct Fixture {
  std::array<uint8_t, 65536> memory{};
  Registers registers;
};

inline Fixture Prepare(uint8_t opcode, unsigned scenario) {
  Fixture fixture;
  for (unsigned address = 0; address < fixture.memory.size(); ++address)
    fixture.memory[address] = uint8_t(address * 37 + 0x13 + scenario * 0x3e);
  auto &r = fixture.registers;
  const bool take = (scenario & 1) != 0;
  const bool wrap = scenario >= 2;
  r.pc = scenario == 2 ? 0xfffe : scenario == 3 ? 0x1fe : 0x200;
  r.a = 0x5a;
  r.x = wrap ? 0xff : 7;
  r.y = scenario == 2 ? 0xff : scenario == 3 ? 0 : 0x39;
  r.s = scenario == 2 ? 0 : scenario == 3 ? 0xff : 0xd0;
  r.p.Unpack(take ? 0xff : 0);
  const uint8_t operand = wrap ? 0xff : 0x40;
  fixture.memory[r.pc] = opcode;
  fixture.memory[uint16_t(r.pc + 1)] = operand;
  fixture.memory[uint16_t(r.pc + 2)] = wrap ? 0xff : 0x20;
  auto direct = [&](uint8_t address, uint8_t value) {
    fixture.memory[(r.p.p ? 0x100 : 0) | address] = value;
  };
  switch (opcode) {
  case 0x10:
    r.p.n = !take;
    break;
  case 0x30:
    r.p.n = take;
    break;
  case 0x50:
    r.p.v = !take;
    break;
  case 0x70:
    r.p.v = take;
    break;
  case 0x90:
    r.p.c = !take;
    break;
  case 0xb0:
    r.p.c = take;
    break;
  case 0xd0:
    r.p.z = !take;
    break;
  case 0xf0:
    r.p.z = take;
    break;
  case 0x2e:
    direct(operand, uint8_t(r.a + unsigned(take)));
    break;
  case 0xde:
    direct(uint8_t(operand + r.x), uint8_t(r.a + unsigned(take)));
    break;
  case 0x6e:
    direct(operand, take ? 2 : 1);
    break;
  case 0xfe:
    r.y = take ? 2 : 1;
    break;
  default:
    break;
  }
  if ((opcode & 15) == 3) {
    const unsigned bit = opcode >> 5;
    const bool set = take == ((opcode & 0x10) == 0);
    direct(operand, set ? uint8_t(1u << bit) : 0);
  }
  // An indexed direct-page operand can alias the instruction in page one.
  fixture.memory[r.pc] = opcode;
  return fixture;
}

inline constexpr uint64_t InitialDigest = 0xcbf29ce484222325;

inline void Record(uint64_t &digest, unsigned kind, uint16_t address, uint8_t data,
                   const Registers &r) {
  const std::array<uint8_t, 13> bytes{uint8_t(kind),
                                      uint8_t(address),
                                      uint8_t(address >> 8),
                                      data,
                                      uint8_t(r.pc),
                                      uint8_t(r.pc >> 8),
                                      r.a,
                                      r.x,
                                      r.y,
                                      r.s,
                                      r.p.Pack(),
                                      uint8_t(r.wait),
                                      uint8_t(r.stop)};
  for (uint8_t value : bytes) {
    digest ^= value;
    digest *= 0x100000001b3;
  }
}
} // namespace spc_bus_test
