#include "snes/core/gameboy/Peripherals.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace snes::core::gameboy;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
using Write = std::pair<uint16_t, uint8_t>;
struct Expected {
  unsigned samples;
  uint64_t digest;
};
constexpr std::array<std::array<std::array<Expected, 5>, 8>, 2> ExpectedWake = {
    {{{{{{90, 0xd397907034cdb145ull},
         {90, 0x4709f726e86f9dd5ull},
         {90, 0xaaafae92991de271ull},
         {91, 0x7c2be1a77aca14b1ull},
         {122, 0x2fba93a39acae8e9ull}}},
       {{{86, 0x08e27dcdeb65dd59ull},
         {86, 0x08e27dcdeb65dd59ull},
         {86, 0x08e27dcdeb65dd59ull},
         {87, 0xc9b4c617fcf3fee5ull},
         {118, 0xde6d225d32c27481ull}}},
       {{{86, 0xb5b4d432eba881edull},
         {86, 0xb5b4d432eba881edull},
         {86, 0xb5b4d432eba881edull},
         {87, 0x8c9263aad7dbf259ull},
         {118, 0x6cf17928093eb7b1ull}}},
       {{{86, 0xdaedeb997afb6f31ull},
         {86, 0xaa0c569425b07731ull},
         {86, 0x7b0511165aeee525ull},
         {87, 0x2b7ba407c1630839ull},
         {117, 0x1a38f2b5760e04d9ull}}},
       {{{86, 0x4d50c538ae976bc1ull},
         {86, 0xe285291ec4cb2e5dull},
         {86, 0xeac48bddb7246b1dull},
         {87, 0xc67fdb7811d12c8dull},
         {117, 0xedae0f33fe0ba985ull}}},
       {{{89, 0x73a98e1057299f15ull},
         {89, 0xd7c2767ff7156339ull},
         {89, 0x7e0aace0463ebae9ull},
         {90, 0xac2f1d53692d0f51ull},
         {120, 0x45a538195c19d4b5ull}}},
       {{{89, 0x3b38eb4cb69a8aa5ull},
         {89, 0x822f54c2dbf48c6dull},
         {89, 0xa1ed99f631e68859ull},
         {90, 0x92cb362a298d2611ull},
         {120, 0x0d05bcc788f72561ull}}},
       {{{86, 0x2ceb6da5e3c58879ull},
         {86, 0xfdea289f537f892dull},
         {86, 0x84120acc26953575ull},
         {87, 0xc284985ceb288f75ull},
         {117, 0x42b695254b2052e1ull}}}}},
     {{{{{90, 0xf9c92360584d2e01ull},
         {90, 0xdb263dda14710689ull},
         {91, 0x450da5cebe695c35ull},
         {91, 0x64e1c9d8d2309dadull},
         {122, 0x71c80522e809b5fdull}}},
       {{{86, 0xe0815f613ec74ab9ull},
         {86, 0xe0815f613ec74ab9ull},
         {86, 0xe0815f613ec74ab9ull},
         {87, 0x60334936bd8409cdull},
         {118, 0xe021448b749a2669ull}}},
       {{{86, 0xc0ce56e53d62686dull},
         {86, 0xc0ce56e53d62686dull},
         {86, 0xc0ce56e53d62686dull},
         {87, 0x0b5331fb2ce1d78dull},
         {118, 0xd86953049a8ce085ull}}},
       {{{86, 0xeead588a54046b59ull},
         {86, 0x19531a11163908f1ull},
         {86, 0x19531a11163908f1ull},
         {87, 0x1f3cef85f9b3ede1ull},
         {117, 0x5669d153303582d1ull}}},
       {{{86, 0x1db3ee58ed1fa021ull},
         {86, 0xed6922c6c3695b65ull},
         {86, 0xed6922c6c3695b65ull},
         {87, 0xfb4780dfdea60219ull},
         {117, 0x4b6ea59622367c85ull}}},
       {{{89, 0xdeae08e8c97bbd6dull},
         {89, 0xbf1d95a480ef66edull},
         {89, 0xc2b8c15c6e45fbc9ull},
         {90, 0x8a6a85147ec5a495ull},
         {120, 0x29b4da45ea171c99ull}}},
       {{{89, 0xf71820c43a300b15ull},
         {89, 0x037c834f7c2dca0dull},
         {89, 0xff580242c59ede9dull},
         {90, 0x5dec8e88c9539fedull},
         {120, 0x8e48f29a7374439dull}}},
       {{{86, 0x5583245e9d19283dull},
         {86, 0xb3a7664a96235e5dull},
         {86, 0x508147426759b8e5ull},
         {87, 0x031310b99410ccd9ull},
         {117, 0x422f248303fc0df5ull}}}}}}};

std::vector<Write> Fixture(unsigned which) {
  if (which == 0)
    return {{0xff26, 0x80}, {0xff24, 0x77}, {0xff25, 0xff}, {0xff11, 0x40}, {0xff12, 0xb2},
            {0xff13, 0x87}, {0xff14, 0x85}, {0xff16, 0xc0}, {0xff17, 0x81}, {0xff18, 0xa2},
            {0xff19, 0x86}, {0xff30, 0x0},  {0xff31, 0x11}, {0xff32, 0x22}, {0xff33, 0x33},
            {0xff34, 0x44}, {0xff35, 0x55}, {0xff36, 0x66}, {0xff37, 0x77}, {0xff38, 0x88},
            {0xff39, 0x99}, {0xff3a, 0xaa}, {0xff3b, 0xbb}, {0xff3c, 0xcc}, {0xff3d, 0xdd},
            {0xff3e, 0xee}, {0xff3f, 0xff}, {0xff1a, 0x80}, {0xff1c, 0x20}, {0xff1d, 0x23},
            {0xff1e, 0x87}, {0xff21, 0x72}, {0xff22, 0x19}, {0xff23, 0x80}, {0xff00, 0x20}};
  std::vector<Write> writes = {{0xff26, 0x80}, {0xff24, 0x77}, {0xff25, 0xff}, {0xff00, 0x20}};
  if (which == 7) {
    writes.insert(writes.end(), {{0xff11, 0xc0}, {0xff12, 0xf0}, {0xff13, 0xff}, {0xff14, 0x87}});
  } else if (which <= 2) {
    writes.insert(writes.end(), {{0xff10, uint8_t(0x10 | (which == 1 ? 1 : 6))},
                                 {0xff11, 0xc0},
                                 {0xff12, 0xf0},
                                 {0xff13, 0xff},
                                 {0xff14, 0x87}});
  } else if (which <= 4) {
    writes.insert(writes.end(),
                  {{0xff21, 0xf0}, {0xff22, uint8_t(0x18 | (which - 3))}, {0xff23, 0x80}});
  } else {
    for (unsigned i = 0; i < 16; ++i)
      writes.emplace_back(uint16_t(0xff30 + i), uint8_t(i * 29 + 0x35));
    writes.insert(writes.end(), {{0xff1a, 0x80},
                                 {0xff1c, 0x20},
                                 {0xff1d, uint8_t(which == 5 ? 0xfe : 0xff)},
                                 {0xff1e, 0x87}});
  }
  return writes;
}

struct System final : Audio::Output, Video::Output {
  Host host;
  Peripherals devices{*this, *this};
  Machine machine{host, devices};
  std::vector<uint8_t> pixels;
  unsigned samples = 0;
  uint64_t digest = 14695981039346656037ull, lastSample = 0;
  void BeginFrame() override {
    host.BeginFrame();
  }
  void BeginLine() override {
    host.BeginLine();
  }
  void Pixel(uint8_t shade) override {
    host.Pixel(shade);
    pixels.push_back(shade);
  }
  void Sample(uint64_t clock, int16_t left, int16_t right) override {
    Check(clock > lastSample, "Audio callbacks keep increasing device clocks");
    lastSample = clock;
    ++samples;
    for (int16_t value : {left, right})
      for (unsigned shift : {0u, 8u}) {
        digest ^= (uint16_t(value) >> shift) & 255;
        digest *= 1099511628211ull;
      }
  }
  void Load(bool secondModel, const std::vector<uint8_t> &program, uint8_t cartridgeType = 0) {
    std::vector<uint8_t> rom(32768);
    rom[0x147] = cartridgeType;
    std::copy(program.begin(), program.end(), rom.begin() + 0x200);
    std::array<uint8_t, 256> boot{};
    Check(machine.Load(rom, boot), "Generated cartridge and boot images load");
    host.Reset();
    machine.Reset(secondModel);
    machine.WriteMemory(0xff40, 0);
    machine.WriteMemory(0xff50, 1);
    machine.WriteMemory(0xff04, 0);
    Cpu::State state;
    state.pc = 0x200;
    state.sp = 0xd000;
    machine.SetCpuState(state);
    samples = 0;
    digest = 14695981039346656037ull;
    lastSample = 0;
    pixels.clear();
  }
};

void StopAndWake() {
  constexpr std::array<unsigned, 5> waits = {0, 1, 2, 31, 1000};
  for (unsigned model = 0; model < 2; ++model)
    for (unsigned fixture = 0; fixture < 8; ++fixture)
      for (unsigned wait = 0; wait < waits.size(); ++wait) {
        const auto writes = Fixture(fixture);
        std::vector<uint8_t> program;
        for (const auto &[address, value] : writes)
          program.insert(program.end(), {0x3e, value, 0xe0, uint8_t(address)});
        program.insert(program.end(), {0x10, 0});
        const auto resume = uint16_t(0x200 + program.size());
        program.insert(program.end(), {0xf0, 0x30, 0, 0xc3, uint8_t(resume), uint8_t(resume >> 8)});
        System system;
        system.Load(model != 0, program);
        for (unsigned i = 0; i < writes.size() * 2 + 1; ++i)
          system.machine.RunInstruction();
        Check(system.machine.CpuState().stopped, "STOP freezes processor execution");
        for (unsigned i = 0; i < waits[wait]; ++i)
          Check(system.machine.RunInstruction() == 4 && system.machine.CpuState().stopped,
                "Sleeping clocks keep the processor stopped");
        system.host.Write(0x6004, 0xfe);
        system.machine.WriteMemory(0xff00, 0x20);
        Check(system.machine.RunInstruction() == 12 && !system.machine.CpuState().stopped,
              "A falling joypad pin wakes the processor through its resume cycles");
        for (unsigned i = 0; i < 1024; ++i)
          system.machine.RunInstruction();
        const auto expected = ExpectedWake[model][fixture][wait];
        if (system.samples != expected.samples || system.digest != expected.digest)
          std::fprintf(stderr, "Model %u fixture %u wait %u: %u samples, digest %016llx\n", model,
                       fixture, waits[wait], system.samples,
                       static_cast<unsigned long long>(system.digest));
        Check(system.samples == expected.samples && system.digest == expected.digest,
              "STOP and wake retain oscillator timing, sample phase, and stereo output");
      }
}

void InfraredExecution() {
  for (bool secondModel : {false, true}) {
    System system;
    system.Load(
        secondModel,
        {0x3e, 14, 0xea, 0, 0, 0x3e, 1, 0xea, 0, 0xa0, 0xfa, 0, 0xa0, 0xea, 0, 0xc1, 0xc3, 0x0a, 2},
        0xff);
    for (unsigned i = 0; i < 631; ++i) {
      system.machine.RunInstruction();
      Check(system.machine.CpuState().a != 0xc1,
            "Instruction-driven infrared reads wait for the sensor charge threshold");
    }
    system.machine.RunInstruction();
    Check(system.machine.CpuState().a == 0xc1 && system.machine.Clocks() == 10096 &&
              system.machine.GameCartridge().InfraredOutput(),
          "CPU bus batches clock the cartridge sensor while its own transmitter is active");
  }
}

void ClockCommandExecution() {
  constexpr uint64_t now = 1789389296;
  for (bool secondModel : {false, true}) {
    System system;
    system.machine.GameCartridge().SetTimeSource([] { return now; });
    std::vector<uint8_t> program;
    const auto store = [&](uint16_t address, uint8_t value) {
      program.insert(program.end(), {0x3e, value, 0xea, uint8_t(address), uint8_t(address >> 8)});
    };
    store(0, 0xb);
    store(0xa000, 0x40);
    store(0xa000, 0x50);
    for (uint8_t command : {0x3f, 0x39, 0x35, 0x3f, 0x3f, 0x3f, 0x3f})
      store(0xa000, command);
    store(0xa000, 0x40);
    store(0xa000, 0x50);
    store(0xa000, 0x10);
    store(0, 0xc);
    program.insert(program.end(), {0xfa, 0, 0xa0});
    system.Load(secondModel, program, 0xfe);
    for (unsigned i = 0; i < 20; ++i)
      system.machine.RunInstruction();
    Check(system.machine.Clocks() == 240, "Clock command writes retain normal CPU bus cycle costs");
    system.machine.GameCartridge().SynchronizeRtc(now + 60);
    for (unsigned i = 0; i < 9; ++i)
      system.machine.RunInstruction();
    const auto battery = system.machine.GameCartridge().SaveBattery();
    Check(battery.size() == 17, "Clock cartridges retain the complete battery trailer");
    uint64_t timestamp = 0;
    for (unsigned i = 0; i < 8; ++i)
      timestamp |= uint64_t(battery[i]) << (i * 8);
    Check(system.machine.CpuState().a == 0 && system.machine.Clocks() == 352 &&
              timestamp == now + 60 &&
              std::all_of(battery.begin() + 8, battery.end(),
                          [](uint8_t value) { return value == 0; }),
          "Instruction-driven clock ports expose minute and day rollover in both adapter models");
  }
}

void ObjectMemoryExecution() {
  constexpr std::array<std::pair<uint8_t, unsigned>, 8> instructions = {
      {{0x7e, 1}, {0x77, 2}, {0x23, 3}, {0x2b, 4}, {0x2a, 6}, {0xc5, 9}, {0xc1, 10}, {0x34, 11}}};
  constexpr std::array<unsigned, 7> phases = {0, 4, 28, 60, 72, 76, 80};
  constexpr std::array<uint64_t, 2> expected = {0x6cea28654e586db6ull, 0x801262c97b09defbull};
  for (unsigned model = 0; model < 2; ++model) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto [opcode, index] : instructions)
      for (const unsigned phase : phases) {
        System system;
        system.Load(model != 0, {opcode, 0, 0, 0});
        auto state = system.machine.CpuState();
        state.a = 0x5a;
        state.b = state.d = state.h = 0xfe;
        state.c = state.e = state.l = 0x20;
        state.sp = 0xfe20;
        system.machine.SetCpuState(state);
        const unsigned fixture = model * 2295 + index * 153 + 51 + phase / 2;
        for (unsigned i = 0; i < 160; ++i)
          system.devices.Write(uint16_t(0xfe00 + i), uint8_t(i * 37 + (i / 8) * 11 + fixture * 17),
                               Machine::Access::Dma);
        for (const auto [address, value] :
             std::array<Write, 4>{{{0xff47, 0xe4}, {0xff48, 0xe4}, {0xff49, 0xe4}, {0xff4b, 7}}})
          system.machine.WriteMemory(address, value);
        system.machine.WriteMemory(0xff40, 0x91);
        system.machine.AdvanceHardware(456 + phase);
        hash = (hash ^ system.machine.RunInstruction()) * 1099511628211ull;
        for (unsigned i = 0; i < 160; ++i)
          hash = (hash ^ system.devices.Read(uint16_t(0xfe00 + i), Machine::Access::Dma)) *
                 1099511628211ull;
      }
    Check(hash == expected[model],
          "CPU reads, writes, increments, stack accesses, and scanner timing retain object memory");
  }
}

std::array<uint8_t, 8192> PatternedVideoRam() {
  std::array<uint8_t, 8192> ram{};
  for (unsigned tile = 0; tile < 384; ++tile)
    for (unsigned y = 0; y < 8; ++y)
      for (unsigned x = 0; x < 8; ++x) {
        const unsigned color = (tile + (tile >> 8) * 2 + x + y * 3) & 3;
        ram[tile * 16 + y * 2] |= uint8_t((color & 1) << (7 - x));
        ram[tile * 16 + y * 2 + 1] |= uint8_t((color >> 1) << (7 - x));
      }
  for (unsigned i = 0; i < 1024; ++i) {
    ram[0x1800 + i] = uint8_t(i * 13 + (i / 32) * 7);
    ram[0x1c00 + i] = uint8_t(3 + (i % 4) * 7 + (i / 32) * 5);
  }
  return ram;
}

void SharedObjectBusLatches() {
  const auto ram = PatternedVideoRam();
  for (bool secondModel : {false, true}) {
    System system;
    system.Load(secondModel, {0});
    for (unsigned i = 0; i < ram.size(); ++i)
      system.devices.Write(uint16_t(0x8000 + i), ram[i], Machine::Access::Dma);
    for (unsigned i = 0; i < 40; ++i)
      for (const auto [offset, value] :
           std::array<std::pair<unsigned, uint8_t>, 4>{{{0, 17},
                                                        {1, uint8_t(i * 13 + 8)},
                                                        {2, uint8_t(i * 7 + 5)},
                                                        {3, uint8_t((i & 3) * 0x20)}}})
        system.devices.Write(uint16_t(0xfe00 + i * 4 + offset), value, Machine::Access::Dma);
    for (const auto [address, value] : std::array<Write, 5>{
             {{0xff43, 3}, {0xff47, 0xe4}, {0xff48, 0xe4}, {0xff49, 0x1b}, {0xff4b, 80}}})
      system.machine.WriteMemory(address, value);
    system.machine.WriteMemory(0xff40, 0x93);
    for (unsigned i = 0; i < 160; ++i)
      system.machine.WriteMemory(uint16_t(0xd000 + i), uint8_t(i * 41 + 17));
    system.machine.AdvanceHardware(468);
    system.machine.WriteMemory(0xff46, 0xd0);
    system.machine.AdvanceHardware(12);
    Check(system.machine.ReadMemory(0xfe20) == 0xff,
          "DMA retains the CPU object-memory read restriction");
    system.machine.AdvanceHardware(1600);
    uint64_t hash = 14695981039346656037ull;
    for (auto shade : system.pixels)
      hash = (hash ^ shade) * 1099511628211ull;
    Check(system.pixels.size() == 787 && hash == 0xc8a8b6f27e91b7d4ull,
          "Sprite flags and scan coordinates retain their shared bus latches across DMA");
  }
}

void WindowWriteDuringObjectFetch() {
  const auto ram = PatternedVideoRam();
  constexpr std::array<unsigned, 10> positions{0, 1, 4, 8, 12, 14, 79, 80, 160, 167};
  for (bool secondModel : {false, true})
    for (bool tallObjects : {false, true})
      for (unsigned clock : {554u, 559u}) {
        System system;
        system.Load(secondModel, {0});
        for (unsigned i = 0; i < ram.size(); ++i)
          system.devices.Write(uint16_t(0x8000 + i), ram[i], Machine::Access::Dma);
        for (unsigned i = 0; i < positions.size(); ++i)
          for (const auto [offset, value] :
               std::array<std::pair<unsigned, uint8_t>, 4>{{{0, 17},
                                                            {1, uint8_t(positions[i])},
                                                            {2, uint8_t(i * 7 + 5)},
                                                            {3, uint8_t((i & 3) * 0x20)}}})
            system.devices.Write(uint16_t(0xfe00 + i * 4 + offset), value, Machine::Access::Dma);
        for (const auto [address, value] :
             std::array<Write, 5>{{{0xff43, 3},
                                   {0xff47, 0xe4},
                                   {0xff48, 0xe4},
                                   {0xff49, 0x1b},
                                   {0xff4b, uint8_t(tallObjects ? 80 : 7)}}})
          system.machine.WriteMemory(address, value);
        system.machine.WriteMemory(0xff40, tallObjects ? 0xf7 : 0xf3);
        system.machine.AdvanceHardware(clock);
        system.machine.WriteMemory(0xff4b, 0);
        system.machine.AdvanceHardware(1600 - clock);
        uint64_t hash = 14695981039346656037ull;
        for (auto shade : system.pixels)
          hash = (hash ^ shade) * 1099511628211ull;
        const auto expected = tallObjects ? 0xa76f4fd6cbf4491aull : 0x30bf5bdf899cfd7cull;
        Check(system.pixels.size() == 552 && hash == expected,
              "A window write during object fetch waits for the next pixel trigger check");
      }
}

void VideoDmaBusContention() {
  struct Fixture {
    unsigned scene, page, start, phase;
    bool sleeping;
  };
  std::vector<Fixture> fixtures;
  for (unsigned scene = 0; scene < 4; ++scene)
    for (unsigned page : {0x80u, 0x87u, 0x90u, 0x98u, 0x9fu})
      for (unsigned start : {78u, 450u, 526u, 534u, 640u})
        for (bool sleeping : {false, true})
          for (unsigned phase : {3u, 12u, 63u, 255u, 639u, 640u, 645u})
            fixtures.push_back({scene, page, start, phase, sleeping});
  const auto ram = PatternedVideoRam();
  for (bool secondModel : {false, true}) {
    uint64_t digest = 14695981039346656037ull;
    for (const auto &fixture : fixtures) {
      System system;
      system.Load(secondModel, {0});
      for (unsigned i = 0; i < ram.size(); ++i)
        system.devices.Write(uint16_t(0x8000 + i), ram[i], Machine::Access::Dma);
      for (unsigned i = 0; i < 40; ++i) {
        const std::array<uint8_t, 4> object{17, uint8_t(i * 13 + 8), uint8_t(i * 7 + 5),
                                            uint8_t((i & 3) * 0x20)};
        for (unsigned byte = 0; byte < object.size(); ++byte)
          system.devices.Write(uint16_t(0xfe00 + i * 4 + byte), object[byte], Machine::Access::Dma);
      }
      for (const auto [address, value] : std::array<Write, 7>{{{0xff43, 3},
                                                               {0xff47, 0xe4},
                                                               {0xff48, 0xe4},
                                                               {0xff49, 0x1b},
                                                               {0xff4b, 80},
                                                               {0xff80, 0x76},
                                                               {0xff81, 0}}})
        system.machine.WriteMemory(address, value);
      auto state = system.machine.CpuState();
      state.pc = 0xff80;
      system.machine.SetCpuState(state);
      constexpr std::array<uint8_t, 4> lcd{0x91, 0x93, 0x97, 0xf3};
      system.machine.WriteMemory(0xff40, lcd[fixture.scene]);
      system.machine.AdvanceHardware(fixture.start);
      system.machine.WriteMemory(0xff46, uint8_t(fixture.page));
      system.machine.AdvanceHardware(fixture.phase);
      unsigned sleepClocks = 0;
      if (fixture.sleeping) {
        sleepClocks = system.machine.RunInstruction();
        Check(system.machine.CpuState().halted, "The DMA fixture enters CPU sleep from HRAM");
      }
      uint64_t hash = 14695981039346656037ull;
      const auto mix = [&](uint8_t value) { hash = (hash ^ value) * 1099511628211ull; };
      const auto capture = [&] {
        for (unsigned i = 0; i < 160; ++i)
          mix(system.devices.Read(uint16_t(0xfe00 + i), Machine::Access::Dma));
      };
      capture();
      system.machine.AdvanceHardware(2000 - fixture.start - fixture.phase - sleepClocks);
      capture();
      for (unsigned shift : {0u, 8u, 16u, 24u})
        mix(uint8_t(system.pixels.size() >> shift));
      for (auto shade : system.pixels)
        mix(shade);
      for (unsigned shift = 0; shift < 64; shift += 8)
        digest = (digest ^ uint8_t(hash >> shift)) * 1099511628211ull;
    }
    Check(digest == 0xb335eaae69c0d30dull,
          "Video DMA retains address collisions, OAM feedback, sleep and release phases");
  }
}

void HostVideoAndReset() {
  System system;
  system.Load(true, {0x18, 0xfe});
  system.machine.WriteMemory(0xff47, 0xe4);
  system.machine.WriteMemory(0xff40, 0x91);
  system.machine.AdvanceHardware(456 * 144);
  Check(system.machine.ReadMemory(0xff44) == 144 && (system.machine.ReadMemory(0xff0f) & 1),
        "The display requests vertical blank through the machine interrupt bus");
  system.host.Write(0x6001, 0);
  for (unsigned i = 0; i < 320; ++i)
    Check(system.host.Read(0x7800) == 0, "The host receives complete blank-background tile rows");
  system.machine.Reset(true);
  Check(system.machine.ReadMemory(0xff26) == 0x70 && system.machine.ReadMemory(0xff40) == 0,
        "Reset clears sound and display register state together");
}
} // namespace

int main() {
  try {
    StopAndWake();
    InfraredExecution();
    ClockCommandExecution();
    ObjectMemoryExecution();
    SharedObjectBusLatches();
    WindowWriteDuringObjectFetch();
    VideoDmaBusContention();
    HostVideoAndReset();
    std::puts("Handheld peripheral timing, wake-up, audio and host display checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
