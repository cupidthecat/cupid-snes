#include "snes/core/Hg51b.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

using snes::core::Hg51b;
namespace {
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Machine : Hg51b {
  std::array<uint8_t, 4096> memory{};
  unsigned clocks = 0;
  bool IsRom(uint32_t address) const override {
    return address < 0x800000;
  }
  bool IsRam(uint32_t address) const override {
    return address >= 0x800000;
  }
  uint8_t ReadExternal(uint32_t address) override {
    return memory[address & 4095];
  }
  void WriteExternal(uint32_t address, uint8_t value) override {
    memory[address & 4095] = value;
  }
  void StepClocks(unsigned count) override {
    clocks += count;
  }
};
void ProductWriteWidth() {
  Machine cpu;
  auto state = cpu.GetState();
  state.control.halted = false;
  state.registers.accumulator = 0x123456;
  state.registers.product = 0x123456abcdefull;
  state.program[0][0] = 0xe001;
  cpu.SetState(state);
  cpu.RunStep();
  Check(cpu.GetState().registers.product == 0x56abcdef,
        "A high-product write transfers the low source byte and preserves the low product");
}
void CacheRefill() {
  Machine cpu;
  auto state = cpu.GetState();
  state.control.halted = false;
  state.control.cache.base = 0x8000;
  cpu.memory[0] = 0x2a;
  cpu.memory[1] = 0x64;
  cpu.SetState(state);
  cpu.RunStep();
  Check(cpu.clocks == 2049 && cpu.GetState().registers.accumulator == 0x2a,
        "A cache miss reads 512 ROM bytes with wait states before executing the first word");
  Check(cpu.GetState().control.cache.page == 1 && cpu.GetState().program[1][0] == 0x642a,
        "The alternate cache page retains fetched program words");
}
void DelayedBusAndReset() {
  Machine cpu;
  auto state = cpu.GetState();
  state.control.halted = false;
  state.registers.memoryAddress = 0x800012;
  state.program[0][0] = 0x602f;
  state.program[0][1] = 0x1c00;
  state.program[0][2] = 0x6003;
  state.dataRam[17] = 0x34;
  state.dataRom[31] = 0x123456;
  state.stack[0] = 0x12345;
  cpu.memory[18] = 0x5a;
  cpu.SetState(state);
  cpu.RunStep();
  Check(cpu.Busy() && cpu.GetState().registers.memoryData == 0,
        "A bus-read register schedules a transfer without publishing its byte");
  cpu.RunStep();
  Check(!cpu.Busy() && cpu.GetState().registers.memoryData == 0x5a && cpu.clocks == 4,
        "WAIT drains the remaining bus clocks and publishes the transfer");
  cpu.RunStep();
  Check(cpu.GetState().registers.accumulator == 0x5a,
        "A subsequent load reads the completed bus result");
  cpu.Reset();
  const auto &reset = cpu.GetState();
  Check(reset.registers.accumulator == 0 && reset.control.halted && reset.control.romWait == 3,
        "Reset clears registers and restores control defaults");
  Check(reset.dataRam[17] == 0x34 && reset.dataRom[31] == 0x123456 && reset.stack[0] == 0x12345 &&
            reset.program[0][1] == 0x1c00,
        "Reset retains data memory, program cache words, and return-stack storage");
}
void HostMasksAndAliases() {
  Machine cpu;
  cpu.WriteHost(0x6f50, 0xff);
  cpu.WriteHost(0x7f4e, 0xff);
  cpu.WriteHost(0x7f81, 0x5a);
  Check(cpu.ReadHost(0x7f50) == 0x77 && cpu.ReadHost(0x6f4e) == 0x7f &&
            cpu.ReadHost(0x7fc1) == 0x5a,
        "Host mirrors preserve wait-state masks, bank width, and register aliases");
  cpu.WriteHost(0x7f58, 0);
  Check(cpu.ReadHost(0x7f58) == 0 && cpu.ReadHost(0x7f5e) == 1,
        "A write-only suspend register reports its state through status aliases");
  cpu.WriteHost(0x7f5d, 0);
  Check(cpu.ReadHost(0x7f5e) == 0, "The resume register releases suspension");
}
void ArithmeticAndMultiply() {
  Machine cpu;
  auto state = cpu.GetState();
  state.control.halted = false;
  state.registers.general[0] = 0x7fffff;
  state.program[0][0] = 0x6060;
  state.program[0][1] = 0x8401;
  state.program[0][2] = 0x9c02;
  cpu.SetState(state);
  cpu.RunStep();
  cpu.RunStep();
  Check(cpu.GetState().registers.accumulator == 0x800000 &&
            cpu.GetState().registers.flags == (Hg51b::Negative | Hg51b::Overflow),
        "Addition wraps at 24 bits and reports signed overflow without carry");
  cpu.RunStep();
  Check(cpu.GetState().registers.product == 0xffffff000000ull &&
            cpu.GetState().registers.flags == (Hg51b::Negative | Hg51b::Overflow),
        "Signed multiplication retains its 48-bit result without changing flags");
}
void ConditionalCallAndReturn() {
  Machine cpu;
  auto state = cpu.GetState();
  state.control.halted = false;
  state.registers.flags = Hg51b::Zero;
  const uint16_t program[]{0x0c03, 0x6400, 0x6400, 0x2806, 0xe060, 0xfc00, 0x642a, 0x3c00};
  std::copy(std::begin(program), std::end(program), state.program[0].begin());
  cpu.SetState(state);
  for (unsigned i = 0; i < 6; ++i)
    cpu.RunStep();
  Check(cpu.GetState().registers.general[0] == 0x2a && cpu.GetState().control.halted &&
            cpu.GetState().stack[0] == 0 && cpu.clocks == 12,
        "A conditional jump and subroutine preserve the return address and branch clocks");
}
} // namespace
int main() {
  try {
    ProductWriteWidth();
    CacheRefill();
    DelayedBusAndReset();
    HostMasksAndAliases();
    ArithmeticAndMultiply();
    ConditionalCallAndReturn();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("HG51B processor tests passed");
}
