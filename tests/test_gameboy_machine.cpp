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
  void InitializePowerOnMemory(std::span<const uint8_t, 160> objects,
                               std::span<const uint8_t, 16> wave) override {
    std::copy(objects.begin(), objects.end(), memory.begin() + 0xfe00);
    std::copy(wave.begin(), wave.end(), memory.begin() + 0xff30);
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

void StartupMemory() {
  constexpr uint64_t expected[2][3][3] = {
      {
          {0x0c0f1a4679c7715dull, 0x7d79c384beba1dccull, 0xf5a3c7d962e56e0bull},
          {0x70e1144bc8285336ull, 0x4a67f410f6b918dbull, 0x6f16ec3ce51c1813ull},
          {0x45ba11aad0f18c62ull, 0x85da23cd5bb6800full, 0x168262b79cdbdd64ull},
      },
      {
          {0xbaa3d27ccc778008ull, 0x64b554027d36d468ull, 0x9d7f0c1faf22b66aull},
          {0x61803d732ff687d0ull, 0x96eab4661e66d488ull, 0x9e91b11b6a67a433ull},
          {0xb3079542b8efeda1ull, 0x48e1b8906e853d82ull, 0x796129d74b55a350ull},
      },
  };
  constexpr std::array<uint64_t, 3> seeds{0, 1, 0x123456789abcdef0ull};
  for (unsigned model = 0; model < 2; ++model)
    for (unsigned seed = 0; seed < seeds.size(); ++seed) {
      System system;
      system.machine.SetStartupEntropy(true, seeds[seed]);
      for (unsigned reset = 0; reset < 3; ++reset) {
        system.machine.Reset(model != 0);
        uint64_t signature = 14695981039346656037ull;
        const auto include = [&](uint16_t first, unsigned size) {
          for (unsigned offset = 0; offset < size; ++offset) {
            signature ^= system.machine.ReadMemory(uint16_t(first + offset));
            signature *= 1099511628211ull;
          }
        };
        include(0xc000, 8192);
        include(0xff80, 127);
        include(0xfe00, 160);
        include(0xff30, 16);
        include(0x8000, 8192);
        Check(signature == expected[model][seed][reset],
              "Startup memory follows the model, seed and persistent noise stream");
        Check(system.machine.Clocks() == 0 && system.machine.CpuState().pc == 0,
              "Power-on memory does not execute instructions or advance devices");
      }
    }
}

void CameraStartupStream() {
  constexpr uint64_t expected[2][2][3] = {
      {
          {0x2846e7bbed5bde3full, 0xab23c007621ff56eull, 0x3d53c908eb812348ull},
          {0x26a8b5c741d32368ull, 0x7e0a243b053a89dfull, 0x554674ce5b19108bull},
      },
      {
          {0xd004fab0cc1a437aull, 0xfd17f441c643cc43ull, 0x52da1298c5af3e51ull},
          {0xba75dfdb469849d2ull, 0xe2ef6aa42d394f63ull, 0x143727bbc6c5afb0ull},
      },
  };
  constexpr std::array<uint64_t, 2> seeds{1, 0x123456789abcdef0ull};
  for (unsigned model = 0; model < 2; ++model)
    for (unsigned seed = 0; seed < seeds.size(); ++seed) {
      System system;
      system.rom[0x147] = 0xfc;
      system.rom[0x149] = 4;
      system.machine.SetStartupEntropy(true, seeds[seed]);
      Check(system.machine.Load(system.rom, system.boot), "Load a generated Camera cartridge");
      if (model) {
        // Select the second model for the first powered state too.
        system.machine.SetStartupEntropy(true, seeds[seed]);
        system.machine.Reset(true);
      }
      for (unsigned reset = 0; reset < 3; ++reset) {
        system.machine.WriteMemory(0x4000, 0x10);
        system.machine.WriteMemory(0xa000, 0);
        system.machine.Reset(model != 0);
        uint64_t signature = 14695981039346656037ull;
        for (const auto [first, size] :
             {std::pair{0xc000, 8192}, {0xff80, 127}, {0xfe00, 160}, {0xff30, 16}, {0x8000, 8192}})
          for (unsigned offset = 0; offset < unsigned(size); ++offset) {
            signature ^= system.machine.ReadMemory(uint16_t(first + offset));
            signature *= 1099511628211ull;
          }
        Check(signature == expected[model][seed][reset],
              "Camera control writes and subsequent resets share one noise stream");
      }
    }
}

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

void CameraSleepAndDma() {
  for (bool stop : {false, true}) {
    System system;
    system.rom[0x147] = 0xfc;
    system.rom[0x149] = 3;
    system.Load({uint8_t(stop ? 0x10 : 0x76), 0, 0});
    auto &machine = system.machine;
    machine.WriteMemory(0xff00, 0x20);
    machine.WriteMemory(0x4000, 0x10);
    machine.WriteMemory(0xa001, 0x80);
    machine.WriteMemory(0xa000, 1);
    machine.AdvanceHardware(129792 - 32);
    machine.RunInstruction();
    Check(stop ? machine.CpuState().stopped : machine.CpuState().halted,
          "The CPU enters sleep while capture is pending");
    machine.AdvanceHardware(1000);
    Check(machine.ReadMemory(0xa000) == 1, "HALT and STOP freeze the Camera capture clock");
    if (stop) {
      system.host.Write(0x6004, 0xfe);
      machine.WriteMemory(0xff00, 0x20);
    } else {
      machine.WriteMemory(0xffff, 1);
      machine.WriteMemory(0xff0f, 1);
    }
    machine.RunInstruction();
    Check(!machine.CpuState().halted && !machine.CpuState().stopped,
          "Controller input or an interrupt wakes the sleeping CPU");
    machine.AdvanceHardware(64);
    Check(machine.ReadMemory(0xa000) == 0, "Capture completes after the active clock resumes");
  }

  constexpr std::array<std::pair<unsigned, unsigned>, 12> boundaries = {{{1, 0},
                                                                         {64, 0},
                                                                         {65, 0},
                                                                         {128, 0},
                                                                         {129, 31},
                                                                         {193, 47},
                                                                         {257, 63},
                                                                         {321, 79},
                                                                         {385, 95},
                                                                         {449, 111},
                                                                         {577, 143},
                                                                         {641, 159}}};
  for (const auto [remaining, zeroBytes] : boundaries) {
    System system;
    system.rom[0x147] = 0xfc;
    system.rom[0x149] = 3;
    system.Load({0});
    auto &machine = system.machine;
    machine.WriteMemory(0x4000, 0x10);
    machine.WriteMemory(0xa001, 0x80);
    for (unsigned i = 6; i < 54; ++i)
      machine.WriteMemory(uint16_t(0xa000 + i), 0xff);
    machine.WriteMemory(0xa000, 1);
    machine.AdvanceHardware(129792 - remaining);
    machine.WriteMemory(0x4000, 0);
    machine.WriteMemory(0xff46, 0xa1);
    machine.AdvanceHardware(64);
    Check(machine.ReadMemory(0xfe00) == 0,
          "A memory read without new clocks preserves the DMA startup batch");
    machine.AdvanceHardware(584);
    for (unsigned i = 0; i < 160; ++i)
      Check(system.devices.memory[0xfe00 + i] == (i < zeroBytes ? 0 : 0xff),
            "DMA retains its startup batch and samples capture completion before each transfer "
            "batch");
  }
}
void ClockResetPhase() {
  for (unsigned model = 0; model < 2; ++model)
    for (uint8_t type : {uint8_t(0x0f), uint8_t(0xfe), uint8_t(0xbc)}) {
      System system;
      system.rom[0x147] = type;
      if (type == 0xbc) {
        system.rom[0x149] = 0xc1;
        system.rom[0x14a] = 0x65;
        system.rom[0x153] = 8;
      }
      uint64_t now = 1789389296;
      unsigned polls = 0;
      system.machine.GameCartridge().SetTimeSource([&] {
        ++polls;
        return now;
      });
      system.Load({0x00, 0xc3, 0, 1});
      const auto bootClocks = system.machine.Clocks();
      system.machine.Reset(model != 0);
      while (system.machine.Clocks() < 1000)
        system.machine.RunInstruction();
      const auto elapsed = bootClocks + system.machine.Clocks();
      system.machine.Reset(model != 0);
      now += 60;
      polls = 0;
      const unsigned rate = model ? 4194304 : 4295454;
      const auto deadline = (rate / 16 + 1) / 2 - elapsed;
      while (polls == 0 && system.machine.Clocks() < deadline + 32)
        system.machine.RunInstruction();
      Check(polls == 1 && system.machine.Clocks() >= deadline &&
                system.machine.Clocks() < deadline + 16,
            "Guest execution retains the battery clock polling edge through processor reset");
    }
}
} // namespace

int main() {
  try {
    StartupMemory();
    CameraStartupStream();
    BootAndMemory();
    OpenBusAndJoypad();
    RegisterPhases();
    InterruptStackAndTimer();
    DmaAndContention();
    CameraSleepAndDma();
    ClockResetPhase();
    std::puts("Handheld CPU, memory, device timing and DMA checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
