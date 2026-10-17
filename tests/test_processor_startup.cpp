#include "snes/core/Emulator.hpp"
#include "snes/core/SnesCpu.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace snes::core;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct RecordingBus : ICpuBus {
  std::array<uint8_t, 65536> bytes{};
  std::vector<std::tuple<bool, uint32_t, uint8_t, uint64_t>> accesses;
  uint64_t elapsed = 0;
  uint8_t speed = 8;
  std::function<void(bool, uint32_t, uint8_t)> observe;

  RecordingBus() {
    bytes[0] = 0x4c;
    bytes[0xfffc] = 0x34;
    bytes[0xfffd] = 0x92;
    bytes[0xfffa] = 0x78;
    bytes[0xfffb] = 0x95;
    bytes[0x9234] = 0xea;
  }
  uint8_t Read(uint32_t address) override {
    const uint8_t value = bytes[address & 0xffff];
    accesses.emplace_back(false, address, value, elapsed);
    if (observe)
      observe(false, address, value);
    return value;
  }
  void Write(uint32_t address, uint8_t value) override {
    accesses.emplace_back(true, address, value, elapsed);
    if (observe)
      observe(true, address, value);
    bytes[address & 0xffff] = value;
  }
  uint8_t Speed(uint32_t) const override {
    return speed;
  }
};

std::vector<uint8_t> Image(std::initializer_list<uint8_t> program, bool pal = false) {
  std::vector<uint8_t> rom(0x8000);
  std::copy(program.begin(), program.end(), rom.begin());
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0x7fc0] = 'T';
  rom[0x7fd5] = 0x20;
  rom[0x7fd6] = 2;
  rom[0x7fd7] = 5;
  rom[0x7fd8] = 3;
  rom[0x7fd9] = pal ? 2 : 1;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffd] = 0x80;
  return rom;
}

std::unique_ptr<Emulator> Machine(bool pal = false) {
  auto machine = std::make_unique<Emulator>();
  std::string error;
  Check(machine->LoadCartridge(Image({0xdb}, pal), &error), "Startup fixture loads");
  return machine;
}

void DeferredReset() {
  RecordingBus bus;
  SnesCpu cpu(bus);
  cpu.SetClockCallback([&](uint32_t clocks) { bus.elapsed += clocks; });
  cpu.Reset();
  Check(bus.accesses.empty() && bus.elapsed == 0 && cpu.Cycles() == 0,
        "Reset queues startup without executing before hardware setup is complete");
  Check(cpu.regs().pc == 0 && cpu.regs().s == 0x1ff && cpu.regs().p == 0x34 && cpu.regs().e,
        "Queued reset exposes the initial processor register state");
  bus.bytes[0xfffc] = 0x56;
  Check(cpu.Step() == 186 && cpu.regs().pc == 0x9256,
        "The first CPU step fetches the current vector after the reset hold");
}

void ResetBusPhases() {
  for (uint8_t speed : {6, 8, 12}) {
    RecordingBus bus;
    bus.speed = speed;
    SnesCpu cpu(bus);
    std::vector<uint32_t> clocks;
    cpu.SetClockCallback([&](uint32_t count) {
      clocks.push_back(count);
      bus.elapsed += count;
    });
    unsigned polls = 0;
    cpu.SetInterruptPollCallback([&] {
      ++polls;
      Check(bus.elapsed == 138 + 5 * speed && cpu.regs().pc == 0x34,
            "Reset polls interrupts after installing the vector's low byte");
      return true;
    });
    cpu.Reset();
    cpu.Step();
    const std::vector<std::tuple<bool, uint32_t, uint8_t, uint64_t>> expected{
        {false, 0, 0x4c, 128u + speed},          {true, 0x1ff, 0, 138u + 2 * speed},
        {true, 0x1fe, 0, 138u + 3 * speed},      {true, 0x1fd, 0x24, 138u + 4 * speed},
        {false, 0xfffc, 0x34, 134u + 5 * speed}, {false, 0xfffd, 0x92, 134u + 6 * speed}};
    Check(bus.accesses == expected,
          "Reset performs its dummy read, stack writes, and vector reads on their bus phases");
    Check(clocks.size() >= 22 && std::all_of(clocks.begin(), clocks.begin() + 22,
                                             [](uint32_t value) { return value == 6; }),
          "The reset hold advances hardware in six-clock intervals without bus accesses");
    Check(cpu.Cycles() == 138 + 6 * speed && bus.elapsed == cpu.Cycles() && polls == 1,
          "Reset accounts for every hold, bus, and idle clock exactly once");
    Check(cpu.regs().s == 0x1fc && cpu.regs().pc == 0x9234 && cpu.regs().vector == 0xfffc &&
              cpu.regs().mar == 0xfffd && cpu.regs().mdr == 0x92,
          "Completed reset retains the stack, vector, address, and data latches");
  }
}

void ResetClearsHaltAndPendingState() {
  RecordingBus bus;
  SnesCpu cpu(bus);
  cpu.regs().e = false;
  cpu.regs().p = 0xff;
  cpu.regs().pb = 0x12;
  cpu.regs().pc = 0xabcd;
  cpu.regs().s = 0x4567;
  cpu.regs().wai = cpu.regs().stp = true;
  cpu.RequestNmi();
  cpu.SetIrqLevel(true);
  cpu.Reset();
  Check(!cpu.regs().wai && !cpu.regs().stp && !cpu.interruptPending(),
        "Reset clears stopped, waiting, and stale interrupt state immediately");
  cpu.Step();
  Check(cpu.regs().pc == 0x9234 && cpu.regs().pb == 0 && cpu.regs().s == 0x1fc,
        "Reset takes priority over previous execution state");
  Check(cpu.Step() == 14 && cpu.regs().pc == 0x9235,
        "The instruction after reset runs without accepting an old interrupt");
  cpu.Reset();
  cpu.Step();
  Check(cpu.regs().pc == 0x9234 && cpu.regs().s == 0x1fc,
        "Repeated reset reconstructs the startup frame from the power-on stack");
}

void InterruptDuringReset() {
  RecordingBus bus;
  SnesCpu cpu(bus);
  cpu.SetClockCallback([&](uint32_t clocks) {
    bus.elapsed += clocks;
    if (bus.elapsed == 6) {
      cpu.RequestNmi();
      cpu.SetIrqLevel(true);
    }
  });
  cpu.Reset();
  Check(cpu.Step() == 186 && cpu.regs().pc == 0x9234,
        "An interrupt arriving during reset cannot replace the reset vector fetch");
  cpu.Step();
  Check(cpu.regs().pc == 0x9578 && cpu.regs().vector == 0xfffa && cpu.regs().s == 0x1f9,
        "NMI sampled at reset completion precedes the first opcode while IRQ stays masked");
  Check(bus.bytes[0x1fc] == 0x92 && bus.bytes[0x1fb] == 0x34,
        "The interrupt following reset saves the completed reset destination");
}

void CpuBusLatches() {
  for (uint8_t speed : {6, 8, 12}) {
    for (bool write : {false, true}) {
      RecordingBus bus;
      bus.speed = speed;
      SnesCpu cpu(bus);
      cpu.regs().mdr = 0x22;
      cpu.SetBeforeCycleCallback([&](uint32_t) { cpu.regs().mar = 0xabcdef; });
      cpu.SetClockCallback([&](uint32_t clocks) {
        Check(cpu.regs().mar == 0x123456,
              "The CPU installs its address after arbitration and before leading bus clocks");
        Check(cpu.regs().mdr == 0x22,
              "The CPU data latch retains the previous value through the leading bus phase");
        bus.elapsed += clocks;
      });
      bus.observe = [&](bool isWrite, uint32_t address, uint8_t) {
        Check(address == cpu.regs().mar && cpu.regs().mdr == (isWrite ? 0xa5 : 0x22),
              "Devices observe the CPU address and correct read/write data-latch phase");
      };
      if (write)
        cpu.write(0x123456, 0xa5);
      else
        cpu.read(0x123456);
      Check(cpu.regs().mar == 0x123456 && cpu.regs().mdr == (write ? 0xa5 : 0),
            "CPU bus latches retain the completed access");
    }
  }
}

void InterruptVectorLatches() {
  RecordingBus bus;
  SnesCpu cpu(bus);
  cpu.Reset();
  cpu.Step();
  cpu.regs().e = false;
  cpu.regs().pb = 0x12;
  cpu.regs().pc = 0x9234;
  bus.bytes[0xffea] = 0x78;
  bus.bytes[0xffeb] = 0x95;
  cpu.RequestNmi();
  cpu.Step();
  bool sawHighVector = false;
  bus.observe = [&](bool write, uint32_t address, uint8_t) {
    if (!write && address == 0xffeb) {
      sawHighVector = true;
      Check(cpu.regs().pc == 0x9278 && cpu.regs().pb == 0x12 && cpu.regs().vector == 0xffea,
            "Native interrupt entry installs each vector byte before clearing the program bank");
    }
  };
  cpu.Step();
  Check(sawHighVector && cpu.regs().pc == 0x9578 && cpu.regs().pb == 0,
        "Interrupt completion retains the selected vector and clears the program bank");
}

void DmaBusLatches() {
  for (bool reverse : {false, true}) {
    auto machine = Machine();
    auto &cpu = *machine->GetCpu();
    cpu.Step();
    auto &bus = machine->GetBus();
    auto &dma = machine->GetDma();
    cpu.regs().mdr = 0x55;
    bus.SetOpenBus(0x55);
    unsigned samples = 0;
    bus.Map(
        0x40, 0x40, 0x100, 0x101,
        [&](uint32_t address, uint8_t) {
          Check(cpu.regs().mar == address,
                "DMA installs its A-bus address before sampling the source");
          return uint8_t(address & 1 ? 0x62 : 0x31);
        },
        [&](uint32_t address, uint8_t value) {
          Check(cpu.regs().mar == address && cpu.regs().mdr == value,
                "Reverse DMA preserves the A-bus address and sampled B-bus data");
          ++samples;
        });
    bus.Map(
        0, 0, 0x2110, 0x2111,
        [&](uint32_t address, uint8_t) {
          Check(cpu.regs().mar == 0x400100 + (address & 1),
                "A reverse DMA read retains the associated A-bus address");
          return uint8_t(address & 1 ? 0x62 : 0x31);
        },
        [&](uint32_t address, uint8_t value) {
          Check(cpu.regs().mar == 0x400100 + (address & 1) && cpu.regs().mdr == value &&
                    bus.OpenBus() == value,
                "DMA updates shared CPU and memory data latches before its destination write");
          ++samples;
        });
    const auto hardwarePoll = machine->GetTiming().onIrqPoll;
    machine->GetTiming().onIrqPoll = [&](uint16_t h, uint16_t v, uint16_t display,
                                         uint16_t period) {
      Check(cpu.regs().mdr == bus.OpenBus(),
            "DMA keeps the CPU data latch coherent during the trailing read clocks");
      hardwarePoll(h, v, display, period);
    };
    auto &channel = dma.Channel(0);
    channel.writeControl(reverse ? 0x81 : 1);
    channel.sourceBank = 0x40;
    channel.sourceAddress = 0x100;
    channel.transferSize = 2;
    channel.targetAddress = 0x10;
    dma.EnableDma(1);
    dma.RunDma();
    Check(samples == 2 && cpu.regs().mar == 0x400101 && cpu.regs().mdr == 0x62,
          "DMA leaves the shared bus latches on its last transferred byte");
  }
}

void CpuMemoryDataPhases() {
  auto machine = Machine();
  auto &cpu = *machine->GetCpu();
  cpu.Step();
  auto &bus = machine->GetBus();
  cpu.regs().mdr = 0x55;
  bus.SetOpenBus(0x55);
  bus.Map(
      0x40, 0x40, 0, 0,
      [&](uint32_t, uint8_t openBus) {
        Check(openBus == 0x55 && cpu.regs().mdr == 0x55,
              "The CPU source sees the previous data latch at the sampling edge");
        return uint8_t(0xa5);
      },
      [&](uint32_t, uint8_t value) {
        Check(value == 0x9a && bus.OpenBus() == value && cpu.regs().mdr == value,
              "CPU writes update both data latches before the device callback");
      });
  const auto hardwarePoll = machine->GetTiming().onIrqPoll;
  machine->GetTiming().onIrqPoll = [&](uint16_t h, uint16_t v, uint16_t display, uint16_t period) {
    Check(cpu.regs().mdr == 0x55 && bus.OpenBus() == 0x55,
          "A CPU read commits neither data latch before its trailing clocks finish");
    hardwarePoll(h, v, display, period);
  };
  cpu.read(0x400000);
  machine->GetTiming().onIrqPoll = hardwarePoll;
  Check(cpu.regs().mdr == 0xa5 && bus.OpenBus() == 0xa5,
        "Both data latches commit the sampled CPU read at completion");
  cpu.write(0x400000, 0x9a);
}

void HdmaTableLatches() {
  for (bool indirect : {false, true}) {
    auto machine = Machine();
    auto &cpu = *machine->GetCpu();
    cpu.Step();
    auto &bus = machine->GetBus();
    auto &dma = machine->GetDma();
    std::array<uint8_t, 5> table = indirect ? std::array<uint8_t, 5>{0x81, 0, 0x20, 0, 0x55}
                                            : std::array<uint8_t, 5>{0x81, 0xa5, 0, 0, 0};
    std::vector<uint32_t> reads;
    bus.Map(
        0x40, 0x40, 0x100, 0x104,
        [&](uint32_t address, uint8_t) {
          Check(cpu.regs().mar == address,
                "Every HDMA descriptor and indirect pointer read updates the address latch");
          reads.push_back(address);
          return table[address - 0x400100];
        },
        [](uint32_t, uint8_t) {});
    bus.Map(
        0x40, 0x40, 0x2000, 0x2000,
        [&](uint32_t address, uint8_t) {
          Check(cpu.regs().mar == address, "Indirect HDMA latches its data source address");
          return uint8_t(0xa5);
        },
        [](uint32_t, uint8_t) {});
    bus.Map(
        0, 0, 0x2110, 0x2110, [](uint32_t, uint8_t value) { return value; },
        [&](uint32_t, uint8_t value) {
          Check(value == 0xa5 && cpu.regs().mdr == value,
                "HDMA destination writes observe the sampled shared data latch");
        });
    auto &channel = dma.Channel(0);
    channel.writeControl(indirect ? 0x40 : 0);
    channel.sourceBank = 0x40;
    channel.sourceAddress = 0x100;
    channel.targetAddress = 0x10;
    channel.indirectBank = 0x40;
    dma.EnableHdma(1);
    dma.HdmaSetup();
    dma.HdmaRun();
    const auto expectedReads =
        indirect ? std::vector<uint32_t>{0x400100, 0x400101, 0x400102, 0x400103, 0x400104}
                 : std::vector<uint32_t>{0x400100, 0x400101, 0x400102};
    Check(reads == expectedReads && cpu.regs().mar == expectedReads.back() &&
              cpu.regs().mdr == (indirect ? 0x55 : 0),
          "HDMA leaves the bus latches on the final table read, including a partial pointer");
  }
}

void DecodedDmaLatches() {
  auto rom = Image({0xdb});
  rom.resize(0x200000);
  rom[0x7fd5] = 0x32;
  rom[0x7fd6] = 0x45;
  rom[0x7fd7] = 11;
  rom[0x100000] = 0xc0; // Packed zero codewords decode to zero bytes.
  auto machine = std::make_unique<Emulator>();
  std::string error;
  Check(machine->LoadCartridge(rom, &error), "The decompression cartridge fixture loads");
  machine->GetCpu()->Step();
  auto &cpu = *machine->GetCpu();
  auto &bus = machine->GetBus();
  auto &dma = machine->GetDma();
  unsigned writes = 0;
  bus.Map(
      0, 0, 0x2110, 0x2110, [](uint32_t, uint8_t value) { return value; },
      [&](uint32_t, uint8_t value) {
        Check(value == 0 && cpu.regs().mar == 0xd00000 && cpu.regs().mdr == 0 && bus.OpenBus() == 0,
              "Decoded DMA updates both data latches with decoded bytes at the source address");
        ++writes;
      });
  bus.Write(0x4800, 1);
  bus.Write(0x4801, 1);
  auto &channel = dma.Channel(0);
  channel.writeControl(8);
  channel.sourceBank = 0xd0;
  channel.sourceAddress = 0;
  channel.transferSize = 2;
  channel.targetAddress = 0x10;
  bus.Write(0x4302, 0);
  bus.Write(0x4303, 0);
  bus.Write(0x4304, 0xd0);
  bus.Write(0x4305, 2);
  bus.Write(0x4306, 0);
  dma.EnableDma(1);
  dma.RunDma();
  Check(writes == 2, "The decoded DMA latch check transfers its complete payload");
}

void InvalidDmaReadLatches() {
  auto machine = Machine();
  auto &cpu = *machine->GetCpu();
  cpu.Step();
  auto &bus = machine->GetBus();
  auto &dma = machine->GetDma();
  bus.Map(
      0, 0, 0x2110, 0x2110, [](uint32_t, uint8_t value) { return value; },
      [&](uint32_t, uint8_t value) {
        Check(value == 0 && cpu.regs().mar == 0x4200 && cpu.regs().mdr == 0,
              "A forbidden DMA source still latches its address and zero data");
      });
  auto &channel = dma.Channel(0);
  channel.writeControl(8);
  channel.sourceBank = 0;
  channel.sourceAddress = 0x4200;
  channel.transferSize = 1;
  channel.targetAddress = 0x10;
  dma.EnableDma(1);
  dma.RunDma();
  channel.writeControl(0x88);
  channel.sourceBank = 0x7e;
  channel.sourceAddress = 0x100;
  channel.transferSize = 1;
  channel.targetAddress = 0x80;
  bus.WramData()[0x100] = 0x55;
  dma.EnableDma(1);
  dma.RunDma();
  Check(cpu.regs().mar == 0x7e0100 && cpu.regs().mdr == 0 && bus.OpenBus() == 0 &&
            bus.WramData()[0x100] == 0,
        "An invalid WRAM-to-WRAM reverse read drives zero through the shared data latch");
}

void HardwareStartupAndReload() {
  for (bool pal : {false, true}) {
    auto machine = Machine(pal);
    for (unsigned load = 0; load < 2; ++load) {
      if (load) {
        std::string error;
        Check(machine->LoadCartridge(Image({0xdb}, pal), &error), "A second cartridge loads");
      }
      Check(machine->GetTiming().MasterClocksElapsed() == 0 &&
                machine->GetSmp().CycleCount() == 0 && machine->GetCpu()->Cycles() == 0,
            "Loading a cartridge leaves all processors at the same unstarted clock");
      std::vector<uint8_t> saved(0x2000, 0x5a);
      machine->LoadSram(saved);
      Check(machine->GetCpu()->Step() == 186 && machine->CurrentMasterCycles() == 186 &&
                machine->GetTiming().MasterClocksElapsed() == 186,
            "Reset advances the shared hardware clock through its complete bus sequence");
      Check(machine->GetSmp().CycleCount() == 8 && machine->GetDsp().Phase() == 8 &&
                machine->GetSmp().GetTimer0().stage0 ==
                    186 * Smp::kHardwareClockFrequency / machine->GetTiming().MasterClockHz(),
            "Sound, DSP, and timers advance during CPU reset in both video regions");
      Check(machine->GetBus().Read(0x700000) == 0x5a && machine->GetBus().WramData()[0x1fd] == 0x24,
            "Saved cartridge data is installed before startup and the reset stack reaches WRAM");
      machine->StepFrame();
    }
  }
}

void FirstInstructionDiagnostic() {
  auto machine = std::make_unique<Emulator>();
  const auto rom = Image({0xad, 0x37, 0x21, // Latch H/V as the first instruction.
                          0xad, 0x3c, 0x21, 0x8f, 0,    1,    0x7e, 0xad, 0x3c, 0x21, 0x29, 1,
                          0x8f, 1,    1,    0x7e, 0xba, 0x8a, 0x8f, 2,    1,    0x7e, 0xdb});
  std::string error;
  Check(machine->LoadCartridge(rom, &error), "The first-instruction diagnostic loads");
  machine->StepFrame();
  const auto *data = machine->GetBus().WramData();
  Check(data[0x100] == 0x35 && data[0x101] == 0 && data[0x102] == 0xfc,
        "The first opcode observes the expected beam position and completed reset stack");
}
} // namespace

int main() {
  unsigned failures = 0;
  for (auto test : {DeferredReset, ResetBusPhases, ResetClearsHaltAndPendingState,
                    InterruptDuringReset, CpuBusLatches, InterruptVectorLatches,
                    CpuMemoryDataPhases, DmaBusLatches, HdmaTableLatches, DecodedDmaLatches,
                    InvalidDmaReadLatches, HardwareStartupAndReload, FirstInstructionDiagnostic}) {
    try {
      test();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s\n", error.what());
      ++failures;
    }
  }
  if (failures)
    return 1;
  std::puts(
      "Processor startup, reset bus phases, shared latches, and first-instruction checks passed");
}
