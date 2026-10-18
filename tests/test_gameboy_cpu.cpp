#include "snes/core/gameboy/Cpu.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <vector>

using snes::core::gameboy::Cpu;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Machine final : Cpu::Bus {
  struct Event {
    char kind;
    uint16_t address;
    uint8_t value;
    uint64_t clock;
  };
  std::array<uint8_t, 65536> memory{};
  std::vector<Event> events;
  uint64_t clocks = 0;
  uint64_t interruptAt = UINT64_MAX;
  Cpu cpu{*this};
  Machine() {
    memory[0xff00] = 0xff;
    Cpu::State state;
    state.pc = 0x100;
    state.sp = 0xd000;
    state.h = 0xc1;
    cpu.SetState(state);
  }
  uint8_t Read(uint16_t address) override {
    events.push_back({'r', address, memory[address], clocks});
    return memory[address];
  }
  void Write(uint16_t address, uint8_t value) override {
    events.push_back({'w', address, value, clocks});
    memory[address] = value;
  }
  void Advance(unsigned count) override {
    clocks += count;
    if (clocks >= interruptAt)
      memory[0xff0f] |= 1;
  }
  uint8_t InterruptEnable() const override {
    return memory[0xffff];
  }
  uint8_t InterruptFlags() const override {
    return memory[0xff0f];
  }
  void AcknowledgeInterrupt(uint8_t mask) override {
    events.push_back({'a', 0xff0f, mask, clocks});
    memory[0xff0f] &= uint8_t(~mask);
  }
  void DriveAddress(uint16_t address) override {
    events.push_back({'i', address, 0, clocks});
  }
  void AddressIncremented(uint16_t address) override {
    events.push_back({'+', address, 0, clocks});
  }
  void Program(std::initializer_list<uint8_t> bytes) {
    std::copy(bytes.begin(), bytes.end(), memory.begin() + 0x100);
  }
};

void ArithmeticVectors() {
  struct Vector {
    uint8_t operation, a, b, flags, result, resultFlags;
  };
  constexpr Vector vectors[] = {
      {0, 0x7f, 1, 0, 0x80, 0x20},       {0, 0xff, 1, 0, 0, 0xb0},
      {1, 0x0f, 0, 0x10, 0x10, 0x20},    {1, 0xff, 0, 0x10, 0, 0xb0},
      {2, 0, 1, 0, 0xff, 0x70},          {2, 0x80, 1, 0, 0x7f, 0x60},
      {3, 0x10, 0x0f, 0x10, 0, 0xe0},    {3, 0, 0, 0x10, 0xff, 0x70},
      {4, 0xff, 0, 0x10, 0, 0xa0},       {5, 0xaa, 0xaa, 0, 0, 0x80},
      {6, 0, 0x80, 0xf0, 0x80, 0},       {7, 0, 1, 0, 0, 0x70},
      {7, 0x42, 0x42, 0x10, 0x42, 0xc0},
  };
  for (const auto &vector : vectors) {
    Machine machine;
    auto state = machine.cpu.GetState();
    state.a = vector.a;
    state.b = vector.b;
    state.f = vector.flags;
    machine.cpu.SetState(state);
    machine.memory[0x100] = uint8_t(0x80 | vector.operation << 3);
    machine.cpu.RunInstruction();
    const auto &after = machine.cpu.GetState();
    Check(after.a == vector.result && after.f == vector.resultFlags, "ALU result and flags");
    Check(machine.clocks == 4 && after.pc == 0x101,
          "Register ALU instruction takes one machine cycle");
  }
}

void CallsAndStack() {
  Machine machine;
  machine.Program({0xcd, 0x00, 0x02, 0x00});
  machine.memory[0x200] = 0xc9;
  machine.cpu.RunInstruction();
  Check(machine.cpu.GetState().pc == 0x200 && machine.cpu.GetState().sp == 0xcffe,
        "CALL enters its target and decrements SP twice");
  Check(machine.memory[0xcfff] == 1 && machine.memory[0xcffe] == 3,
        "CALL writes the return address high byte before the low byte");
  Check(machine.clocks == 24, "CALL uses six machine cycles");
  Check(machine.events[machine.events.size() - 2].clock == 16 && machine.events.back().clock == 20,
        "CALL stack stores retain their bus positions");
  machine.cpu.RunInstruction();
  Check(machine.cpu.GetState().pc == 0x103 && machine.cpu.GetState().sp == 0xd000 &&
            machine.clocks == 40,
        "RET restores both stack bytes and consumes sixteen clocks");

  Machine flags;
  flags.Program({0xf1});
  flags.memory[0xd000] = 0xff;
  flags.memory[0xd001] = 0x12;
  flags.cpu.RunInstruction();
  Check(flags.cpu.GetState().a == 0x12 && flags.cpu.GetState().f == 0xf0,
        "POP AF clears the unused flag bits");
}

void RelativeAndStackArithmetic() {
  Machine branch;
  branch.Program({0x20, 0xfd});
  branch.cpu.RunInstruction();
  Check(branch.cpu.GetState().pc == 0xff && branch.clocks == 12, "Taken JR uses a signed offset");
  Check(branch.events.back().kind == 'i' && branch.events.back().address == 0xff &&
            branch.events.back().clock == 8,
        "A taken conditional relative jump drives its target during the idle cycle");
  auto state = branch.cpu.GetState();
  state.pc = 0x100;
  state.f = 0x80;
  branch.cpu.SetState(state);
  branch.clocks = 0;
  branch.events.clear();
  branch.cpu.RunInstruction();
  Check(branch.cpu.GetState().pc == 0x102 && branch.clocks == 8,
        "Untaken JR still fetches its operand");
  Check(branch.events.size() == 2, "An untaken relative jump has no idle address cycle");

  Machine unconditional;
  unconditional.Program({0x18, 0xfd});
  unconditional.cpu.RunInstruction();
  Check(unconditional.cpu.GetState().pc == 0xff && unconditional.clocks == 12 &&
            unconditional.events.back().kind == 'i' &&
            unconditional.events.back().address == 0x102 && unconditional.events.back().clock == 8,
        "An unconditional relative jump drives the sequential address before applying its offset");

  Machine stack;
  stack.Program({0xe8, 0xff});
  state = stack.cpu.GetState();
  state.sp = 1;
  stack.cpu.SetState(state);
  stack.cpu.RunInstruction();
  Check(stack.cpu.GetState().sp == 0 && stack.cpu.GetState().f == 0x30 && stack.clocks == 16,
        "ADD SP negative offset reports the low-byte carries");
  Machine load;
  load.Program({0xf8, 0xff});
  state = load.cpu.GetState();
  state.sp = 0;
  load.cpu.SetState(state);
  load.cpu.RunInstruction();
  Check(load.cpu.GetState().h == 0xff && load.cpu.GetState().l == 0xff &&
            load.cpu.GetState().sp == 0 && load.cpu.GetState().f == 0 && load.clocks == 12,
        "LD HL,SP+offset keeps SP and clears arithmetic flags for zero minus one");
}

void ExtendedMemory() {
  Machine rotate;
  rotate.Program({0xcb, 0x3e});
  rotate.memory[0xc100] = 1;
  rotate.cpu.RunInstruction();
  Check(rotate.memory[0xc100] == 0 && rotate.cpu.GetState().f == 0x90 && rotate.clocks == 16,
        "SRL (HL) reads, shifts and writes with zero and carry");
  Check(rotate.events[2].kind == 'r' && rotate.events[2].clock == 8 &&
            rotate.events[3].kind == 'w' && rotate.events[3].clock == 12,
        "Memory shifts place their read and write on separate machine cycles");
  Machine bit;
  bit.Program({0xcb, 0x46});
  auto state = bit.cpu.GetState();
  state.f = 0x10;
  bit.cpu.SetState(state);
  bit.cpu.RunInstruction();
  Check(bit.cpu.GetState().f == 0xb0 && bit.clocks == 12 && bit.events.size() == 3,
        "BIT (HL) retains carry and does not write memory");
  Machine set;
  set.Program({0xcb, 0xc6, 0xcb, 0x86});
  state = set.cpu.GetState();
  state.f = 0xa0;
  set.cpu.SetState(state);
  set.cpu.RunInstruction();
  set.cpu.RunInstruction();
  Check(set.memory[0xc100] == 0 && set.cpu.GetState().f == 0xa0 && set.clocks == 32,
        "SET and RES preserve all flags");
}

void InterruptAndDelay() {
  Machine machine;
  machine.Program({0xfb, 0x00, 0x00});
  machine.memory[0xffff] = machine.memory[0xff0f] = 1;
  machine.cpu.RunInstruction();
  Check(!machine.cpu.GetState().ime && machine.cpu.GetState().enablePending,
        "EI leaves interrupt entry delayed");
  machine.cpu.RunInstruction();
  Check(machine.cpu.GetState().ime && machine.cpu.GetState().pc == 0x102,
        "The instruction after EI executes before entry");
  machine.events.clear();
  machine.clocks = 0;
  machine.cpu.RunInstruction();
  Check(machine.cpu.GetState().pc == 0x40 && !machine.cpu.GetState().ime &&
            machine.cpu.GetState().sp == 0xcffe && machine.clocks == 20,
        "Interrupt entry takes twenty clocks and selects the highest priority request");
  Check(machine.events.front().kind == 'r' && machine.events.front().address == 0x102,
        "Interrupt entry reads and discards the current opcode");
  Check(machine.events.back().kind == 'a' && machine.events.back().clock == 18 &&
            machine.memory[0xff0f] == 0,
        "Interrupt acknowledgement occurs in the final machine cycle");

  Machine cancel;
  auto state = cancel.cpu.GetState();
  state.pc = 0x42;
  state.sp = 0;
  state.ime = true;
  cancel.cpu.SetState(state);
  cancel.memory[0xffff] = cancel.memory[0xff0f] = 1;
  cancel.cpu.RunInstruction();
  Check(cancel.cpu.GetState().pc == 0 && cancel.memory[0xff0f] == 1,
        "A stack store that clears IE can cancel interrupt vector selection");
}

void HaltAndFetchWrap() {
  Machine bug;
  bug.Program({0x76, 0x3e, 0x12});
  bug.memory[0xffff] = bug.memory[0xff0f] = 1;
  bug.cpu.RunInstruction();
  bug.cpu.RunInstruction();
  Check(bug.cpu.GetState().a == 0x3e && bug.cpu.GetState().pc == 0x102,
        "HALT with a pending disabled interrupt suppresses one opcode increment");

  Machine halted;
  halted.Program({0x76, 0x00});
  halted.cpu.RunInstruction();
  halted.cpu.RunInstruction();
  halted.cpu.RunInstruction();
  Check(halted.clocks == 12 && halted.cpu.GetState().halted,
        "HALT keeps advancing hardware by four clocks per wait");
  halted.memory[0xffff] = halted.memory[0xff0f] = 1;
  halted.cpu.RunInstruction();
  Check(!halted.cpu.GetState().halted && halted.cpu.GetState().pc == 0x101 && halted.clocks == 16,
        "Disabled interrupts wake HALT without executing an opcode in that wait");
  halted.cpu.RunInstruction();
  Check(halted.cpu.GetState().pc == 0x102 && halted.clocks == 20,
        "Normal execution resumes after HALT wakes");

  Machine wrap;
  auto state = wrap.cpu.GetState();
  state.pc = 0xffff;
  wrap.cpu.SetState(state);
  wrap.memory[0xffff] = 0x3e;
  wrap.memory[0] = 0x5a;
  wrap.cpu.RunInstruction();
  Check(wrap.cpu.GetState().a == 0x5a && wrap.cpu.GetState().pc == 1,
        "Instruction operands wrap the address bus");
}

void HaltInterruptPhases() {
  Machine delayed;
  delayed.Program({0xfb, 0x76, 0x00});
  delayed.memory[0xffff] = delayed.memory[0xff0f] = 1;
  delayed.cpu.RunInstruction();
  delayed.cpu.RunInstruction();
  Check(delayed.cpu.GetState().pc == 0x101 && !delayed.cpu.GetState().halted &&
            delayed.cpu.GetState().ime,
        "EI followed by HALT leaves the interrupt return address on HALT");
  delayed.cpu.RunInstruction();
  Check(delayed.cpu.GetState().pc == 0x40 && delayed.memory[0xcffe] == 1 &&
            delayed.memory[0xcfff] == 1 && delayed.clocks == 28,
        "Interrupt entry after EI and HALT pushes the rewound address");

  for (bool first : {false, true}) {
    Machine machine;
    auto state = machine.cpu.GetState();
    state.halted = true;
    state.firstHaltWait = first;
    machine.cpu.SetState(state);
    machine.memory[0xffff] = 1;
    machine.interruptAt = 2;
    machine.cpu.RunInstruction();
    Check(machine.clocks == 4 && machine.cpu.GetState().halted == first,
          "Later HALT waits sample interrupts between their two clock halves");
    if (first) {
      machine.cpu.RunInstruction();
      Check(machine.clocks == 8 && !machine.cpu.GetState().halted,
            "The first HALT wait samples before advancing hardware");
    }
  }
}

void StopAndIllegalOpcodes() {
  Machine pressed;
  pressed.Program({0x10, 0x00, 0x00});
  pressed.memory[0xff00] = 0xfe;
  pressed.cpu.RunInstruction();
  Check(pressed.cpu.GetState().halted && pressed.cpu.GetState().firstHaltWait &&
            !pressed.cpu.GetState().stopped && pressed.cpu.GetState().pc == 0x102 &&
            pressed.clocks == 8,
        "STOP with a held button consumes its padding byte and enters HALT");
  pressed.cpu.RunInstruction();
  Check(pressed.clocks == 12 && pressed.cpu.GetState().halted,
        "A held button does not bypass the HALT wait after STOP");

  Machine pending;
  pending.Program({0x10, 0x04, 0x00});
  pending.memory[0xff00] = 0xfe;
  pending.memory[0xffff] = pending.memory[0xff0f] = 1;
  pending.cpu.RunInstruction();
  pending.cpu.RunInstruction();
  Check(!pending.cpu.GetState().halted && !pending.cpu.GetState().stopped &&
            pending.cpu.GetState().b == 1 && pending.clocks == 8,
        "STOP with a held button and pending interrupt executes its second byte");

  Machine illegal;
  illegal.Program({0xd3, 0x04});
  illegal.memory[0xffff] = 0x1f;
  illegal.cpu.RunInstruction();
  Check(illegal.cpu.GetState().halted && illegal.memory[0xffff] == 0,
        "An illegal opcode disables interrupts and halts the processor");
  illegal.cpu.RunInstruction();
  Check(illegal.cpu.GetState().pc == 0x101 && illegal.cpu.GetState().b == 0 && illegal.clocks == 8,
        "Illegal opcodes leave peripheral clocks running during the halt");
}
} // namespace

int main() {
  try {
    ArithmeticVectors();
    CallsAndStack();
    RelativeAndStackArithmetic();
    ExtendedMemory();
    InterruptAndDelay();
    HaltAndFetchWrap();
    HaltInterruptPhases();
    StopAndIllegalOpcodes();
    std::puts("SM83 generated instruction, bus and interrupt checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
