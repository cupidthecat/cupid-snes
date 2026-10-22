#include "snes/core/Sa1.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <vector>

using snes::core::Sa1;

namespace {

void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Set(Sa1 &chip, uint32_t address, uint32_t value, unsigned bytes = 2) {
  for (unsigned i = 0; i < bytes; ++i)
    chip.WriteSa1(address + i, static_cast<uint8_t>(value >> (i * 8)));
}

void SetCpu(Sa1 &chip, uint32_t address, uint32_t value, unsigned bytes = 2) {
  for (unsigned i = 0; i < bytes; ++i)
    chip.WriteCpu(address + i, static_cast<uint8_t>(value >> (i * 8)));
}

uint64_t Get(Sa1 &chip, uint32_t address, unsigned bytes = 2) {
  uint64_t value = 0;
  for (unsigned i = 0; i < bytes; ++i)
    value |= uint64_t(chip.ReadSa1(address + i)) << (i * 8);
  return value;
}

struct Fixture {
  std::vector<uint8_t> rom;
  std::vector<uint8_t> ram;
  Sa1 chip;

  explicit Fixture(size_t romSize = 0x400000, size_t ramSize = 0x40000)
      : rom(romSize, 0xea), ram(ramSize), chip(rom, ram) {}

  void Code(uint16_t address, std::initializer_list<uint8_t> bytes) {
    std::copy(bytes.begin(), bytes.end(), rom.begin() + address - 0x8000);
  }

  void Start(uint16_t address = 0x8000) {
    SetCpu(chip, 0x2203, address);
    chip.WriteCpu(0x2200, 0);
  }
};

void DeferredExecution() {
  for (uint32_t address :
       {0x7e1000u, 0x008000u, 0xc00000u, 0x503000u, 0x600000u, 0x002200u, 0x8023ffu, 0x003000u,
        0xbf37ffu, 0x006000u, 0xbf7fffu, 0x400000u, 0x4fffffu}) {
    Fixture f;
    f.Code(0x8000, {0xa9, 0x5a, 0x8d, 0, 0x30, 0xdb});
    f.Start();
    f.chip.WriteSa1(0x222a, 0xff);
    f.chip.SetBusContention(false);
    f.chip.Advance(128, address, false, false);
    Check(f.chip.ReadSa1(0x3000) == 0, "Charging clocks does not execute the deferred processor");
    f.chip.ReadCpu(address);
    const bool shared = address == 0x002200 || address == 0x8023ff || address == 0x003000 ||
                        address == 0xbf37ff || address == 0x006000 || address == 0xbf7fff ||
                        address == 0x400000 || address == 0x4fffff;
    Check(f.chip.ReadSa1(0x3000) == (shared ? 0x5a : 0),
          "Deferred execution resumes for CPU shared memory and registers only");
    Check(f.chip.MasterClocks() == 128, "Deferring execution retains every requested clock");
    f.chip.Advance(0, 0x7e1000, false, true);
    Check(f.chip.ReadSa1(0x3000) == 0x5a && f.chip.MasterClocks() == 128,
          "A forced handoff consumes retained debt without adding time");
  }
  for (uint32_t address : {0x003001u, 0x006000u, 0x400000u}) {
    Fixture f;
    f.Code(0x8000, {0xa9, 0x5a, 0x8d, 0, 0x30, 0xdb});
    f.Start();
    f.chip.WriteSa1(0x222a, 0xff);
    f.chip.SetBusContention(false);
    f.chip.Advance(128, 0x7e1000, false, false);
    f.chip.WriteCpu(address, 0);
    Check(f.chip.ReadSa1(0x3000) == 0x5a,
          "A mapped write consumes debt independently of the last CPU address");
  }
  for (bool contention : {false, true}) {
    Fixture f;
    f.Code(0x8000, {0xa9, 0x5a, 0x8d, 0, 0x30, 0xdb});
    f.Start();
    f.chip.WriteSa1(0x222a, 0xff);
    f.chip.SetBusContention(contention);
    f.chip.Advance(128, 0x7e1000, false, contention ? false : true);
    Check(f.chip.ReadSa1(0x3000) == 0x5a,
          "Standalone advancement and contended execution retain immediate handoffs");
  }
}

void ResetControlFlow() {
  for (bool native : {false, true}) {
    for (uint8_t opcode : {uint8_t{0}, uint8_t{2}}) {
      const unsigned prefix = native ? 8 : 0;
      const unsigned highRead = native ? 15 : 13;
      for (unsigned resetAt : {highRead, highRead + 1}) {
        Fixture f;
        if (native)
          f.Code(0x8000, {0x18, 0xfb, opcode, 0xea});
        else
          f.Code(0x8000, {opcode, 0xea});
        const unsigned vector = native ? (opcode ? 0x7fe4 : 0x7fe6) : (opcode ? 0x7ff4 : 0x7ffe);
        f.rom[vector] = 0x34;
        f.rom[vector + 1] = 0x12;
        f.Start();
        f.chip.Advance(prefix + resetAt);
        SetCpu(f.chip, 0x2203, 0x9000);
        f.chip.WriteCpu(0x2200, 0x20);
        f.chip.WriteCpu(0x2200, 0);
        f.chip.Advance(highRead + 2 - resetAt);
        Check(f.chip.CpuRegisters().pc == 0x1201 && f.chip.CpuRegisters().mar == 0x1200,
              "A pending vector high read preserves the low PC byte written by reset release");
      }
    }
    for (unsigned resetAt : {7u, 8u}) {
      for (uint8_t opcode : {uint8_t{0x82}, uint8_t{0x62}}) {
        Fixture f;
        const unsigned prefix = native ? 8 : 0;
        if (native)
          f.Code(0x8000, {0x18, 0xfb, opcode, 0, 0});
        else
          f.Code(0x8000, {opcode, 0, 0});
        f.Start();
        f.chip.Advance(prefix + resetAt);
        SetCpu(f.chip, 0x2203, 0x9000);
        f.chip.WriteCpu(0x2200, 0x20);
        f.chip.WriteCpu(0x2200, 0);
        if (opcode == 0x82) {
          f.chip.Advance(11 - resetAt);
          Check(f.chip.CpuRegisters().pc == (native ? 0x8006 : 0x8004),
                "A long branch retains its relative target across the final idle cycle");
        } else {
          f.chip.Advance(11 - resetAt);
          Check(f.chip.CpuRegisters().mdr == 0,
                "A relative stack push samples the live program counter after the idle cycle");
        }
      }
    }
  }
  for (uint8_t opcode : {uint8_t{0x20}, uint8_t{0x22}, uint8_t{0xfc}}) {
    const unsigned highPush = opcode == 0x20 ? 9 : opcode == 0x22 ? 13 : 5;
    for (unsigned resetAt : {highPush, highPush + 1}) {
      Fixture f;
      f.Code(0x8000, {opcode, 0x10, 0x80, 0});
      f.Start();
      f.chip.Advance(resetAt);
      SetCpu(f.chip, 0x2203, 0x9000);
      f.chip.WriteCpu(0x2200, 0x20);
      f.chip.WriteCpu(0x2200, 0);
      f.chip.Advance(highPush + 2 - resetAt);
      Check(f.chip.CpuRegisters().mdr == 0,
            "A call samples the live return-address low byte after the high-byte push");
    }
  }
}

void BusPhases() {
  for (bool pal : {false, true})
    for (unsigned mode = 0; mode < 4; ++mode) {
      Fixture f;
      f.chip.SetPal(pal);
      f.chip.WriteCpu(0x2229, 0xff);
      for (unsigned offset = 0; offset < 2048; ++offset)
        f.chip.WriteCpu(0x3000 + offset, 0xea);
      f.chip.WriteCpu(0x3000, 0);
      const uint8_t code[] = {0xa9, 0x5a, 0x8d, 0, 0x30, 0xea};
      for (unsigned offset = 0; offset < sizeof(code); ++offset)
        f.chip.WriteCpu(0x3100 + offset, code[offset]);
      f.Start(0x100);
      f.chip.WriteSa1(0x222a, 0xff);
      for (unsigned master = 1; master <= 32; ++master) {
        const uint32_t console = mode == 1                                   ? 0xc00000
                                 : (mode == 2 || (mode == 3 && master <= 7)) ? 0x3000
                                                                             : 0;
        f.chip.Advance(1, console, mode == 2 && master >= 11 && master <= 12);
        const auto &r = f.chip.CpuRegisters();
        if (master <= 2)
          Check(r.pc == 0x101 && r.a == 0,
                "An opcode read retains its live stack until its bus phase completes");
        if (master == 4 && mode < 2)
          Check(r.pc == 0x102 && r.a == 0,
                "An immediate operand is consumed after its pending bus phase");
        if (master == 8 && mode == 3)
          Check(r.pc == 0x102 && r.a == 0,
                "Each IRAM conflict is checked again after the preceding bus phase");
        const unsigned visibleAt = mode == 2 ? 33 : mode == 3 ? 17 : 13;
        Check(f.chip.ReadCpu(0x3000) == (master < visibleAt ? 0 : 0x5a),
              "Shared IRAM stores publish only after all native bus phases complete");
      }
    }
}

void ExecutionLifecycle() {
  const auto prepare = [](Fixture &f, uint8_t value) {
    f.chip.WriteCpu(0x2229, 0xff);
    for (unsigned offset = 0; offset < 2048; ++offset)
      f.chip.WriteCpu(0x3000 + offset, 0xea);
    f.chip.WriteCpu(0x3000, 0);
    const uint8_t code[] = {0xa9, value, 0x8d, 0, 0x30, 0xea};
    for (unsigned offset = 0; offset < sizeof(code); ++offset)
      f.chip.WriteCpu(0x3100 + offset, code[offset]);
    f.Start(0x100);
    f.chip.WriteSa1(0x222a, 0xff);
  };
  {
    Fixture left, right;
    prepare(left, 0x5a);
    prepare(right, 0xa7);
    left.chip.Advance(4);
    right.chip.Advance(13);
    Check(left.chip.CpuRegisters().a == 0 && left.chip.ReadCpu(0x3000) == 0 &&
              right.chip.ReadCpu(0x3000) == 0xa7,
          "Separate SA-1 instances retain independent suspended stacks");
    left.chip.Advance(9);
    Check(left.chip.ReadCpu(0x3000) == 0x5a && right.chip.ReadCpu(0x3000) == 0xa7,
          "Resuming one cartridge cannot execute another cartridge's pending memory access");
  }
  {
    Fixture f;
    prepare(f, 0x5a);
    f.ram[0] = 0x46;
    f.chip.Advance(12);
    f.chip.Reset();
    f.chip.Advance(100);
    Check(f.chip.ReadCpu(0x3000) == 0 && f.ram[0] == 0x46,
          "Device reset cancels a pending store while retaining battery RAM");
    prepare(f, 0xa7);
    f.chip.Advance(13);
    Check(f.chip.ReadCpu(0x3000) == 0xa7, "Reset starts a fresh execution stack");
  }
  {
    Fixture f;
    prepare(f, 0x5a);
    f.chip.Advance(1);
    f.chip.WriteCpu(0x2200, 0x40);
    f.chip.Advance(100);
    Check(f.chip.CpuRegisters().pc == 0x102 && f.chip.CpuRegisters().a == 0x5a &&
              f.chip.ReadCpu(0x3000) == 0,
          "WAIT lets the live instruction finish before holding the following opcode");
    f.chip.WriteCpu(0x2200, 0);
    f.chip.Advance(100);
    Check(f.chip.ReadCpu(0x3000) == 0x5a, "WAIT release resumes the following instruction");
  }
  {
    Fixture f;
    prepare(f, 0x5a);
    f.chip.Advance(12);
    std::vector<uint8_t> rom(0x8000, 0xea), ram(0x2000);
    const uint8_t code[] = {0xa9, 0x99, 0x8f, 0, 1, 0x40, 0xdb};
    std::copy(std::begin(code), std::end(code), rom.begin());
    f.chip.Reset(rom, ram);
    f.Start();
    f.chip.WriteSa1(0x2227, 0x80);
    f.chip.Advance(100);
    Check(ram[0x100] == 0x99 && f.ram[0x100] == 0,
          "Replacing cartridge spans cancels the old live operation before attaching new memory");
    f.chip.Reset(f.rom, f.ram);
  }
  for (unsigned deadline : {1u, 3u, 5u, 11u, 12u, 13u}) {
    Fixture f;
    prepare(f, 0x5a);
    f.chip.Advance(deadline);
  }
}

void ResetControl() {
  Fixture f;
  f.Code(0x8000, {0xa9, 0xff, 0x8d, 0x2a, 0x22, 0xa9, 0x4f, 0x48, 0xab, 0x18, 0xfb,
                  0xc2, 0x30, 0xa9, 0x34, 0x12, 0x5b, 0xa2, 0x78, 0x56, 0xa0, 0xbc,
                  0x9a, 0xa9, 0x21, 0x43, 0x1b, 0xa9, 0xef, 0xbe, 0xdb});
  f.Start();
  f.chip.Advance(200);
  const auto before = f.chip.CpuRegisters();
  Check(before.stp && !before.e && before.a == 0xbeef && before.db == 0x4f,
        "The reset fixture enters native mode with live general registers");
  f.chip.WriteCpu(0x2200, 0x20);
  SetCpu(f.chip, 0x2203, 0x9000);
  f.chip.WriteCpu(0x2200, 0);
  const auto &after = f.chip.CpuRegisters();
  Check(after.pc == 0x9000 && !after.pb && !after.db && !after.stp,
        "SA-1 reset release selects its vector, clears banks and releases STOP");
  Check(after.a == before.a && after.x == before.x && after.y == before.y && after.s == before.s &&
            after.d == before.d && after.p == before.p && after.e == before.e,
        "SA-1 control reset retains general registers, stack and processor mode");
  f.chip.WriteSa1(0x300, 0x55);
  Check(f.chip.ReadCpu(0x3300) == 0, "SA-1 reset release clears its IRAM write permission");
}

void TransferTiming() {
  struct Case {
    std::array<uint8_t, 4> code;
    uint32_t target;
    std::array<unsigned, 3> clocks;
  };
  const Case cases[] = {
      {{0x80, 2}, 0x8004, {6, 10, 6}},
      {{0x80, 3}, 0x8005, {8, 14, 8}},
      {{0x82, 5, 0}, 0x8008, {8, 14, 8}},
      {{0x82, 6, 0}, 0x8009, {10, 18, 10}},
      {{0x4c, 0, 0x81}, 0x8100, {8, 16, 8}},
      {{0x4c, 0, 1}, 0x100, {6, 12, 6}},
      {{0x5c, 0, 0x81, 0xc0}, 0xc08100, {10, 20, 10}},
      {{0x5c, 0, 1, 0}, 0x100, {8, 16, 8}},
      {{0x20, 0, 0x81}, 0x8100, {14, 22, 22}},
      {{0x22, 0, 0x81, 0xc0}, 0xc08100, {18, 28, 30}},
      {{0x6c, 0, 2}, 0x8100, {12, 20, 20}},
      {{0x7c, 0, 2}, 0x8100, {14, 22, 22}},
      {{0xdc, 0, 2}, 0xc08100, {14, 22, 26}},
  };
  const std::array<uint32_t, 3> consoles{0, 0xc00000, 0x3000};
  const std::array<uint32_t, 13> deadlinePc{0x8002, 0x8005,   0x8003,  0x8009, 0x8100,
                                            0x8003, 0xc08100, 0x8004,  0x8100, 0xc08100,
                                            0x8100, 0x8100,   0xc08100};
  for (unsigned index = 0; index < consoles.size(); ++index)
    for (unsigned instruction = 0; instruction < std::size(cases); ++instruction) {
      const auto &test = cases[instruction];
      Fixture f;
      std::copy(test.code.begin(), test.code.end(), f.rom.begin());
      f.chip.WriteCpu(0x2229, 4);
      f.chip.WriteCpu(0x3200, 0);
      f.chip.WriteCpu(0x3201, 0x81);
      f.chip.WriteCpu(0x3202, 0xc0);
      f.Start();
      f.chip.Advance(test.clocks[index], consoles[index]);
      const auto &r = f.chip.CpuRegisters();
      Check(((uint32_t(r.pb) << 16) | r.pc) == deadlinePc[instruction],
            "SA-1 control transfers retain their pending operand, idle or prefetch phase");
      f.chip.Advance(1, consoles[index]);
      Check(((uint32_t(r.pb) << 16) | r.pc) == test.target + 1,
            "The next opcode fetch starts when the SA-1 transfer's final phase resumes");
    }
}

void MemoryTiming() {
  struct Console {
    uint32_t address;
    bool refresh;
    unsigned fetch;
  };
  const Console consoles[] = {{0, false, 8},     {0xc00000, false, 16}, {0x3000, false, 8},
                              {0x3000, true, 8}, {0x6000, false, 8},    {0x500000, false, 8}};
  struct Access {
    uint32_t address;
    std::array<unsigned, 6> clocks;
  };
  const Access accesses[] = {
      {0x000000, {2, 2, 6, 2, 2, 2}}, {0x8007ff, {2, 2, 6, 2, 2, 2}},
      {0x003000, {2, 2, 6, 2, 2, 2}}, {0xbf37ff, {2, 2, 6, 2, 2, 2}},
      {0x000800, {2, 2, 2, 2, 2, 2}}, {0x003800, {2, 2, 2, 2, 2, 2}},
      {0x002300, {2, 2, 2, 2, 2, 2}}, {0x7e3000, {2, 2, 2, 2, 2, 2}},
      {0x006000, {4, 4, 4, 4, 8, 4}}, {0xbf7fff, {4, 4, 4, 4, 8, 4}},
      {0x400000, {4, 4, 4, 4, 8, 4}}, {0x5fffff, {4, 4, 4, 4, 8, 4}},
      {0x600000, {4, 4, 4, 4, 8, 4}}, {0x6fffff, {4, 4, 4, 4, 8, 4}},
      {0x700000, {2, 2, 2, 2, 2, 2}}, {0x008000, {2, 4, 2, 2, 2, 2}},
      {0xbf8000, {2, 4, 2, 2, 2, 2}}, {0xc00000, {2, 4, 2, 2, 2, 2}},
  };
  for (bool pal : {false, true})
    for (bool contention : {false, true})
      for (uint8_t opcode : {0xafu, 0x8fu})
        for (unsigned index = 0; index < std::size(consoles); ++index)
          for (const auto &access : accesses) {
            Fixture f;
            f.Code(0x8000, {opcode, uint8_t(access.address), uint8_t(access.address >> 8),
                            uint8_t(access.address >> 16)});
            f.chip.SetPal(pal);
            f.chip.SetBusContention(contention);
            f.chip.Reset();
            f.Start();
            const auto &console = consoles[index];
            const auto clocks = contention ? console.fetch + access.clocks[index]
                                           : consoles[0].fetch + access.clocks[0];
            f.chip.Advance(clocks, console.address, console.refresh);
            Check(f.chip.CpuRegisters().pc == 0x8004,
                  "SA-1 memory cycles retain the selected contention mode across reset");
            f.chip.Advance(1, console.address, console.refresh);
            Check(f.chip.CpuRegisters().pc == 0x8005,
                  "SA-1 memory contention consumes only the required bus cycles");
          }
}

void HardwareInterruptTiming() {
  const std::array<uint32_t, 3> consoles{0, 0xc00000, 0x3000};
  const std::array<unsigned, 3> emulationClocks{10, 12, 22};
  const std::array<unsigned, 3> nativeClocks{12, 14, 28};
  for (bool native : {false, true})
    for (unsigned index = 0; index < consoles.size(); ++index) {
      Fixture f;
      if (native)
        f.Code(0x8000, {0x18, 0xfb, 0x58, 0xea, 0xea});
      else
        f.Code(0x8000, {0x58, 0xea, 0xea});
      f.chip.WriteSa1(0x220a, 0x80);
      SetCpu(f.chip, 0x2207, 0x8100);
      f.Start();
      f.chip.WriteSa1(0x222a, 0xff);
      if (native)
        f.chip.Advance(8);
      f.chip.Advance(4);
      f.chip.WriteCpu(0x2200, 0x80);
      f.chip.Advance(4);
      // Finish the preceding instruction's pending read before changing MAR.
      f.chip.Advance(1);
      f.chip.Advance((native ? nativeClocks[index] : emulationClocks[index]) - 1, consoles[index]);
      const auto &r = f.chip.CpuRegisters();
      Check(r.pc == (native ? 0x8004 : 0x8002) && r.s == 0x1ff - (native ? 3 : 2) &&
                (r.p & 0x0c) == 0,
            "SA-1 hardware entry retains its final status write at the native clock deadline");
      f.chip.Advance(1, consoles[index]);
      Check(r.pc == 0x8101 && !r.pb && r.s == 0x1ff - (native ? 4 : 3) && (r.p & 0x0c) == 4,
            "The handler starts after SA-1 hardware entry without vector-table read cycles");
    }
}

void RegistersAndProtection() {
  Fixture f;
  auto &c = f.chip;
  for (uint8_t openBus : {0, 0x5a, 0xa5, 0xff}) {
    Check(c.ReadCpu(0x230e, openBus) == openBus,
          "The unpopulated console version port retains open bus");
    for (uint16_t address = 0x2200; address < 0x2400; ++address) {
      if (address < 0x2301 || address > 0x230d)
        Check(c.ReadSa1(address, openBus) == 0xff,
              "Undecoded SA-1 I/O reads return the fixed bus value");
    }
  }
  Check(c.ReadCpu(0x2200, 0x6d) == 0x6d, "Console write-only registers retain open bus");
  c.WriteCpu(0x2209, 0x80);
  Check(c.ReadCpu(0x2300) == 0, "Console writes cannot assert their own cartridge IRQ");
  c.WriteSa1(0x2200, 0x80);
  Check(c.ReadSa1(0x2301) == 0, "SA-1 cannot write console control registers");
  c.WriteCpu(0x2200, 0x2b);
  c.WriteSa1(0x2209, 0x5a);
  Check(c.ReadSa1(0x2301) == 0x0b && c.ReadCpu(0x2300) == 0x5a,
        "Both four-bit mailboxes and vector control bits are visible");
  c.WriteSa1(0x2209, 0x80);
  Check(!c.CpuIrqPending() && (c.ReadCpu(0x2300) & 0x80),
        "A disabled console IRQ still latches its status flag");
  c.WriteCpu(0x2201, 0x80);
  Check(c.CpuIrqPending(), "Enabling an already latched console IRQ asserts the line");
  c.WriteCpu(0x2201, 0);
  Check(c.CpuIrqPending() && (c.ReadCpu(0x2300) & 0x80),
        "Masking an IRQ preserves its status flag and asserted line");
  c.WriteCpu(0x2202, 0x80);
  c.WriteCpu(0x2201, 0x80);
  Check(!c.CpuIrqPending(), "Acknowledging a masked IRQ prevents it from reappearing on enable");

  c.WriteCpu(0x3000, 0x11);
  c.WriteSa1(0, 0x22);
  Check(c.ReadCpu(0x3000) == 0, "IRAM powers up protected on both ports");
  c.WriteCpu(0x2229, 1);
  c.WriteCpu(0x8030ff, 0x31);
  c.WriteCpu(0x3100, 0x42);
  Check(c.ReadSa1(0x00ff) == 0x31 && c.ReadSa1(0x0100) == 0,
        "Console IRAM mask gates each 256-byte page");
  c.WriteSa1(0x222a, 2);
  c.WriteSa1(0x80100, 0x53);
  c.WriteSa1(0xff, 0x64);
  Check(c.ReadCpu(0x803100) == 0x53 && c.ReadCpu(0x30ff) == 0x31,
        "SA-1 IRAM mask is independent of the console mask");
  Check(c.ReadCpu(0, 0x9c) == 0x9c && c.ReadSa1(0x0800, 0x8d) == 0x8d &&
            c.ReadSa1(0x7e0000, 0x7e) == 0x7e,
        "CPU WRAM and IRAM overrun addresses remain unmapped on the SA-1 bus");

  c.WriteCpu(0x400000, 0x55);
  c.WriteSa1(0x400000, 0x66);
  Check(f.ram[0] == 0, "BW-RAM powers up protected");
  c.WriteCpu(0x2228, 0);
  c.WriteCpu(0x4000ff, 0x12);
  c.WriteCpu(0x400100, 0x34);
  Check(f.ram[0xff] == 0 && f.ram[0x100] == 0x34,
        "BW-RAM protected prefix has an exact 256-byte minimum");
  c.WriteCpu(0x2226, 0x80);
  c.WriteCpu(0x400000, 0x56);
  c.WriteSa1(0x400000, 0x78);
  Check(f.ram[0] == 0x78, "Either BW-RAM enable opens the shared write gate for both processors");
  c.WriteCpu(0x2226, 0);
  c.WriteSa1(0x2227, 0x80);
  c.WriteCpu(0x400000, 0x78);
  Check(f.ram[0] == 0x78 && c.ReadCpu(0x4c0000) == 0x78,
        "BW-RAM write enable and full-bank mirrors share cartridge save storage");
  c.Reset();
  Check(f.ram[0] == 0x78 && c.ReadCpu(0x3000) == 0, "Device reset preserves battery RAM");
}

void Mapping() {
  Fixture f(0x800000);
  auto &c = f.chip;
  for (size_t i = 0; i < f.rom.size(); ++i)
    f.rom[i] = static_cast<uint8_t>((i >> 20) * 17 + ((i >> 15) & 31));
  const std::array<uint8_t, 4> lowBanks{0x00, 0x20, 0x80, 0xa0};
  for (unsigned block = 0; block < 4; ++block) {
    const uint32_t low = (uint32_t(lowBanks[block]) << 16) | 0x8123;
    const uint32_t high = 0xc00123 + block * 0x100000;
    Check(c.ReadCpu(low) == block * 17 && c.ReadSa1(high) == block * 17,
          "Four default LoROM and HiROM windows");
    for (unsigned page = 0; page < 8; ++page) {
      c.WriteCpu(0x2220 + block, static_cast<uint8_t>(page));
      Check(c.ReadCpu(low) == block * 17 && c.ReadSa1(high) == page * 17,
            "HiROM-only bank change preserves the fixed LoROM window");
      c.WriteCpu(0x2220 + block, static_cast<uint8_t>(0x80 | page));
      Check(c.ReadCpu(low) == page * 17 && c.ReadSa1(low) == page * 17,
            "LoROM bank override is shared by both processors");
      Check(c.ReadCpu(high + 0xffff) == f.rom[page * 0x100000 + 0x10122],
            "Mapped HiROM offset survives a bank boundary");
    }
  }
  c.WriteSa1(0x2220, 0);
  Check(c.ReadCpu(0xc00000) == 119, "SA-1 cannot change console-owned MMC registers");
  c.WriteCpu(0x2226, 0x80);
  c.WriteSa1(0x2227, 0x80);
  c.WriteCpu(0x2224, 3);
  c.WriteSa1(0x2225, 5);
  c.WriteCpu(0x006123, 0x39);
  c.WriteSa1(0x006123, 0x57);
  Check(f.ram[0x6123] == 0x39 && f.ram[0xa123] == 0x57, "Independent raw BW-RAM windows");

  for (uint8_t depth : {2, 4}) {
    c.WriteSa1(0x223f, depth == 2 ? 0x80 : 0);
    const unsigned perByte = 8 / depth;
    const unsigned mask = (1u << depth) - 1;
    std::fill(f.ram.begin(), f.ram.end(), 0xa5);
    c.WriteSa1(0x600001, 0xff);
    const uint8_t expected = static_cast<uint8_t>((0xa5 & ~(mask << depth)) | (mask << depth));
    Check(f.ram[0] == expected, "Bitmap writes mask one pixel without disturbing neighbors");
    Check(c.ReadSa1(0x600001) == mask && c.ReadSa1(0x600000) == (0xa5 & mask),
          "Bitmap reads zero-extend individual pixels");
    c.WriteSa1(0x2225, 0x83);
    c.WriteSa1(0x006123, 9);
    const unsigned byte = 3 * 0x800 + 0x123 / perByte;
    Check(((f.ram[byte] >> ((0x123 % perByte) * depth)) & mask) == (9 & mask),
          "Bitmap window selects fixed two-kilobyte physical pages");
    const uint32_t alias = 0x600000 + 3 * 0x800 * perByte + 0x123;
    Check(c.ReadSa1(alias) == (9 & mask), "Bitmap window aliases the matching full pixel address");
    c.WriteSa1(0x2227, 0);
    c.WriteCpu(0x2226, 0);
    c.WriteCpu(0x2228, 15);
    const auto old = f.ram[0];
    c.WriteSa1(0x400000, 0);
    Check(f.ram[0] == old, "Linear BW-RAM writes are protected when both enables are clear");
    c.WriteSa1(0x600000, 0);
    Check(f.ram[0] == (old & ~mask),
          "Bitmap pixel writes use their own port and bypass linear protection");
    c.WriteSa1(0x2227, 0x80);
  }

  Fixture odd(0x300000, 0x2000);
  std::fill_n(odd.rom.begin(), 0x100000, 0x11);
  std::fill_n(odd.rom.begin() + 0x100000, 0x100000, 0x22);
  std::fill_n(odd.rom.begin() + 0x200000, 0x100000, 0x33);
  Check(odd.chip.ReadCpu(0xf00000) == 0x33,
        "Three-megabyte ROM mirrors its last address-line branch");
  odd.chip.WriteCpu(0x2226, 0x80);
  odd.chip.WriteCpu(0x43ffff, 0x8a);
  Check(odd.ram[0x1fff] == 0x8a, "Small physical BW-RAM mirrors safely");
  Sa1 empty({}, {});
  Check(empty.ReadCpu(0xc00000, 0x91) == 0x91 && empty.ReadSa1(0x600000, 0x82) == 0x82,
        "Absent memory stays open bus");

  f.ram[0] = 0xa6;
  Check(c.ReadSa1(0x500000) == 0xa6 && c.ReadCpu(0x500000, 0x59) == 0x59,
        "The upper linear BW-RAM bank range is available only to the cartridge processor");
  c.WriteSa1(0x53ffff, 0x7b);
  Check(f.ram[0x3ffff] == 0x7b, "Upper SA-1 linear banks share physical BW-RAM and protection");
}

void Arithmetic() {
  Fixture f;
  auto &c = f.chip;
  constexpr uint64_t mask = (uint64_t{1} << 40) - 1;
  const std::array<uint16_t, 8> values{0, 1, 2, 0x100, 0x7fff, 0x8000, 0xfffe, 0xffff};
  for (unsigned mode : {0u, 1u}) {
    c.WriteSa1(0x2250, static_cast<uint8_t>(mode));
    for (uint16_t a : values)
      for (uint16_t b : values) {
        Set(c, 0x2251, a);
        Set(c, 0x2253, b);
        const int64_t signedA = a < 0x8000 ? a : int64_t(a) - 65536;
        const int64_t signedB = b < 0x8000 ? b : int64_t(b) - 65536;
        uint64_t expected = 0;
        if (!mode)
          expected = static_cast<uint32_t>(signedA * signedB);
        else if (b) {
          const auto quotient = static_cast<int64_t>(std::floor(double(signedA) / b));
          const int64_t remainder = signedA - quotient * b;
          expected = uint16_t(quotient) | (uint64_t(remainder) << 16);
          if (expected & 0x80000000)
            expected |= uint64_t{0xff} << 32;
        }
        Check(Get(c, 0x2306, 5) == expected,
              "Arithmetic publishes the complete result when the high operand byte is written");
      }
  }
  struct DivisionCase {
    uint16_t dividend, divisor, quotient, remainder;
  };
  for (const auto test :
       {DivisionCase{0x87f8, 0xfffb, 0xffff, 0x87f3}, DivisionCase{0x8000, 0x492a, 0xfffe, 0x1254},
        DivisionCase{0x87f8, 0x0000, 0x0000, 0x0000},
        DivisionCase{0x8000, 0x0000, 0x0000, 0x0000}}) {
    c.WriteSa1(0x2250, 1);
    Set(c, 0x2251, test.dividend);
    Set(c, 0x2253, test.divisor);
    Check(Get(c, 0x2306, 4) == (uint64_t(test.remainder) << 16 | test.quotient),
          "Division rounds down with a nonnegative remainder and returns zero for divisor zero");
  }
  c.WriteSa1(0x2250, 0);
  Set(c, 0x2251, 3);
  Set(c, 0x2253, 7);
  Check(Get(c, 0x2306, 5) == 21, "Multiplication publishes its result at the trigger write");
  Set(c, 0x2253, 11);
  Check(Get(c, 0x2306, 5) == 33, "Multiplication retains the multiplicand for the next operation");
  c.WriteSa1(0x2254, 0);
  Check(Get(c, 0x2306, 5) == 0, "Multiplier is consumed by the preceding operation");
  c.WriteSa1(0x2250, 2);
  Set(c, 0x2251, 0x8000);
  for (unsigned i = 0; i < 1024; ++i) {
    Set(c, 0x2253, 0x8000);
    Check(c.ReadSa1(0x230b) == (i == 1023 ? 0x80 : 0),
          "Accumulation reports carry beyond the full forty-bit result");
    if (i == 511)
      Check(Get(c, 0x2306, 5) == (uint64_t{1} << 39),
            "Bit thirty-nine remains part of the accumulator value");
  }
  Check(Get(c, 0x2306, 5) == 0, "Accumulation wraps after forty bits");
  c.WriteSa1(0x2250, 2);
  Check(c.ReadSa1(0x230b) == 0x80, "Clearing the result preserves the overflow latch");
  Set(c, 0x2251, 0xffff);
  Set(c, 0x2253, 1);
  Check(Get(c, 0x2306, 5) == mask && c.ReadSa1(0x230b) == 0x80,
        "A negative accumulated product retains its low forty bits and reports unsigned underflow");
  c.WriteSa1(0x2250, 0);
  Set(c, 0x2251, 3);
  Set(c, 0x2253, 7);
  Check(Get(c, 0x2306, 5) == 21 && c.ReadSa1(0x230b) == 0x80,
        "Ordinary multiplication preserves the accumulator's overflow latch");
  c.WriteSa1(0x2250, 3);
  Set(c, 0x2251, 1);
  Set(c, 0x2253, 1);
  Check(Get(c, 0x2306, 5) == 1 && c.ReadSa1(0x230b) == 0,
        "A subsequent accumulation refreshes overflow in either accumulate mode");
}

void VariableBits() {
  Fixture f;
  auto &c = f.chip;
  constexpr unsigned source = 0xfffd;
  for (unsigned i = 0; i < 160; ++i)
    f.rom[source + i] = static_cast<uint8_t>(i * 59 + 0x93);
  const auto expected = [&](unsigned bit) {
    uint16_t result = 0;
    for (unsigned i = 0; i < 16; ++i) {
      const unsigned at = bit + i;
      result |= ((f.rom[source + at / 8] >> (at % 8)) & 1) << i;
    }
    return result;
  };
  c.WriteSa1(0x2258, 0x85);
  Set(c, 0x2259, 0xc0fffd, 3);
  Check(c.ReadSa1(0x230c) == uint8_t(expected(0)) && c.ReadSa1(0x230c) == uint8_t(expected(0)) &&
            Get(c, 0x230c) == expected(0) && Get(c, 0x230c) == expected(5),
        "Odd byte addresses and address writes retain automatic mode");
  for (unsigned length = 1; length <= 16; ++length) {
    c.WriteSa1(0x2258, static_cast<uint8_t>(length & 15));
    Set(c, 0x2259, 0xc0fffd, 3);
    for (unsigned bit = 0; bit < 160; bit += length) {
      Check(Get(c, 0x230c) == expected(bit),
            "Manual variable-bit reads cross both word and ROM-bank boundaries");
      Check(Get(c, 0x230c) == expected(bit), "Manual port reads do not consume bits");
      c.WriteSa1(0x2258, static_cast<uint8_t>(length & 15));
    }
    c.WriteSa1(0x2258, static_cast<uint8_t>(0x80 | (length & 15)));
    Set(c, 0x2259, 0xc0fffd, 3);
    for (unsigned bit = 0; bit < 160; bit += length) {
      Check(c.ReadSa1(0x230c) == uint8_t(expected(bit)) &&
                c.ReadSa1(0x230c) == uint8_t(expected(bit)),
            "Low data reads do not advance the automatic stream");
      Check(Get(c, 0x230c) == expected(bit),
            "Automatic stream advances after returning the high byte");
    }
  }

  Fixture mapped(0x800000);
  mapped.rom[0x0123] = 0x34;
  mapped.rom[0x0124] = 0x12;
  mapped.rom[0x8123] = 0x78;
  mapped.rom[0x8124] = 0x56;
  mapped.rom[0x400123] = 0xbc;
  mapped.rom[0x400124] = 0x9a;
  Set(mapped.chip, 0x2259, 0x000123, 3);
  Check(Get(mapped.chip, 0x230c) == 0, "Low bit-reader addresses select IRAM");
  Set(mapped.chip, 0x2259, 0x010123, 3);
  Check(Get(mapped.chip, 0x230c) == 0, "Low IRAM bit-reader windows retain bank mirrors");
  Set(mapped.chip, 0x2259, 0x3f7123, 3);
  Check(Get(mapped.chip, 0x230c) == 0, "The low BW-RAM bit-reader window reads physical RAM");
  Set(mapped.chip, 0x2259, 0x418123, 3);
  Check(Get(mapped.chip, 0x230c) == 0, "Linear BW-RAM bit-reader banks retain RAM decoding");
  mapped.chip.WriteCpu(0x2220, 0x84);
  Set(mapped.chip, 0x2259, 0x008123, 3);
  Check(Get(mapped.chip, 0x230c) == 0x9abc,
        "Bit reader follows Super MMC projection for LoROM windows");
  Set(mapped.chip, 0x2259, 0xc00123, 3);
  Check(Get(mapped.chip, 0x230c) == 0x9abc,
        "Bit reader follows Super MMC projection for HiROM banks");
}

void BitReaderWindows() {
  Fixture f;
  auto &c = f.chip;
  f.rom[0x120] = 0x56;
  f.rom[0x121] = 0x78;
  f.ram[0x6120] = 0xbc;
  f.ram[0x6121] = 0x9a;
  c.WriteSa1(0x222a, 0xff);
  Set(c, 0x120, 0x3412);
  c.WriteSa1(0x2225, 0x85);
  for (uint32_t address : {0x000120u, 0x013120u, 0x800120u}) {
    Set(c, 0x2259, address, 3);
    Check(Get(c, 0x230c) == 0x3412, "The bit reader uses both IRAM windows and bank mirrors");
  }
  for (uint32_t address : {0x006120u, 0x806120u, 0x406120u}) {
    Set(c, 0x2259, address, 3);
    Check(Get(c, 0x230c) == 0x9abc,
          "Bit reads use physical BW-RAM addresses independently of its processor window mode");
  }
  for (uint32_t address : {0x000820u, 0x500120u, 0x600120u, 0x7e0120u}) {
    Set(c, 0x2259, address, 3);
    Check(Get(c, 0x230c) == 0xffff, "Unmapped bit-reader windows return the fixed bus value");
  }
  for (uint32_t address : {0x008120u, 0xc00120u}) {
    Set(c, 0x2259, address, 3);
    Check(Get(c, 0x230c) == 0x7856, "Bit reads retain ROM projection on both cartridge windows");
  }
}

void BitReaderLiveState() {
  Fixture f;
  auto &c = f.chip;
  c.WriteSa1(0x222a, 0xff);
  Set(c, 0x120, 0x1234);
  Set(c, 0x2259, 0x120, 3);
  Check(Get(c, 0x230c) == 0x1234, "A bit read samples shared IRAM");
  Set(c, 0x120, 0xabcd);
  Check(Get(c, 0x230c) == 0xabcd, "A repeated bit read observes live IRAM writes");
  Set(c, 0x2259, 0x406120, 3);
  f.ram[0x6120] = 0x12;
  f.ram[0x6121] = 0x34;
  Check(Get(c, 0x230c) == 0x3412, "A bit read observes live BW-RAM writes");
  f.ram[0x6121] = 0x56;
  Check(Get(c, 0x230c) == 0x5612, "BW-RAM changes do not need a bit-address rewrite");
  f.rom[0x120] = 0x78;
  f.rom[0x121] = 0x56;
  f.rom[0x100120] = 0xbc;
  f.rom[0x100121] = 0x9a;
  Set(c, 0x2259, 0xc00120, 3);
  Check(Get(c, 0x230c) == 0x5678, "A bit read uses the current ROM bank");
  c.WriteCpu(0x2220, 1);
  Check(Get(c, 0x230c) == 0x9abc, "A ROM bank change takes effect on the next bit read");

  Fixture partial;
  auto &p = partial.chip;
  partial.rom[0x121] = 0x66;
  partial.rom[0x122] = 0x77;
  partial.rom[0x123] = 0x88;
  partial.rom[0x221] = 0xab;
  partial.rom[0x222] = 0xcd;
  partial.rom[0x223] = 0xef;
  partial.rom[0x10221] = 0xf1;
  partial.rom[0x10222] = 0xe2;
  partial.rom[0x10223] = 0xd3;
  Set(p, 0x2259, 0xc00120, 3);
  p.WriteSa1(0x2258, 3);
  p.WriteSa1(0x2259, 0x21);
  Check(Get(p, 0x230c) == ((0x887766 >> 3) & 0xffff),
        "Writing the low bit-address byte preserves its sub-byte offset");
  p.WriteSa1(0x225a, 2);
  Check(Get(p, 0x230c) == ((0xefcdab >> 3) & 0xffff),
        "Writing the middle bit-address byte updates the live cursor");
  p.WriteSa1(0x2258, 0x85);
  p.WriteSa1(0x225b, 0xc1);
  Check(p.ReadSa1(0x230c) == 0xf1 && p.ReadSa1(0x230d) == 0xe2 &&
            p.ReadSa1(0x230c) == ((0xd3e2f1 >> 5) & 0xff),
        "Writing the high address byte resets the bit offset and retains automatic mode");
}

void NativeTimerEdges() {
  for (bool pal : {false, true})
    for (uint8_t mode : {0, 0x80}) {
      Fixture f;
      auto &c = f.chip;
      c.SetPal(pal);
      c.WriteSa1(0x2210, mode);
      c.Advance(20);
      Set(c, 0x2214, 0);
      c.WriteSa1(0x2210, mode | 2);
      c.Advance(1);
      Check(!(c.ReadSa1(0x2301) & 0x40),
            "Enabling a vertical compare inside a line waits for horizontal counter zero");
    }
}

void Timers() {
  {
    Fixture f;
    auto &c = f.chip;
    Set(c, 0x2212, 3);
    c.WriteSa1(0x2210, 1);
    c.Advance(12);
    Check(!(c.ReadSa1(0x2301) & 0x40), "H compare waits for its native timer step to resume");
    c.Advance(1);
    Check(c.ReadSa1(0x2301) & 0x40, "H compare publishes at master clock 13 for dot three");
    c.WriteSa1(0x220b, 0x40);
    c.Advance(2);
    Check(!(c.ReadSa1(0x2301) & 0x40), "Acknowledgment does not reassert halfway through a dot");
    c.Advance(1364);
    Check(c.ReadSa1(0x2301) & 0x40, "H compare repeats on the next scanline");
  }
  {
    Fixture f;
    auto &c = f.chip;
    Set(c, 0x2214, 1);
    c.WriteSa1(0x2210, 2);
    c.Advance(1364);
    Check(!(c.ReadSa1(0x2301) & 0x40), "V-only compare waits for the completed line");
    c.Advance(1);
    Check(c.ReadSa1(0x2301) & 0x40, "V-only compare fires at horizontal counter zero");
    c.WriteSa1(0x220b, 0x40);
    c.Advance(100);
    Check(!(c.ReadSa1(0x2301) & 0x40), "V-only IRQ stays acknowledged for the rest of the line");
  }
  {
    Fixture f;
    auto &c = f.chip;
    Set(c, 0x2212, 2);
    Set(c, 0x2214, 1);
    c.WriteSa1(0x2210, 3);
    c.Advance(1372);
    Check(!(c.ReadSa1(0x2301) & 0x40), "Combined H/V compare requires both coordinates");
    c.Advance(1);
    Check(c.ReadSa1(0x2301) & 0x40, "Combined H/V compare publishes the matching native edge");
  }
  {
    Fixture f;
    auto &c = f.chip;
    c.WriteSa1(0x2210, 0x80);
    c.Advance(511 * 4 + 1);
    Check(Get(c, 0x2302) == 511 && Get(c, 0x2304) == 0,
          "Linear counters expose all nine horizontal bits");
    c.Advance(4);
    Check(Get(c, 0x2304) == 0 && c.ReadSa1(0x2303) == 1,
          "Counter high bytes stay latched across a rollover");
    Check(c.ReadSa1(0x2302) == 0 && Get(c, 0x2304) == 1,
          "Reading H low atomically refreshes the H/V latch");
    c.Advance(511 * 2048);
    Check(c.ReadSa1(0x2302) == 0 && Get(c, 0x2304) == 0,
          "Linear counters wrap their nine-bit vertical coordinate");
  }
  for (bool pal : {false, true})
    for (uint8_t mode : {0, 0x80}) {
      Fixture whole, pieces;
      for (auto *f : {&whole, &pieces}) {
        f->chip.SetPal(pal);
        f->chip.WriteSa1(0x2210, mode);
      }
      const uint32_t width = mode ? 2048 : 1364;
      const uint32_t height = mode ? 512 : pal ? 312 : 262;
      const uint32_t total = width * height * 2 + 3;
      whole.chip.Advance(total);
      uint32_t remaining = total;
      unsigned next = 1;
      while (remaining) {
        const auto clocks = std::min(remaining, next);
        pieces.chip.Advance(clocks);
        remaining -= clocks;
        next = (next * 7) % 251 + 1;
      }
      const auto published = ((total - 1) / 2) * 2;
      for (auto *f : {&whole, &pieces}) {
        Check(Get(f->chip, 0x2302) == (published % width) / 4 &&
                  Get(f->chip, 0x2304) == (published / width) % height,
              "Native counters retain exact regional wraps across odd scheduler partitions");
      }
      Fixture invalid;
      invalid.chip.SetPal(pal);
      invalid.chip.WriteSa1(0x2210, mode | 1);
      Set(invalid.chip, 0x2212, 512);
      invalid.chip.Advance(width * height + 1);
      Check(!(invalid.chip.ReadSa1(0x2301) & 0x40),
            "Out-of-range timer targets never alias a valid nine-bit coordinate");
    }
  {
    Fixture f;
    f.chip.WriteSa1(0x2210, 0x80);
    f.chip.Advance(2021);
    f.chip.WriteSa1(0x2210, 0);
    Check(Get(f.chip, 0x2302) == 505, "Changing timer mode preserves the live counter");
    f.chip.Advance(2);
    Check(Get(f.chip, 0x2302) == 0 && Get(f.chip, 0x2304) == 1,
          "The next HV step resets a counter beyond its line limit");
  }
}

void ConfigureDma(Sa1 &c, uint8_t control, uint32_t source, uint32_t destination, uint16_t length) {
  c.WriteSa1(0x2230, control);
  Set(c, 0x2232, source, 3);
  Set(c, 0x2238, length);
  Set(c, 0x2235, destination, (control & 4) ? 3 : 2);
}

void NativeDmaTiming() {
  const unsigned edges[4][5][2] = {
      {{3, 5}, {5, 9}, {7, 13}, {3, 5}, {5, 7}},
      {{5, 9}, {5, 9}, {5, 9}, {9, 17}, {9, 13}},
      {{5, 9}, {5, 9}, {7, 13}, {9, 17}, {9, 13}},
      {{5, 9}, {5, 9}, {7, 13}, {9, 17}, {9, 13}},
  };
  for (unsigned kind = 0; kind < 4; ++kind)
    for (unsigned owner = 0; owner < 5; ++owner) {
      Fixture f;
      f.rom[0x100] = 0x51;
      f.rom[0x101] = 0x52;
      f.ram[0x200] = 0x61;
      f.ram[0x201] = 0x62;
      f.chip.WriteCpu(0x2229, 0xff);
      f.chip.WriteCpu(0x3400, 0x71);
      f.chip.WriteCpu(0x3401, 0x72);
      const bool bwram = kind == 1 || kind == 3;
      const uint8_t control = kind == 0 ? 0x80 : kind == 1 ? 0x84 : kind == 2 ? 0x81 : 0x86;
      const uint32_t source = kind < 2 ? 0xc00100 : kind == 2 ? 0x200 : 0x400;
      ConfigureDma(f.chip, control, source, bwram ? 0x1000 : 0x600, 2);
      for (unsigned master = 1; master <= 24; ++master) {
        const auto console = owner == 1   ? 0xc00000
                             : owner == 2 ? 0x3000
                             : owner == 3 ? 0x400000
                             : owner == 4 ? (master <= 4   ? 0x3000
                                             : master <= 8 ? 0x400000
                                                           : 0)
                                          : 0;
        f.chip.Advance(1, console);
        const auto value = [&](unsigned offset) {
          return bwram ? f.ram[0x1000 + offset] : f.chip.ReadSa1(0x600 + offset);
        };
        const uint8_t first = kind < 2 ? 0x51 : kind == 2 ? 0x61 : 0x71;
        Check(value(0) == (master < edges[kind][owner][0] ? 0 : first) &&
                  value(1) == (master < edges[kind][owner][1] ? 0 : first + 1),
              "DMA data publication retains native memory phases and rechecks changing ownership");
        Check(bool(f.chip.ReadSa1(0x2301) & 0x20) == (master >= edges[kind][owner][1]),
              "DMA completion IRQ follows the final memory store");
      }
    }
}

void DmaLiveRegisters() {
  {
    Fixture f;
    auto &c = f.chip;
    f.rom[0x100] = 0x51;
    f.rom[0x101] = 0x62;
    f.rom[0x140] = 0x73;
    ConfigureDma(c, 0x80, 0xc00100, 0x600, 2);
    c.Advance(1);
    c.WriteCpu(0x2232, 0x40);
    c.WriteCpu(0x2235, 0x70);
    c.Advance(4);
    Check(c.ReadSa1(0x600) == 0x51 && c.ReadSa1(0x670) == 0x73 && c.ReadSa1(0x601) == 0,
          "DMA latches the current byte and observes address writes for the following byte");
  }
  {
    Fixture f;
    auto &c = f.chip;
    for (unsigned i = 0; i < 4; ++i)
      f.rom[0x100 + i] = static_cast<uint8_t>(0x51 + i);
    ConfigureDma(c, 0x80, 0xc00100, 0x600, 2);
    c.Advance(1);
    c.WriteSa1(0x2238, 3);
    c.Advance(8);
    Check(Get(c, 0x600, 4) == 0x54535251 && !c.DmaActive(),
          "An untimed count write during DMA controls the bytes after the current byte");
    c.WriteSa1(0x220b, 0x20);
    c.WriteSa1(0x2236, 6);
    c.Advance(1);
    Check(c.DmaActive() && !(c.ReadSa1(0x2301) & 0x20),
          "Restarting without a count write uses the wrapped completion count");
  }
  {
    Fixture f;
    auto &c = f.chip;
    f.rom[0x801] = 0x73;
    f.rom[1] = 0x24;
    ConfigureDma(c, 0x86, 0x7ff, 0x1000, 2);
    c.Advance(9);
    c.WriteSa1(0x2230, 0x84);
    c.WriteSa1(0x2234, 0xc0);
    Set(c, 0x2238, 1);
    c.WriteSa1(0x2237, 0);
    c.Advance(8);
    Check(f.ram[0x1002] == 0x73,
          "A physical IRAM wrap retains the high bits of the DMA source cursor");
  }
  {
    Fixture f;
    auto &c = f.chip;
    f.rom[0x102] = 0x83;
    ConfigureDma(c, 0x80, 0xc00100, 0x7fe, 2);
    c.Advance(5);
    c.WriteSa1(0x2230, 0x84);
    Set(c, 0x2238, 1);
    c.WriteSa1(0x2237, 0);
    c.Advance(8);
    Check(f.ram[0x800] == 0x83 && f.ram[0] == 0,
          "A physical IRAM wrap retains the high bits of the DMA destination cursor");
  }
  {
    Fixture f;
    auto &c = f.chip;
    ConfigureDma(c, 0x80, 0xc00100, 0x600, 0);
    c.WriteSa1(0x220b, 0x20);
    c.WriteSa1(0x2236, 6);
    c.Advance(1);
    Check(c.DmaActive() && !(c.ReadSa1(0x2301) & 0x20),
          "A zero-length DMA wraps its count before a subsequent trigger");
  }
}

void Dma() {
  Fixture f;
  auto &c = f.chip;
  for (unsigned i = 0; i < 6; ++i)
    f.rom[0x100 + i] = static_cast<uint8_t>(0x51 + i);
  ConfigureDma(c, 0x80, 0xc00100, 0x7fd, 6);
  c.Advance(2);
  Check(c.DmaActive() && c.ReadSa1(0x7fd) == 0, "DMA waits for a complete byte cycle");
  c.Advance(1);
  Check(c.ReadSa1(0x7fd) == 0x51 && !(c.ReadSa1(0x2301) & 0x20),
        "DMA progressively transfers bytes before its completion IRQ");
  c.Advance(9);
  Check(c.DmaActive(), "Six ROM-to-IRAM bytes require twelve master clocks");
  c.Advance(1);
  for (unsigned i = 0; i < 6; ++i)
    Check(c.ReadSa1((0x7fd + i) & 0x7ff) == 0x51 + i,
          "DMA uses all 2KB of IRAM and wraps its destination");
  Check(!c.DmaActive() && (c.ReadSa1(0x2301) & 0x20) && !c.CpuIrqPending(),
        "Normal DMA raises the SA-1 completion flag only");

  c.WriteSa1(0x220b, 0x20);
  c.Reset();
  f.rom[0xffffe] = 0x71;
  f.rom[0xfffff] = 0x82;
  f.rom[0x300000] = 0x93;
  f.rom[0x300001] = 0xa4;
  c.WriteCpu(0x2221, 3);
  ConfigureDma(c, 0x80, 0xcffffe, 0x500, 4);
  c.Advance(9);
  Check(Get(c, 0x500, 4) == 0xa4938271,
        "DMA redecodes ROM mapping across a one-megabyte bank boundary");

  c.Reset();
  c.WriteSa1(0x222a, 0xff);
  Set(c, 0x7fe, 0x1234);
  Set(c, 0, 0x5678);
  ConfigureDma(c, 0x86, 0x7fe, 0x3ffff, 4);
  c.Advance(15);
  Check(c.DmaActive(), "BW-RAM transfers take four clocks per byte");
  c.Advance(2);
  Check(f.ram.back() == 0x34 && f.ram[0] == 0x12 && f.ram[1] == 0x78 && f.ram[2] == 0x56,
        "IRAM-to-BW-RAM DMA wraps both independent physical ports");
  c.Reset();
  ConfigureDma(c, 0x81, 0x3ffff, 0x700, 4);
  c.Advance(17);
  Check(Get(c, 0x700, 4) == 0x56781234, "BW-RAM-to-IRAM DMA reads shared save storage");

  c.WriteSa1(0x220b, 0x20);
  c.Reset();
  ConfigureDma(c, 0x80, 0xc00100, 0x600, 4);
  c.Advance(1);
  c.WriteSa1(0x2230, 0);
  c.Advance(20);
  Check(!c.DmaActive() && c.ReadSa1(0x600) == 0x51 && (c.ReadSa1(0x2301) & 0x20),
        "Disabling the DMA trigger retains an in-flight transfer and its completion interrupt");
  ConfigureDma(c, 0x82, 0, 0, 4);
  Check(!c.DmaActive(), "An unselected DMA port pair completes without memory cycles");
  ConfigureDma(c, 0x80, 0xc00100, 0x600, 0);
  Check(!c.DmaActive() && c.ReadSa1(0x600) == 0x51 && (c.ReadSa1(0x2301) & 0x20),
        "Zero-length DMA completes without copying memory");
}

std::vector<uint8_t> Planar(const std::array<uint8_t, 64> &pixels, unsigned depth) {
  std::vector<uint8_t> output(depth * 8);
  // Independent pixel-scatter oracle: each source pixel contributes one bit
  // to each output plane. The device itself gathers complete plane rows.
  for (unsigned pixel = 0; pixel < 64; ++pixel)
    for (unsigned plane = 0; plane < depth; ++plane) {
      if (pixels[pixel] & (1u << plane))
        output[(plane & ~1u) * 8 + (pixel / 8) * 2 + (plane & 1)] |=
            static_cast<uint8_t>(0x80 >> (pixel & 7));
    }
  return output;
}

void CharacterConversionVisibility() {
  for (unsigned mode = 0; mode < 4; ++mode)
    for (unsigned widthCode : {5u, 6u, 7u}) {
      Fixture f;
      auto &c = f.chip;
      const unsigned depth = mode < 2 ? 8 >> mode : 2;
      const uint8_t firstPixels = static_cast<uint8_t>(0xff << (8 - 8 / depth));
      c.WriteCpu(0x2229, 0xff);
      for (unsigned address = 0; address < 2048; ++address)
        c.WriteCpu(0x3000 + address, 0x5a);
      f.ram[0x1000 + 32 * depth] = 0xff;
      c.WriteSa1(0x2231, static_cast<uint8_t>(mode | (widthCode << 2)));
      Set(c, 0x2232, 0x1000, 3);
      Set(c, 0x2235, 0x703, 3);
      c.WriteSa1(0x2230, 0xb0);
      c.WriteSa1(0x2236, 7);
      Check(c.ReadCpu(0x3703) == 0x5a && c.ReadCpu(0x401001) == 0x5a,
            "Type-1 conversion waits for a character-boundary read before changing IRAM");
      Check(c.ReadCpu(0x401000) == 0 && c.ReadCpu(0x3705) == firstPixels &&
                c.ReadCpu(0x3702) == 0x5a,
            "Conversion preserves an unaligned IRAM destination and clamps character width");
      f.ram[0x1000] = 0xff;
      Check(c.ReadCpu(0x401000) == firstPixels && c.ReadCpu(0x3703) == firstPixels,
            "A repeated character-boundary read converts live bitmap data again");
      Check(c.ReadCpu(0x401000 + depth * 8) == 0 && c.ReadCpu(0x3703) == 0 &&
                c.ReadCpu(0x3703 + depth * 8) == 0x5a,
            "Each converted character uses the programmed IRAM buffer");
      c.WriteCpu(0x2235, 0x21);
      Check(c.ReadCpu(0x401000) == firstPixels && c.ReadCpu(0x3721) == firstPixels,
            "A partial destination write affects the next conversion");
      c.WriteSa1(0x2230, 0);
      f.ram[0x1000] = 0;
      Check(c.ReadCpu(0x401000) == 0 && c.ReadCpu(0x3721) == 0,
            "Disabling the conversion trigger retains its active read mapping");
      c.WriteCpu(0x2231, 0x80);
      f.ram[0x1000] = 0x37;
      Check(c.ReadCpu(0x401000) == 0x37 && c.ReadCpu(0x3721) == 0,
            "The conversion-end bit releases the read mapping without another IRAM write");
    }
}

void CharacterConversion() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    const unsigned depth = 8 >> mode;
    Fixture f;
    auto &c = f.chip;
    c.WriteSa1(0x2230, 0xa0);
    c.WriteSa1(0x2231, static_cast<uint8_t>(mode));
    Set(c, 0x2235, 0x780);
    for (unsigned tile = 0; tile < 3; ++tile) {
      std::array<uint8_t, 64> pixels{};
      for (unsigned i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>(i * 29 + tile * 47 + 13);
        c.WriteSa1(0x2240 + (i & 15), pixels[i]);
        if ((i & 7) == 7) {
          const auto partial = Planar(pixels, depth);
          const unsigned row = i / 8;
          for (unsigned plane = 0; plane < depth; ++plane) {
            const unsigned at = (plane / 2) * 16 + row * 2 + (plane & 1);
            Check(c.ReadCpu(0x3780 + (tile & 1) * depth * 8 + at) == partial[at],
                  "Type-2 conversion exposes each completed row before the tile is finished");
          }
        }
      }
      const auto expected = Planar(pixels, depth);
      for (unsigned i = 0; i < expected.size(); ++i)
        Check(c.ReadCpu(0x3000 + ((0x780 + (tile & 1) * expected.size() + i) & 0x7ff)) ==
                  expected[i],
              "Type-2 conversion matches independent planar pixels and alternates IRAM buffers");
    }
    c.WriteSa1(0x2230, 0);
    const auto previous = c.ReadCpu(0x3780);
    for (unsigned i = 0; i < 64; ++i)
      c.WriteSa1(0x2240 + (i & 15), 0);
    Check(c.ReadCpu(0x3780) == previous,
          "Disabled type-2 conversion leaves the output buffer intact");

    for (unsigned width : {1u, 2u, 8u}) {
      unsigned widthCode = 0;
      while ((1u << widthCode) != width)
        ++widthCode;
      c.WriteSa1(0x2230, 0xb0);
      c.WriteCpu(0x2231, static_cast<uint8_t>(mode | (widthCode << 2)));
      const unsigned source = 0x1000;
      std::vector<std::array<uint8_t, 64>> tiles(width * 2);
      for (unsigned tile = 0; tile < tiles.size(); ++tile) {
        for (unsigned i = 0; i < 64; ++i) {
          const uint8_t pixel =
              static_cast<uint8_t>((tile * 23 + i * 37 + 5) & ((1u << depth) - 1));
          tiles[tile][i] = pixel;
          const unsigned x = (tile % width) * 8 + (i & 7);
          const unsigned y = (tile / width) * 8 + i / 8;
          const unsigned bit = (y * width * 8 + x) * depth;
          const unsigned at = source + bit / 8;
          const unsigned mask = ((1u << depth) - 1) << (bit & 7);
          f.ram[at] = static_cast<uint8_t>((f.ram[at] & ~mask) | (pixel << (bit & 7)));
        }
      }
      c.WriteCpu(0x2201, 0x20);
      SetCpu(c, 0x2232, source, 3);
      SetCpu(c, 0x2235, 0x780);
      Check(c.CpuIrqPending() && (c.ReadCpu(0x2300) & 0x20),
            "Type-1 first-character notification reaches the console IRQ");
      c.WriteCpu(0x2202, 0x20);
      for (unsigned tile = 0; tile < tiles.size(); ++tile) {
        const auto expected = Planar(tiles[tile], depth);
        for (unsigned i = 0; i < expected.size(); ++i) {
          const unsigned address = source + tile * expected.size() + i;
          Check(c.ReadCpu(0x400000 + address) == expected[i],
                "Type-1 packed bitmap conversion respects depth, tile-row stride, and source base");
          Check(c.ReadSa1(0x400000 + address) == f.ram[address],
                "SA-1 retains raw BW-RAM access while the console sees converted tiles");
        }
      }
      c.WriteCpu(0x2224, 0);
      Check(c.ReadCpu(0x7000) == Planar(tiles[0], depth)[0],
            "Type-1 conversion is also visible through the console's 8KB BW-RAM window");
      c.WriteCpu(0x2231, 0x80);
      Check(c.ReadCpu(0x401000) == f.ram[0x1000],
            "Ending type-1 conversion restores raw console BW-RAM reads");
    }
  }
}

void ExecutableProgram() {
  Fixture f;
  auto &c = f.chip;
  f.Code(0x8000,
         {0xa9, 0xff, 0x8d, 0x2a, 0x22, // Enable SA-1 IRAM writes, including stack.
          0xa9, 0x80, 0x8d, 0x27, 0x22, // Enable shared BW-RAM writes.
          0x18, 0xfb, 0xc2, 0x30,       // Enter native mode, 16-bit A/X/Y.
          0xa2, 0x04, 0x00, 0xa9, 0x00, 0x00, 0x18, 0x69, 0x07, 0x00, 0xca, 0xd0, 0xfa, 0x8d, 0x10,
          0x00, 0x8d, 0x00, 0x60, 0x20, 0x40, 0x80, // JSR verifies stack ownership
                                                    // and 16-bit PHA/PLA.
          0xe2, 0x20, 0xa9, 0x8d, 0x8d, 0x09, 0x22, 0xdb});
  f.Code(0x8040, {0x48, 0xa9, 0xef, 0xbe, 0x8d, 0x02, 0x60, 0x68, 0x60});
  c.WriteCpu(0x2201, 0x80);
  c.Advance(1000);
  Check(!c.CpuRegisters().stp && c.CpuRegisters().pc == 0 && f.ram[0] == 0,
        "Power-on reset holds off instruction execution");
  f.Start();
  c.Advance(4096);
  Check(c.CpuRegisters().stp && !c.CpuRegisters().e && c.CpuRegisters().s == 0x1ff,
        "Native machine-code program runs to STP and balances its stack");
  Check(Get(c, 0x10) == 28 && f.ram[0] == 28 && f.ram[1] == 0 && f.ram[2] == 0xef &&
            f.ram[3] == 0xbe,
        "Machine-code loop, subroutine, 16-bit arithmetic, IRAM, and BW-RAM produce expected "
        "results");
  Check(c.CpuIrqPending() && (c.ReadCpu(0x2300) & 15) == 13,
        "SA-1 program sends its completion IRQ and four-bit message");
  c.WriteCpu(0x2202, 0x80);
  Check(!c.CpuIrqPending(), "Console acknowledges program completion");
  const auto stoppedPc = c.CpuRegisters().pc;
  c.WriteCpu(0x2200, 0x80);
  c.Advance(200);
  Check(c.CpuRegisters().stp && c.CpuRegisters().pc == stoppedPc,
        "A masked IRQ does not release STP");

  c.WriteCpu(0x2229, 0xff);
  const std::array<uint8_t, 7> iramProgram{0xa9, 0x6c, 0x8f, 0x00, 0x01, 0x40, 0xdb};
  for (unsigned i = 0; i < iramProgram.size(); ++i)
    c.WriteCpu(0x3300 + i, iramProgram[i]);
  c.WriteCpu(0x2200, 0x20);
  SetCpu(c, 0x2203, 0x300);
  c.WriteCpu(0x2200, 0);
  c.Advance(100);
  Check(c.CpuRegisters().stp && !c.CpuRegisters().e && f.ram[0x100] == 0x6c,
        "Reset releases STP, retains CPU mode, and executes a program from low IRAM");
}

void Interrupts() {
  Fixture f;
  auto &c = f.chip;
  f.Code(0x8000, {0xa9, 0xff, 0x8d, 0x2a, 0x22, 0xa9, 0x90, 0x8d, 0x0b, 0x22, 0x8d, 0x0a, 0x22,
                  0x58, 0xcb, 0xe6, 0x10, 0xdb});
  f.Code(0x8100, {0xe6, 0x20, 0xa9, 0xe0, 0x8d, 0x0b, 0x22, 0x40});
  f.Code(0x8200, {0xe6, 0x21, 0xa9, 0x10, 0x8d, 0x0b, 0x22, 0x40});
  SetCpu(c, 0x2205, 0x8200);
  SetCpu(c, 0x2207, 0x8100);
  f.Start();
  c.Advance(100);
  Check(c.CpuRegisters().wai && c.ReadSa1(0x10) == 0, "WAI suspends the instruction following it");
  c.WriteCpu(0x2200, 0x90);
  c.Advance(2);
  c.WriteCpu(0x2200, 0x80);
  c.Advance(198);
  Check(c.ReadSa1(0x20) == 1 && c.ReadSa1(0x21) == 1 && c.ReadSa1(0x10) == 1 &&
            c.CpuRegisters().stp,
        "NMI and IRQ use independent programmable vectors and return through the protected IRAM "
        "stack");

  Fixture masked;
  masked.Code(0x8000, {0xcb, 0xa9, 0x73, 0xdb});
  masked.chip.WriteSa1(0x220a, 0x80);
  masked.Start();
  masked.chip.Advance(20);
  masked.chip.WriteCpu(0x2200, 0x80);
  masked.chip.Advance(20);
  Check(masked.chip.CpuRegisters().wai && !masked.chip.CpuRegisters().stp &&
            masked.chip.CpuRegisters().a == 0,
        "SA-1 WAI retains the processor I mask for an enabled IRQ");

  Fixture latency;
  auto &l = latency.chip;
  latency.Code(0x8000, {0x58, 0x78, 0xea, 0xea});
  latency.Code(0x8100, {0xdb});
  l.WriteSa1(0x220a, 0x80);
  SetCpu(l, 0x2207, 0x8100);
  latency.Start();
  l.WriteSa1(0x222a, 0xff);
  l.Advance(4); // CLI samples the previous set-I state.
  Check(l.CpuRegisters().pc == 0x8001, "CLI completes before an IRQ can be taken");
  l.WriteCpu(0x2200, 0x80);
  l.Advance(4); // SEI samples clear I before setting it.
  Check(l.CpuRegisters().pc == 0x8002 && !(l.CpuRegisters().p & 4),
        "SEI samples IRQ before its pending final-cycle I update");
  l.Advance(1);
  Check(l.CpuRegisters().pc == 0x8002 && (l.CpuRegisters().p & 4),
        "SEI sets I when its final bus phase resumes");
  l.Advance(10);
  Check(l.CpuRegisters().pc == 0x8101 && !l.CpuRegisters().stp,
        "A latched IRQ is taken even after SEI sets I");

  Fixture cli;
  auto &q = cli.chip;
  cli.Code(0x8000, {0x58, 0xea, 0xdb});
  q.WriteSa1(0x220a, 0x80);
  SetCpu(q, 0x2207, 0x8100);
  cli.Start();
  q.WriteSa1(0x222a, 0xff);
  q.WriteCpu(0x2200, 0x80);
  q.Advance(4);
  Check(q.CpuRegisters().pc == 0x8001, "Already asserted IRQ is delayed through CLI");
  q.Advance(4);
  Check(q.CpuRegisters().pc == 0x8002, "One instruction after CLI executes before the pending IRQ");
  q.Advance(1);
  Check(q.CpuRegisters().pc == 0x8002, "CLI-delayed IRQ begins with its pending dummy read");
  q.Advance(10);
  Check(q.CpuRegisters().pc == 0x8101, "CLI-delayed IRQ then enters its programmed vector");

  Fixture plp;
  auto &p = plp.chip;
  plp.Code(0x8000, {0x58, 0xa9, 0x34, 0x48, 0x28, 0xea});
  p.WriteSa1(0x220a, 0x80);
  SetCpu(p, 0x2207, 0x8100);
  plp.Start();
  p.WriteSa1(0x222a, 0xff);
  p.Advance(14); // CLI + LDA + PHA.
  p.WriteCpu(0x2200, 0x80);
  p.Advance(8); // PLP samples I before pulling the set-I status.
  Check(p.CpuRegisters().pc == 0x8005 && !(p.CpuRegisters().p & 4),
        "PLP samples IRQ before its pending status read");
  p.Advance(1);
  Check(p.CpuRegisters().pc == 0x8005 && (p.CpuRegisters().p & 4),
        "PLP restores I after its final bus phase resumes");
  p.Advance(10);
  Check(p.CpuRegisters().pc == 0x8101, "An IRQ sampled by PLP survives the pulled I flag");

  Set(p, 0x220c, 0xabcd);
  Set(p, 0x220e, 0x9876);
  p.WriteSa1(0x2209, 0x50);
  for (const auto read : {&Sa1::ReadCpu, &Sa1::ReadSa1}) {
    Check((p.*read)(0xffea, 0) == 0xcd && (p.*read)(0xffeb, 0) == 0xab,
          "Both processors read the substituted native NMI vector");
    Check((p.*read)(0xffee, 0) == 0x76 && (p.*read)(0xffef, 0) == 0x98,
          "Both processors read the substituted native IRQ vector");
    for (uint32_t vector : {0xfffau, 0xfffbu, 0xfffeu, 0xffffu, 0xfffcu, 0xffe4u})
      Check((p.*read)(vector, 0) == 0xea,
            "Vector control retains emulation, reset and COP ROM entries");
  }
}

void NativeInterruptSampling() {
  for (unsigned kind = 0; kind < 8; ++kind) {
    Fixture f;
    auto &c = f.chip;
    if (kind == 0 || kind == 6)
      f.Code(0x8000, {0xcb, 0xa9, 0x73, 0xdb});
    else if (kind == 1)
      f.Code(0x8000, {0x58, 0xcb, 0xa9, 0x73, 0xdb});
    else if (kind == 5)
      f.Code(0x8000, {0x58, 0xea, 0x80, 0xfd});
    else if (kind == 7)
      f.Code(0x8000, {0xdb});
    else
      f.Code(0x8000, {0xea, 0x80, 0xfd});
    for (uint16_t vector : {0x8100, 0x8200, 0x8300})
      f.Code(vector, {0xdb});
    SetCpu(c, 0x2205, 0x8100);
    SetCpu(c, 0x2207, 0x8200);
    c.WriteSa1(0x220a, kind == 0 || kind == 1 || kind == 5 ? 0x80 : 0x10);
    f.Start();
    c.WriteSa1(0x222a, 0xff);
    for (unsigned master = 1; master <= 64; ++master) {
      if ((kind == 0 || kind == 1) && master == 21)
        c.WriteCpu(0x2200, 0x80);
      if (kind == 2 && master == 4)
        c.WriteCpu(0x2200, 0x10);
      if (kind == 2 && master == 6)
        c.WriteCpu(0x2200, 0);
      if ((kind == 3 || kind == 4) && master == 3)
        c.WriteCpu(0x2200, 0x10);
      if (kind == 3 && master == 4)
        SetCpu(c, 0x2205, 0x8300);
      if (kind == 4 && master == 4)
        c.WriteSa1(0x220b, 0x10);
      if ((kind == 3 || kind == 4) && master == 5)
        c.WriteCpu(0x2200, 0);
      if (kind == 5 && master == 5)
        c.WriteCpu(0x2200, 0x80);
      if (kind == 5 && master == 8)
        SetCpu(c, 0x2207, 0x8300);
      if (kind == 5 && master == 9) {
        c.WriteSa1(0x220b, 0x80);
        c.WriteCpu(0x2200, 0);
      }
      if (kind == 6 && master == 21)
        c.WriteCpu(0x2200, 0x50);
      if (kind == 6 && master == 41)
        c.WriteCpu(0x2200, 0x10);
      if (kind == 7 && master == 21)
        c.WriteCpu(0x2200, 0x10);
      if (kind == 7 && master == 23)
        c.WriteCpu(0x2200, 0);
      if (kind == 7 && master == 25)
        c.WriteSa1(0x220b, 0x10);
      if (kind == 7 && master == 41)
        c.WriteCpu(0x2200, 0x20);
      if (kind == 7 && master == 43)
        c.WriteCpu(0x2200, 0);
      c.Advance(1);
      const auto &r = c.CpuRegisters();
      if (kind == 0 && master >= 21)
        Check(r.wai && !r.stp && r.a == 0, "A masked SA-1 IRQ retains WAI");
      if (kind == 1 && master == 27)
        Check(!r.wai && r.pc == 0x8002 && r.s == 0x1ff,
              "WAI completes its final idle before hardware interrupt stack writes");
      if (kind == 1 && master == 41)
        Check(r.pc == 0x8201 && r.stp, "An accepted IRQ leaves WAI through its vector");
      if (kind == 2)
        Check(r.pc >= 0x8000 && r.pc <= 0x8003 && !r.stp,
              "An NMI pulse between sampling phases does not start an interrupt");
      if ((kind == 3 || kind == 4) && master == 15)
        Check(
            r.pc == 0x8101,
            "A sampled NMI retains its request and vector across acknowledgment and vector writes");
      if (kind == 5 && master == 19)
        Check(r.pc == 0x8201, "A sampled IRQ retains its programmed vector during entry");
      if (kind == 6 && master == 21)
        Check(!r.wai && r.pc == 0x8001, "WAI samples NMI while the console hold is asserted");
      if (kind == 6 && master == 40)
        Check(r.pc == 0x8001, "Console hold retains the accepted interrupt before entry");
      if (kind == 6 && master == 55)
        Check(r.pc == 0x8101 && r.stp, "Console hold release starts the accepted NMI");
      if (kind == 7 && master == 25)
        Check(r.stp && !(c.ReadSa1(0x2301) & 0x10),
              "STOP retains its state while acknowledging an NMI status flag");
      if (kind == 7 && master == 55)
        Check(r.pc == 0x8101 && r.stp,
              "Control reset releases STOP without dropping its sampled interrupt");
    }
  }
}

void ConsoleInterruptLatch() {
  Fixture f;
  auto &c = f.chip;
  c.WriteSa1(0x2209, 0x80);
  Check((c.ReadCpu(0x2300) & 0x80) && !c.CpuIrqPending(),
        "A disabled console IRQ retains its status flag");
  c.WriteCpu(0x2201, 0x80);
  Check(c.CpuIrqPending(), "Enabling a flagged console IRQ asserts its request");
  c.WriteCpu(0x2201, 0);
  Check(c.CpuIrqPending(), "Disabling the enable bit retains an asserted console IRQ");
  c.WriteSa1(0x2209, 0);
  Check(c.CpuIrqPending(), "Dropping the cartridge control bit retains its console IRQ");
  c.WriteSa1(0x2230, 0xb0);
  c.WriteSa1(0x2236, 0);
  c.WriteCpu(0x2202, 0x80);
  Check(c.CpuIrqPending() && (c.ReadCpu(0x2300) & 0xa0) == 0x20,
        "A masked conversion flag retains the line while another flag is acknowledged");
  c.WriteCpu(0x2202, 0x20);
  Check(!c.CpuIrqPending(), "Acknowledging both console status flags releases the line");
}

void Synchronization() {
  Fixture whole, pieces;
  for (auto *f : {&whole, &pieces}) {
    f->Code(0x8000, {0xa9, 0xff, 0x8d, 0x2a, 0x22, 0xe6, 0x20, 0x80, 0xfc});
    f->Start();
  }
  whole.chip.Advance(10003);
  unsigned remaining = 10003;
  unsigned step = 1;
  while (remaining) {
    const unsigned clocks = std::min(remaining, step);
    pieces.chip.Advance(clocks);
    remaining -= clocks;
    step = (step * 7) % 29 + 1;
  }
  Check(whole.chip.ReadSa1(0x20) == pieces.chip.ReadSa1(0x20) &&
            whole.chip.CpuRegisters().pc == pieces.chip.CpuRegisters().pc &&
            whole.chip.CpuRegisters().p == pieces.chip.CpuRegisters().p,
        "Instruction clock debt makes fragmented and batched scheduling equivalent");
  Check(whole.chip.MasterClocks() == 10003 && pieces.chip.MasterClocks() == 10003,
        "Master clock accounting retains the exact scheduler budget");
  pieces.chip.WriteCpu(0x2200, 0x40);
  pieces.chip.Advance(1000);
  const uint16_t pc = pieces.chip.CpuRegisters().pc;
  pieces.chip.Advance(64);
  Check(pieces.chip.CpuRegisters().pc == pc, "Console WAIT holds execution between instructions");
  pieces.chip.WriteCpu(0x2200, 0);
  pieces.chip.Advance(100);
  Check(pieces.chip.ReadSa1(0x20) != whole.chip.ReadSa1(0x20),
        "WAIT release resumes execution without a reset");
}

void ExecutableDmaAndTimer() {
  Fixture f;
  auto &c = f.chip;
  f.Code(0x8000,
         {0xa9, 0xff, 0x8d, 0x2a, 0x22,                         // Stack/IRAM write enable.
          0xa9, 0x20, 0x8d, 0x0b, 0x22, 0x8d, 0x0a, 0x22, 0x58, // Acknowledge and enable DMA IRQ.
          0xa9, 0x80, 0x8d, 0x30, 0x22,                         // ROM -> IRAM DMA.
          0xa9, 0x00, 0x8d, 0x32, 0x22, 0xa9, 0x90, 0x8d, 0x33,
          0x22, 0xa9, 0x00, 0x8d, 0x34, 0x22, // Source $00:9000.
          0xa9, 0x04, 0x8d, 0x38, 0x22, 0x9c, 0x39, 0x22, 0xa9,
          0x00, 0x8d, 0x35, 0x22, 0xa9, 0x06, 0x8d, 0x36, 0x22, // Destination $0600; launches DMA.
          0xcb, 0xe6, 0x10, 0xdb});
  f.Code(0x8100, {0xe6, 0x20, 0xa9, 0x20, 0x8d, 0x0b, 0x22, 0x40});
  f.Code(0x9000, {0x12, 0x34, 0x56, 0x78});
  SetCpu(c, 0x2207, 0x8100);
  f.Start();
  c.Advance(1000);
  Check(c.CpuRegisters().stp && Get(c, 0x600, 4) == 0x78563412 && c.ReadSa1(0x20) == 1 &&
            c.ReadSa1(0x10) == 1 && !(c.ReadSa1(0x2301) & 0x20),
        "A running SA-1 program starts DMA, receives its completion IRQ, acknowledges it, and "
        "resumes past WAI");

  Fixture timer;
  auto &t = timer.chip;
  timer.Code(0x8000, {0xa9, 0xff, 0x8d, 0x2a, 0x22, 0x18, 0xfb, 0xa9, 0x40, 0x8d,
                      0x0b, 0x22, 0x8d, 0x0a, 0x22, 0x58, 0xcb, 0xe6, 0x10, 0xdb});
  timer.Code(0x8100, {0xe6, 0x20, 0xa9, 0x40, 0x8d, 0x0b, 0x22, 0x9c, 0x10, 0x22, 0x40});
  SetCpu(t, 0x2207, 0x8100);
  Set(t, 0x2212, 40);
  t.WriteSa1(0x2210, 1);
  timer.Start();
  t.Advance(1000);
  Check(t.CpuRegisters().stp && !t.CpuRegisters().e && t.CpuRegisters().s == 0x1ff &&
            t.ReadSa1(0x20) == 1 && t.ReadSa1(0x10) == 1 && !(t.ReadSa1(0x2301) & 0x40),
        "Native-mode WAI wakes on the timer IRQ and RTI restores its four-byte interrupt stack "
        "frame");
}

} // namespace

int main() {
  try {
    ResetControlFlow();
    DeferredExecution();
    BusPhases();
    ExecutionLifecycle();
    ResetControl();
    TransferTiming();
    MemoryTiming();
    HardwareInterruptTiming();
    NativeInterruptSampling();
    ConsoleInterruptLatch();
    RegistersAndProtection();
    Mapping();
    Arithmetic();
    BitReaderWindows();
    BitReaderLiveState();
    VariableBits();
    NativeTimerEdges();
    Timers();
    NativeDmaTiming();
    DmaLiveRegisters();
    Dma();
    CharacterConversionVisibility();
    CharacterConversion();
    ExecutableProgram();
    Interrupts();
    Synchronization();
    ExecutableDmaAndTimer();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "SA-1 regression failed: %s\n", error.what());
    return 1;
  }
  std::puts("SA-1 processor, mapping, protection, interrupts, timers, arithmetic, DMA, conversion, "
            "and synchronization checks passed");
  return 0;
}
