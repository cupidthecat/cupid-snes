#include "snes/core/gameboy/Machine.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>

using snes::core::gameboy::Cpu;
using snes::core::gameboy::Host;
using snes::core::gameboy::Machine;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Devices final : Machine::Devices {
  struct WriteEvent {
    uint64_t clock;
    uint16_t address;
    uint8_t value;
  };
  std::array<uint8_t, 65536> memory{};
  std::vector<WriteEvent> writes;
  uint64_t clocks = 0, interruptAt = UINT64_MAX;
  void Reset(bool) override {
    memory.fill(0);
    writes.clear();
    clocks = 0;
    interruptAt = UINT64_MAX;
  }
  uint8_t Read(uint16_t address, Machine::Access) override {
    return memory[address];
  }
  uint8_t Write(uint16_t address, uint8_t value, Machine::Access) override {
    memory[address] = value;
    writes.push_back({clocks, address, value});
    return 0;
  }
  uint8_t Advance(unsigned count) override {
    const auto previous = clocks;
    clocks += count;
    return previous < interruptAt && clocks >= interruptAt ? 4 : 0;
  }
  void AudioDividerEdge(bool, uint64_t) override {}
};
struct System {
  Host host;
  Devices devices;
  Machine machine{host, devices};
  std::vector<uint8_t> rom = std::vector<uint8_t>(32768);
  std::array<uint8_t, 256> boot{};
  System() {
    boot[0] = 0xc3;
    boot[1] = 0;
    boot[2] = 1;
  }
  void Load(std::initializer_list<uint8_t> program) {
    std::copy(program.begin(), program.end(), rom.begin() + 0x100);
    Check(machine.Load(rom, boot), "Load generated handheld ROM and boot firmware");
    machine.RunInstruction();
    Check(machine.CpuState().pc == 0x100, "Boot firmware jumps into the cartridge program");
  }
};

void BootAndMemory() {
  System system;
  system.Load({0x3e, 2, 0xe0, 0x50, 0x3e, 1, 0xe0, 0x50, 0x00});
  system.machine.RunInstruction();
  system.machine.RunInstruction();
  Check(system.machine.ReadMemory(0xff50) == 0xfe && system.machine.ReadMemory(0) == 0xc3,
        "Only boot-disable bit zero disconnects the firmware");
  system.machine.RunInstruction();
  system.machine.RunInstruction();
  Check(system.machine.ReadMemory(0xff50) == 0xff && system.machine.ReadMemory(0) == 0,
        "Disabling firmware reveals cartridge bytes at the bottom of memory");
  system.machine.WriteMemory(0xff50, 0);
  Check(system.machine.ReadMemory(0xff50) == 0xff, "Firmware remains disconnected until reset");
  system.machine.WriteMemory(0xc123, 0x45);
  system.machine.WriteMemory(0xf456, 0x67);
  Check(system.machine.ReadMemory(0xe123) == 0x45 && system.machine.ReadMemory(0xd456) == 0x67,
        "Work RAM mirrors retain both 4 KiB banks");
  system.machine.Reset(true);
  Check(system.machine.ReadMemory(0xc123) == 0x55 && system.machine.ReadMemory(0xff80) == 0,
        "The second adapter model uses its own deterministic WRAM power pattern");
}

void OpenBusAndJoypad() {
  System system;
  system.Load({0x00});
  system.machine.WriteMemory(0xc000, 0x37);
  system.machine.AdvanceHardware(5);
  Check(system.machine.ReadMemory(0xa000) == 0x37,
        "Unconnected cartridge RAM preserves the main data-bus value");
  system.machine.AdvanceHardware(1);
  Check(system.machine.ReadMemory(0xa000) == 0xff,
        "The undriven main bus decays after six handheld clocks");
  system.host.Write(0x6004, 0xa5);
  system.machine.WriteMemory(0xff0f, 0);
  system.machine.WriteMemory(0xff00, 0x20);
  Check(system.machine.ReadMemory(0xff00) == 0xc5 && system.machine.ReadMemory(0xff0f) == 0xf0,
        "Joypad pins latch active-low inputs and request an interrupt on a falling bit");
  system.host.Write(0x6004, 0xff);
  Check(system.machine.ReadMemory(0xff00) == 0xc5,
        "Controller changes remain pending until another joypad pin write");
  system.machine.WriteMemory(0xff00, 0x20);
  Check(system.machine.ReadMemory(0xff00) == 0xcf,
        "Rewriting select pins refreshes the input latch");
}

void RegisterPhases() {
  for (const auto [address, phase] :
       {std::pair{0xff42, -1}, {0xff43, -2}, {0xff47, -1}, {0xff48, -1}, {0xff49, -1}}) {
    System system;
    system.Load({0xe0, uint8_t(address), 0x00});
    auto state = system.machine.CpuState();
    state.a = 0x53;
    system.machine.SetCpuState(state);
    const auto before = system.devices.clocks;
    system.machine.RunInstruction();
    Check(system.devices.writes.back().address == address &&
              system.devices.writes.back().value == 0x53 &&
              system.devices.writes.back().clock == before + 8 + phase,
          "Video register effects keep their position within the CPU write cycle");
    Check(system.devices.clocks == system.machine.Clocks(),
          "Returning an instruction commits all outstanding device clocks");
  }
  System late;
  late.Load({0xe0, 0x0f});
  const auto before = late.machine.Clocks();
  late.devices.interruptAt = before + 9;
  late.machine.RunInstruction();
  Check(late.machine.ReadMemory(0xff0f) == 0xe0,
        "An IF write at the ninth clock replaces an interrupt raised on that clock");
}

void InterruptStackAndTimer() {
  System system;
  system.Load({0x00});
  auto state = system.machine.CpuState();
  state.pc = 0x100;
  state.sp = 0xff11;
  state.ime = true;
  system.machine.SetCpuState(state);
  system.machine.WriteMemory(0xffff, 1);
  system.machine.WriteMemory(0xff0f, 1);
  Check(system.machine.RunInstruction() == 20 && system.machine.CpuState().pc == 0x40 &&
            system.machine.CpuState().sp == 0xff0f,
        "Interrupt entry samples IF before the low stack byte overwrites it");

  system.machine.Reset();
  state = system.machine.CpuState();
  state.pc = 0x100;
  state.sp = 0xd000;
  state.ime = true;
  system.machine.SetCpuState(state);
  system.machine.WriteMemory(0xff50, 1);
  system.machine.WriteMemory(0xff04, 0);
  system.machine.WriteMemory(0xff05, 0xff);
  system.machine.WriteMemory(0xff06, 0x42);
  system.machine.WriteMemory(0xff07, 5);
  system.machine.WriteMemory(0xffff, 4);
  for (unsigned i = 0; i < 5; ++i)
    Check(system.machine.RunInstruction() == 4,
          "NOP retains its clock count during timer overflow");
  Check(system.machine.CpuState().pc == 0x105 && system.machine.ReadMemory(0xff05) == 0x42,
        "The timer requests an interrupt after the reload window");
  Check(system.machine.RunInstruction() == 20 && system.machine.CpuState().pc == 0x50,
        "The next instruction boundary enters the timer interrupt vector");
}

void DmaAndContention() {
  System system;
  system.Load({0x00});
  for (unsigned i = 0; i < 160; ++i)
    system.machine.WriteMemory(uint16_t(0xd000 + i), uint8_t(0xb3 + i * 37));
  system.machine.WriteMemory(0xff46, 0xd0);
  system.machine.AdvanceHardware(5);
  Check(system.devices.memory[0xfe00] == 0, "OAM DMA keeps its startup delay");
  system.machine.AdvanceHardware(1);
  Check(system.devices.memory[0xfe00] == 0xb3, "DMA copies its first byte after the startup delay");
  Check(system.machine.ReadMemory(0xc555) == 0xb3 && system.machine.ReadMemory(0xd001) == 0xd8,
        "DMA redirects competing reads while retaining the current-source shortcut");
  system.machine.WriteMemory(0xc555, 0x0f);
  Check(system.devices.memory[0xfe00] == 3,
        "A competing main-bus write combines with the last transferred OAM byte");
  system.machine.AdvanceHardware(640);
  Check(system.machine.ReadMemory(0xc555) == 0 && system.machine.ReadMemory(0xd000) == 0xb3 &&
            system.devices.memory[0xfe9f] == uint8_t(0xb3 + 159 * 37),
        "DMA releases the bus after all 160 bytes and leaves source RAM intact");
}
} // namespace

int main() {
  try {
    BootAndMemory();
    OpenBusAndJoypad();
    RegisterPhases();
    InterruptStackAndTimer();
    DmaAndContention();
    std::puts("Handheld CPU, memory, device timing and DMA checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
