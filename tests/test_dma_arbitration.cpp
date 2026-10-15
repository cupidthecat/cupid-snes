#include "snes/core/Dma.hpp"
#include "snes/core/Emulator.hpp"
#include "snes/core/SnesCpu.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace snes::core;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::unique_ptr<Emulator> Machine(std::vector<uint8_t> rom) {
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0x7fc0] = 'T';
  rom[0x7fd5] = 0x20;
  rom[0x7fd7] = 5;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffd] = 0x80;
  auto machine = std::make_unique<Emulator>();
  std::string error;
  Check(machine->LoadCartridge(rom, &error), "The DMA diagnostic program loads");
  return machine;
}

struct Collision {
  std::unique_ptr<MemoryBus> bus = std::make_unique<MemoryBus>();
  DmaController dma;
  uint64_t elapsed = 0;
  std::vector<uint8_t> writes;
  std::vector<uint64_t> writtenAt;

  Collision(unsigned phase, unsigned cycle, unsigned mode) : elapsed(phase) {
    bus->MapWram();
    dma.SetBus(bus.get());
    dma.SetClockQuery([this] { return elapsed; });
    dma.SetClockCallback([this](uint32_t clocks) { elapsed += clocks; });
    dma.SetCpuCycleClocks(cycle);
    bus->Map(
        0, 0, 0x2110, 0x2113, [](uint32_t, uint8_t value) { return value; },
        [this](uint32_t, uint8_t value) {
          writes.push_back(value);
          writtenAt.push_back(elapsed);
        });
    std::fill_n(bus->WramData() + 0x1ff0, 32, 0x37);
    bus->WramData()[0x1000] = 0xb9;
    bus->WramData()[0x1001] = 0;
    auto &channel = dma.Channel(0);
    channel.writeControl(uint8_t(mode));
    channel.sourceBank = 0x7e;
    channel.sourceAddress = 0x2000;
    channel.targetAddress = 0x10;
    channel.transferSize = 8;
    channel.hdmaAddress = 0x1000;
    channel.lineCounter = 0x81;
    channel.hdmaDoTransfer = true;
    dma.EnableHdma(1);
    dma.EnableDma(1);
  }
};

void CancelAfterByte() {
  for (unsigned cycle : {6u, 8u, 12u}) {
    for (unsigned phase : {0u, 2u, 4u, 6u}) {
      for (unsigned mode = 0; mode < 8; ++mode) {
        for (unsigned count : {1u, 2u, 8u}) {
          Collision fixture(phase, cycle, mode);
          unsigned boundary = 0;
          fixture.dma.SetBoundaryCallback([&] {
            if (++boundary == 2 + count)
              fixture.dma.HdmaRun();
          });
          fixture.dma.RunDma();
          const auto &channel = fixture.dma.Channel(0);
          Check(channel.sourceAddress == 0x2000 + count,
                "The interrupted byte advances the DMA source before HDMA runs");
          Check(channel.transferSize == 9 - count,
                "An HDMA cancellation leaves the interrupted byte in the remaining count");
          const unsigned length = DmaController::kTransferLengths[mode];
          Check(fixture.writes.size() == count + length,
                "HDMA transfers its complete pattern after the last DMA byte");
          Check(std::all_of(fixture.writes.begin(), fixture.writes.begin() + count,
                            [](uint8_t value) { return value == 0x37; }) &&
                    fixture.writes[count] == 0xb9,
                "Channel cancellation preserves DMA and HDMA transfer order");
          const unsigned dmaClocks = 8 - phase + 16 + count * 8 + 16 + length * 8;
          const unsigned resume = cycle - dmaClocks % cycle;
          Check(fixture.elapsed == phase + dmaClocks + 2 * resume,
                "Cancelling the final DMA channel synchronizes both HDMA exit and DMA exit");
        }
      }
    }
  }
}

void CancelDuringStartup() {
  for (bool setup : {false, true}) {
    for (unsigned cycle : {6u, 8u, 12u}) {
      for (unsigned phase : {0u, 2u, 4u, 6u}) {
        for (unsigned cancelAt : {1u, 2u}) {
          Collision fixture(phase, cycle, 0);
          unsigned boundary = 0;
          fixture.dma.SetBoundaryCallback([&] {
            if (++boundary == cancelAt) {
              if (setup)
                fixture.dma.HdmaSetup();
              else
                fixture.dma.HdmaRun();
            }
          });
          fixture.dma.RunDma();
          const auto &channel = fixture.dma.Channel(0);
          std::vector<uint8_t> expected;
          if (!setup)
            expected.push_back(0xb9);
          if (cancelAt == 2)
            expected.push_back(0x37);
          Check(fixture.writes == expected,
                "A channel already in startup performs its first DMA byte after HDMA cancels it");
          Check(
              channel.sourceAddress == 0x2000 + (cancelAt == 2) && channel.transferSize == 8,
              "Startup cancellation advances the source without decrementing the remaining count");
          const unsigned throughHdma = 8 - phase + cancelAt * 8 + (setup ? 16 : 24);
          const unsigned firstResume = cycle - throughHdma % cycle;
          const unsigned byteClocks = cancelAt == 2 ? 8 : 0;
          const unsigned finalResume = cycle - (throughHdma + byteClocks) % cycle;
          Check(fixture.elapsed == phase + throughHdma + firstResume + byteClocks + finalResume,
                "The first post-cancellation byte follows the HDMA CPU synchronization interval");
        }
      }
    }
  }
}

void PendingDmaBeforeHdma() {
  for (unsigned cycle : {6u, 8u, 12u}) {
    for (unsigned phase : {0u, 2u, 4u, 6u}) {
      for (uint8_t pending : {0, 1, 2}) {
        Collision fixture(phase, cycle, 0);
        fixture.dma.EnableDma(pending);
        const auto ownClocks = fixture.dma.HdmaRun();
        const unsigned transfers = (pending ? 0 : 8 - phase) + 24;
        const unsigned resume = pending == 2 ? 0 : cycle - transfers % cycle;
        Check(ownClocks == transfers + resume && fixture.elapsed == phase + ownClocks,
              "Pending DMA controls HDMA entry alignment and CPU resumption independently");
      }
    }
  }
}

void CompletedChannelOverhead() {
  Collision fixture(2, 6, 0);
  fixture.dma.EnableDma(0);
  fixture.dma.Channel(0).hdmaCompleted = true;
  Check(fixture.dma.HdmaRun() == 18 && fixture.elapsed == 20 && fixture.writes.empty(),
        "An enabled completed HDMA channel retains the requested operation's overhead");
}

void PendingEnableTransition() {
  for (uint8_t enabled : {0, 2}) {
    std::vector<uint8_t> rom(0x8000);
    rom[0] = 0xdb;
    auto machine = Machine(std::move(rom));
    auto &cpu = *machine->GetCpu();
    auto &timing = machine->GetTiming();
    while (timing.HCounter() < 1098)
      cpu.idle();
    Check(timing.HCounter() < 1104 && timing.HCounter() + 6 >= 1104,
          "The enable write crosses the HDMA request position");
    machine->GetDma().Channel(0).hdmaCompleted = false;
    machine->GetDma().Channel(1).hdmaCompleted = true;
    machine->GetDma().EnableHdma(1);
    cpu.write(0x420c, enabled);
    cpu.idle();
    const auto before = timing.MasterClocksElapsed();
    cpu.idle();
    const unsigned overhead = 8 - unsigned(before & 7) + 8;
    const unsigned expected = 6 + (enabled ? overhead + 6 - overhead % 6 : 0);
    Check(timing.MasterClocksElapsed() - before == expected,
          "A pending HDMA request checks enabled channels after a CPU enable-mask write");
  }
}

void OtherChannelContinues() {
  for (unsigned cycle : {6u, 8u, 12u}) {
    Collision fixture(0, cycle, 0);
    auto &next = fixture.dma.Channel(1);
    next.writeControl(0);
    next.sourceBank = 0x7e;
    next.sourceAddress = 0x3000;
    next.transferSize = 4;
    next.targetAddress = 0x10;
    std::fill_n(fixture.bus->WramData() + 0x3000, 4, 0x62);
    fixture.dma.EnableDma(3);
    unsigned boundary = 0;
    fixture.dma.SetBoundaryCallback([&] {
      if (++boundary == 4)
        fixture.dma.HdmaRun();
    });
    fixture.dma.RunDma();
    Check(fixture.writes == std::vector<uint8_t>{0x37, 0x37, 0xb9, 0x62, 0x62, 0x62, 0x62},
          "HDMA cancellation preserves the remaining DMA channel's priority and bytes");
    Check(fixture.dma.Channel(0).transferSize == 7 && next.transferSize == 0,
          "Only the cancelled DMA channel retains its count");
    constexpr unsigned dmaClocks = 8 + 16 + 16 + 24 + 8 + 32;
    Check(fixture.elapsed == dmaClocks + cycle - dmaClocks % cycle,
          "An enabled DMA channel prevents an intermediate CPU synchronization");
  }
}

void SourceAddressAndDirection() {
  for (uint8_t addressing : {0, 8, 16, 24}) {
    for (bool reverseBus : {false, true}) {
      Collision fixture(2, 6, addressing | (reverseBus ? 0x80 : 0));
      if (reverseBus) {
        fixture.bus->Map(
            0, 0, 0x2110, 0x2110, [](uint32_t, uint8_t) { return uint8_t(0x4c); },
            [](uint32_t, uint8_t) {});
      }
      unsigned boundary = 0;
      fixture.dma.SetBoundaryCallback([&] {
        if (++boundary == 4)
          fixture.dma.HdmaRun();
      });
      fixture.dma.RunDma();
      const int delta = (addressing & 8) ? 0 : (addressing & 16) ? -2 : 2;
      Check(fixture.dma.Channel(0).sourceAddress == 0x2000 + delta &&
                fixture.dma.Channel(0).sourceBank == 0x7e &&
                fixture.dma.Channel(0).transferSize == 7,
            "Cancellation preserves increment, decrement, and fixed addressing in both directions");
      if (reverseBus)
        Check(fixture.bus->WramData()[0x2000] == 0x4c,
              "Reverse DMA commits its last byte before HDMA cancellation");
    }
  }
}

void IndirectAliasDuringStartup() {
  Collision fixture(0, 6, 0);
  auto &channel = fixture.dma.Channel(0);
  channel.indirect = true;
  channel.indirectBank = 0x7e;
  fixture.bus->WramData()[8] = 0xb9;
  fixture.bus->WramData()[0x1000] = 0;
  fixture.bus->WramData()[0x1001] = 0x55;
  unsigned boundary = 0;
  fixture.dma.SetBoundaryCallback([&] {
    if (++boundary == 2)
      fixture.dma.HdmaRun();
  });
  fixture.dma.RunDma();
  Check(fixture.writes == std::vector<uint8_t>{0xb9, 0x37},
        "Indirect HDMA can cancel a channel during startup without dropping its first byte");
  Check(channel.transferSize == 0x5500,
        "The HDMA indirect-address alias survives the cancelled DMA continuation");
}

void ProgramCancellation() {
  std::vector<uint8_t> rom(0x8000);
  std::vector<uint8_t> code;
  auto emit = [&](std::initializer_list<uint8_t> bytes) {
    code.insert(code.end(), bytes.begin(), bytes.end());
  };
  auto store = [&](uint16_t address, uint8_t value) {
    emit({0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  emit({0x78, 0xd8});
  store(0x4200, 0);
  store(0x420c, 0);
  for (auto [address, value] : std::array<std::pair<uint16_t, uint8_t>, 7>{{{0x4300, 0},
                                                                            {0x4301, 0x10},
                                                                            {0x4302, 0},
                                                                            {0x4303, 0x90},
                                                                            {0x4304, 0x80},
                                                                            {0x4305, 0},
                                                                            {0x4306, 1}}})
    store(address, value);
  store(0x420c, 1);
  emit({0xad, 0x12, 0x42, 0x10, 0xfb}); // Wait for VBlank.
  emit({0xad, 0x12, 0x42, 0x30, 0xfb}); // Wait for the next field's HDMA setup.
  store(0x420b, 1);
  store(0x420c, 0);
  unsigned output = 0;
  for (uint16_t address : {0x4302, 0x4303, 0x4305, 0x4306}) {
    emit({0xad, uint8_t(address), uint8_t(address >> 8), 0x8f, uint8_t(output++), 0x01, 0x7e});
  }
  emit({0xa9, 0xa5, 0x8f, 0x04, 0x01, 0x7e, 0xdb});
  std::copy(code.begin(), code.end(), rom.begin());
  rom[0x1000] = 0x81;
  rom[0x1001] = 0xb9;
  auto machine = Machine(std::move(rom));
  for (unsigned frame = 0; frame < 3; ++frame)
    machine->StepFrame();
  const auto *data = machine->GetBus().WramData() + 0x100;
  const unsigned source = data[0] | data[1] << 8;
  const unsigned remaining = data[2] | data[3] << 8;
  Check(data[4] == 0xa5 && source > 0x9000 && source < 0x9100,
        "The diagnostic completes after HDMA interrupts a general DMA transfer");
  Check(source - 0x9000 + remaining == 257,
        "A running CPU reads the cancelled byte in DAS after the transfer has ended");
}
} // namespace

int main() {
  unsigned failures = 0;
  for (auto test : {CancelAfterByte, CancelDuringStartup, PendingDmaBeforeHdma,
                    CompletedChannelOverhead, PendingEnableTransition, OtherChannelContinues,
                    SourceAddressAndDirection, IndirectAliasDuringStartup, ProgramCancellation}) {
    try {
      test();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s\n", error.what());
      ++failures;
    }
  }
  if (failures)
    return 1;
  std::puts("DMA/HDMA cancellation, channel startup, aliases, and CPU resumption passed");
}
