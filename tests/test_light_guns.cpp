#include "ConsoleConfig.hpp"
#include "snes/core/SnesCpu.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>

using namespace snes::core;
namespace {
void Check(bool condition, const char *label) {
  if (!condition)
    throw std::runtime_error(label);
}
void Latch(ControllerPorts &ports) {
  ports.SetLatch(true);
  ports.SetLatch(false);
}
uint32_t Packet(ControllerPorts &ports, unsigned bits, unsigned port = 1) {
  uint32_t value = 0;
  while (bits--)
    value = (value << 1) | ports.Read(port);
  return value;
}

void ScopePackets() {
  ControllerPorts ports;
  LightGunState input;
  ports.SetGunCallback([&](int, int) { return input; });
  ports.Configure(1, ControllerDevice::SuperScope);
  Latch(ports);
  Check(Packet(ports, 16) == 0x00ff, "Scope idle signature and ones after packet");
  input.trigger = true;
  input.offscreen = false;
  ports.SetLatch(true);
  Check(ports.Read(1) == 1 && ports.Read(1) == 0, "Scope shifts serial data while latch is high");
  ports.SetLatch(false);
  Check(Packet(ports, 16) == 0x00ff, "A new packet keeps the held normal trigger locked");
  Latch(ports);
  Check(Packet(ports, 8) == 0, "Normal trigger is reported once per press");
  input.turbo = true;
  Latch(ports);
  Check(Packet(ports, 8) == 0xa0, "Turbo starts firing a held trigger");
  Latch(ports);
  Check(Packet(ports, 8) == 0xa0, "Turbo repeats through strobes");
  input.turbo = false;
  Latch(ports);
  Check(Packet(ports, 8) == 0xa0, "Releasing the toggle button retains the turbo switch");
  input.turbo = true;
  Latch(ports);
  Check(Packet(ports, 8) == 0, "Stopping turbo clears its flag and keeps the held trigger locked");
  input.turbo = false;
  input.trigger = false;
  Latch(ports);
  Check(Packet(ports, 8) == 0, "Scope observes a released trigger at packet sampling");
  input.trigger = true;
  Latch(ports);
  Check(Packet(ports, 8) == 0x80, "A released normal trigger can fire again");
  input.trigger = false;
  input.offscreen = false;
  Latch(ports);
  Check(Packet(ports, 8) == 0, "Released buttons clear the next packet");
  input.cursor = true;
  input.pause = true;
  Latch(ports);
  Check(Packet(ports, 8) == 0x50, "Cursor and pause bits");
  Latch(ports);
  Check(Packet(ports, 8) == 0x40, "Held cursor remains asserted while pause stays locked");
  for (unsigned i = 0; i < 1000; ++i)
    Check(ports.Read(1) == 1, "Scope reads saturate");
  ports.Reset();
  input = {};
  Latch(ports);
  Check(Packet(ports, 8) == 0, "Reset retains device but clears packet state");
}

void ScopeReadBoundaries() {
  for (unsigned port : {0u, 1u}) {
    ControllerPorts ports;
    LightGunState input;
    ports.Configure(port, ControllerDevice::SuperScope);
    ports.SetGunCallback([&](int, int) { return input; });
    ports.SetLatch(true);
    input.trigger = input.cursor = input.pause = true;
    Check(Packet(ports, 8, port) == 0xd0,
          "Scope samples changed buttons on its first read after the latch edge");
    input = {};
    Check(ports.Read(port) == 1, "Scope retains its completed packet until a latch transition");
    ports.SetLatch(true);
    Check(ports.Read(port) == 1, "A repeated high latch does not restart Scope serial reads");
    ports.SetLatch(false);
    Check(ports.Read(port) == 0, "A falling latch restarts Scope serial reads");
    input.cursor = true;
    Check(Packet(ports, 7, port) == 0, "Scope retains buttons captured by the first bit");
    ports.SetLatch(true);
    Check(Packet(ports, 8, port) == 0x40, "A rising latch also starts a fresh Scope packet");
  }
}

void ScopeTurboSampling() {
  ControllerPorts ports;
  LightGunState input;
  ports.Configure(1, ControllerDevice::SuperScope);
  ports.SetGunCallback([&](int, int) { return input; });
  input.turbo = true;
  Latch(ports);
  input.turbo = false;
  Check(Packet(ports, 8) == 0, "An unobserved turbo press cannot change the switch");
  input.turbo = true;
  Latch(ports);
  Check(Packet(ports, 8) == 0x20, "The first serial read toggles the Scope switch");
  Latch(ports);
  Check(Packet(ports, 8) == 0x20, "A held toggle button changes the switch only once");
  input.turbo = false;
  Latch(ports);
  Check(Packet(ports, 8) == 0x20, "The switch retains its position after button release");
  input.turbo = true;
  Latch(ports);
  Check(Packet(ports, 8) == 0, "A second observed toggle press turns turbo off");
  input.turbo = false;
  Latch(ports);
  input.turbo = true;
  Latch(ports);
  Check(Packet(ports, 8) == 0, "Latch edges cannot observe a released toggle button");
  ports.Reset();
  Latch(ports);
  Check(Packet(ports, 8) == 0x20,
        "Reset reconstructs the Scope switch before its first observed button press");
}

void JustifierPackets() {
  for (bool paired : {false, true}) {
    ControllerPorts ports;
    std::array<LightGunState, 2> input;
    input[0].trigger = true;
    input[1].trigger = input[1].start = true;
    ports.Configure(1, paired ? ControllerDevice::Justifiers : ControllerDevice::Justifier);
    ports.SetGunCallback([&](int, int gun) { return input[gun]; });
    ports.SetLatch(true);
    Check(ports.Read(1) == 0 && ports.Read(1) == 0, "Justifier strobe reads zero");
    ports.SetLatch(false);
    Check(Packet(ports, 32) == (paired ? 0x000e55d8u : 0x000e5588u),
          "Justifier signature, buttons, and select bit");
    for (unsigned i = 0; i < 1000; ++i)
      Check(ports.Read(1) == 1, "Justifier reads saturate");
    Latch(ports);
    Check(Packet(ports, 32) == (paired ? 0x000e55d0u : 0x000e5580u),
          "Gun selection toggles with every strobe");
    input[0].trigger = false;
    input[0].start = true;
    Latch(ports);
    Check(Packet(ports, 32) == (paired ? 0x000e5578u : 0x000e5528u),
          "Independent start and trigger bits");
  }
}

void JustifierReadBoundaries() {
  for (bool paired : {false, true}) {
    ControllerPorts ports;
    std::array<LightGunState, 2> input;
    ports.Configure(1, paired ? ControllerDevice::Justifiers : ControllerDevice::Justifier);
    ports.SetGunCallback([&](int, int gun) { return input[gun]; });
    ports.SetLatch(true);
    Check(ports.Read(1) == 0, "First Justifier bit captures released buttons");
    for (auto &gun : input)
      gun.trigger = gun.start = true;
    Check(
        Packet(ports, 31) == 0x000e5500,
        "Justifier shifts its full packet while latch is high and retains its first-read buttons");
    Check(ports.Read(1) == 1, "Justifier latch-high reads saturate after the packet");

    ports.SetLatch(false);
    Check(
        Packet(ports, 32) == (paired ? 0x000e55f8u : 0x000e55a8u),
        "Justifier falling latch selects the next gun and its first read samples current buttons");
    ports.SetLatch(true);
    for (auto &gun : input)
      gun.trigger = gun.start = false;
    Check(Packet(ports, 32) == 0x000e5508,
          "Justifier rising latch resets the packet without changing gun selection");
  }
}

void RiflePackets() {
  ControllerPorts ports;
  LightGunState input;
  ports.Configure(1, ControllerDevice::MacsRifle);
  ports.SetGunCallback([&](int, int) { return input; });
  for (bool strobe : {false, true}) {
    ports.SetLatch(strobe);
    input.trigger = false;
    Check(Packet(ports, 32) == 0, "Rifle trigger low");
    input.trigger = true;
    Check(Packet(ports, 32) == 0xffffffff, "Rifle exposes live trigger without latching");
  }
}

struct Input : IInputProvider {
  std::array<LightGunState, 2> guns;
  InputState Poll(uint64_t) override {
    return {};
  }
  LightGunState PollLightGun(int, int gun, uint64_t) override {
    return guns[gun];
  }
};
std::unique_ptr<Emulator> Machine(Input &input, bool pal = false,
                                  Ppu::RenderModel model = Ppu::RenderModel::Scanline) {
  std::vector<uint8_t> rom(0x8000, 0);
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0] = 0xdb;
  rom[0x7fc0] = 'T';
  rom[0x7fd5] = 0x20;
  rom[0x7fd7] = 5;
  rom[0x7fd9] = pal ? 2 : 1;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffd] = 0x80;
  auto config = snes::tests::DeterministicConsoleConfig();
  config.ppu.model = model;
  auto emu = std::make_unique<Emulator>(config);
  emu->AttachInputProvider(&input);
  Check(emu->LoadCartridge(rom), "Fixture loads");
  return emu;
}
unsigned Counter(MemoryBus &bus, unsigned address) {
  const unsigned lo = bus.Read(address);
  return lo | ((bus.Read(address) & 1) << 8);
}

void RifleBeamLatches() {
  for (auto device : {ControllerDevice::MacsRifle}) {
    Input input;
    input.guns[0].x = 32;
    input.guns[0].y = 10;
    input.guns[0].offscreen = false;
    input.guns[0].trigger = true;
    input.guns[1] = input.guns[0];
    input.guns[1].x = 200;
    input.guns[1].y = 20;
    auto emu = Machine(input);
    auto &ports = emu->GetAutoJoypad().Ports();
    auto &bus = emu->GetBus();
    ports.Configure(1, device);
    ports.PollLightGuns();
    const unsigned h = 108;
    const unsigned v = 52;
    emu->GetTiming().Tick(v * 1364 + (h - 1) * 4);
    Check((bus.Read(0x213f) & 0x40) == 0, "Counter does not latch before beam arrival");
    emu->GetTiming().Tick(6);
    Check((bus.Read(0x4213) & 0x80) == 0, "Gun pulls I/O pin low at beam hit");
    Check((bus.Read(0x213f) & 0x40) != 0, "Gun sets PPU latch flag");
    Check(Counter(bus, 0x213c) == h && Counter(bus, 0x213d) == v, "Gun beam coordinates");
    emu->GetTiming().Tick(4);
    Check((bus.Read(0x4213) & 0x80) != 0, "Gun pulse releases I/O pin");
    Check((bus.Read(0x213f) & 0x40) == 0, "Latch flag clears after status read");

    ports.FrameBegin();
    bus.Write(0x4201, 0x7f);
    bus.Read(0x213f);
    emu->GetPpu().LatchCounters(17, 23);
    ports.PollLightGuns();
    ports.BeamPosition(h, v, 224);
    // WRIO low keeps STAT78 bit 6 high regardless of gun pulses; inspect coordinates instead.
    Check(Counter(bus, 0x213c) == 17 && Counter(bus, 0x213d) == 23,
          "WRIO-low beam leaves stored coordinates intact");
    bus.Write(0x4201, 0xff);
    bus.Read(0x213f);
    ports.FrameBegin();
    input.guns[0].offscreen = true;
    ports.PollLightGuns();
    ports.BeamPosition(h, v, 224);
    Check((bus.Read(0x213f) & 0x40) == 0, "Offscreen gun does not latch");
  }
}

void RifleDeviceLifetimes() {
  for (bool pal : {false, true}) {
    for (auto model : {Ppu::RenderModel::Progressive, Ppu::RenderModel::Scanline}) {
      Input input;
      input.guns[0].y = 10;
      input.guns[0].offscreen = false;
      auto machine = Machine(input, pal, model);
      auto &ports = machine->GetAutoJoypad().Ports();
      auto &timing = machine->GetTiming();
      auto &bus = machine->GetBus();
      timing.Tick(52 * 1364);
      const auto beam = [&](unsigned x) {
        input.guns[0].x = int32_t(x);
        ports.PollLightGuns();
        const unsigned clock = (x + 76) * 4 + 2;
        Check(timing.VCounter() == 52 && timing.HCounter() < clock,
              "Rifle lifetime test advances to a later beam target");
        timing.Tick(clock - timing.HCounter());
      };
      unsigned index = 0;
      for (auto device :
           {ControllerDevice::None, ControllerDevice::Gamepad, ControllerDevice::Mouse,
            ControllerDevice::Multitap, ControllerDevice::SuperScope, ControllerDevice::Justifier,
            ControllerDevice::Justifiers}) {
        ports.Configure(1, ControllerDevice::MacsRifle);
        beam(16 + index++ * 8);
        Check((bus.Read(0x4213) & 0x80) == 0, "Connected rifle produces its beam pulse");
        ports.Configure(1, device);
        Check((bus.Read(0x4213) & 0x80) != 0, "Changing the receiver releases a pending pulse");
        ports.Configure(0, ControllerDevice::MacsRifle);
        ports.PollLightGuns();
        timing.Tick(8);
        Check((bus.Read(0x4213) & 0x80) != 0,
              "A rifle on port one cannot retain the port-two pulse");
      }
      ports.Configure(1, ControllerDevice::MacsRifle);
      beam(100);
      Check((bus.Read(0x4213) & 0x80) == 0, "A replacement rifle can latch later in the field");
      timing.Tick(4);
      Check((bus.Read(0x4213) & 0x80) != 0, "A rifle pulse ends at the next polling edge");
      beam(108);
      Check(Counter(bus, 0x213c) == 176, "A rifle cannot latch a second target in the same field");
      ports.FrameBegin();
      beam(116);
      Check((bus.Read(0x4213) & 0x80) == 0 && Counter(bus, 0x213c) == 192,
            "Frame begin rearms a completed rifle receiver");
      Check(machine->Reset(), "Rifle console resets with a pulse pending");
      Check((bus.Read(0x4213) & 0x80) != 0, "Reset releases the pending rifle pulse");
      ports.PollLightGuns();
      timing.Tick(52 * 1364 + 192 * 4 + 2);
      Check((bus.Read(0x4213) & 0x80) == 0,
            "Reset retains rifle selection and rearms its next target");
    }
  }
}

void GunSelectionAndPorts() {
  ControllerPorts ports;
  std::array<LightGunState, 2> input;
  input[0].offscreen = input[1].offscreen = false;
  input[0].x = 10;
  input[1].x = 50;
  input[0].y = input[1].y = 5;
  ports.SetGunCallback([&](int, int gun) { return input[gun]; });
  unsigned h = 0, hits = 0;
  ports.SetGunLatchCallback([&](uint16_t x, uint16_t) {
    h = x;
    ++hits;
  });
  ports.Configure(1, ControllerDevice::Justifiers);
  Latch(ports);
  ports.LatchLightGuns();
  Check(hits == 1 && h == 50, "Second gun selected after first strobe");
  ports.FrameBegin();
  Latch(ports);
  ports.LatchLightGuns();
  Check(hits == 2 && h == 10, "First gun selected after second strobe");
  ports.Configure(1, ControllerDevice::Justifier);
  Latch(ports);
  ports.LatchLightGuns();
  Check(hits == 2, "Missing second gun does not latch");
  ports.Configure(1, ControllerDevice::None);
  ports.Configure(0, ControllerDevice::Justifier);
  Latch(ports);
  ports.LatchLightGuns();
  Check(hits == 2 && Packet(ports, 32, 0) == 0x000e5508,
        "Port 1 has serial data but no PPU latch wire");
}

void AutomaticPolling() {
  Input input;
  input.guns[0].trigger = true;
  input.guns[0].offscreen = false;
  auto emu = Machine(input);
  emu->GetAutoJoypad().Ports().Configure(1, ControllerDevice::SuperScope);
  auto &bus = emu->GetBus();
  bus.Write(0x4200, 1);
  emu->StepFrame();
  emu->StepFrame(); // Automatic polling completes after the first display event.
  Check(bus.Read(0x421a) == 0xff && bus.Read(0x421b) == 0x80,
        "Automatic polling reads Scope's eight bits then ones");
  Check((bus.Read(0x4017) & 1) == 1, "Manual reader continues after automatic clocks");
}

void ScopeAimAndOffscreen() {
  ControllerPorts ports;
  LightGunState input;
  input.x = 50;
  input.y = 20;
  input.offscreen = false;
  ports.Configure(1, ControllerDevice::SuperScope);
  ports.SetGunCallback([&](int, int) { return input; });
  unsigned hits = 0;
  ports.SetGunLatchCallback([&](uint16_t, uint16_t) { ++hits; });
  ports.LatchLightGuns();
  Check(hits == 1, "Scope latches aim without a button press");
  ports.SetPio(0x7f);
  ports.LatchLightGuns();
  Check(hits == 2, "Scope frame latching is independent of WRIO input gating");
  ports.BeamPosition(90, 21, 224);
  Check(hits == 2 && ports.IoBits() == 0xff, "Scope does not emit an optical raster pulse");
  input.trigger = input.cursor = true;
  Latch(ports);
  Check(Packet(ports, 8) == 0xc0, "Scope button sampling is independent of frame aim sampling");
  input.offscreen = true;
  ports.LatchLightGuns();
  Latch(ports);
  Check(Packet(ports, 8) == 0x42, "Offscreen Scope preserves cursor and suppresses trigger");
  input.trigger = false;
  Latch(ports);
  Check(Packet(ports, 8) == 0x42, "Offscreen status refreshes without a shot");
  input.trigger = true;
  Latch(ports);
  Check(Packet(ports, 8) == 0x42, "An offscreen press still locks the normal trigger");
  input.offscreen = false;
  ports.LatchLightGuns();
  Latch(ports);
  Check(Packet(ports, 8) == 0x40, "Moving onscreen does not retrigger a held normal shot");
  input.turbo = true;
  Latch(ports);
  Check(Packet(ports, 8) == 0xe0, "Turbo allows a held trigger to fire again");
  for (auto position :
       {std::pair{-1, 20}, std::pair{256, 20}, std::pair{50, -1}, std::pair{50, 225}}) {
    input.x = position.first;
    input.y = position.second;
    const unsigned before = hits;
    ports.LatchLightGuns();
    Latch(ports);
    Check(hits == before && Packet(ports, 8) == 0x62,
          "Out-of-range Scope coordinates suppress even a turbo shot");
  }
  input.x = 255;
  for (unsigned vdisp : {225u, 240u}) {
    ports.SetVDisp(vdisp);
    input.y = vdisp - 1;
    const unsigned before = hits;
    ports.LatchLightGuns();
    Latch(ports);
    Check(hits == before + 1 && Packet(ports, 8) == 0xe0,
          "Scope accepts the last coordinate before VBlank in either display height");
  }
  ports.SetVDisp(225);
  Latch(ports);
  Check(Packet(ports, 8) == 0x62,
        "A display-height change updates Scope offscreen status before the next frame latch");
}

void FrameEventLatches() {
  for (bool pal : {false, true}) {
    for (bool overscan : {false, true}) {
      for (auto device : {ControllerDevice::SuperScope, ControllerDevice::Justifier,
                          ControllerDevice::Justifiers}) {
        for (bool pioHigh : {false, true}) {
          Input input;
          input.guns[0].x = 32;
          input.guns[0].y = 10;
          input.guns[0].offscreen = false;
          auto machine = Machine(input, pal);
          auto &bus = machine->GetBus();
          machine->GetAutoJoypad().Ports().Configure(1, device);
          for (bool warm : {false, true}) {
            if (warm)
              Check(machine->Reset(), "Light-gun console resets");
            bus.Write(0x2133, overscan ? 4 : 0);
            bus.Write(0x4201, pioHigh ? 0xff : 0x7f);
            bus.Read(0x213f);
            while (machine->GetTiming().VCounter() < 200)
              machine->GetCpu()->Step();
            Check(Counter(bus, 0x213c) == 0 && Counter(bus, 0x213d) == 0,
                  "Scope and Justifier aim does not latch during the visible raster");
            machine->StepFrame();
            Check(machine->GetTiming().VCounter() == (overscan ? 240 : 225) &&
                      machine->GetTiming().HCounter() == 0,
                  "Light-gun test stops at the display event");
            bus.Read(0x213f);
            Check(
                Counter(bus, 0x213c) == 32 && Counter(bus, 0x213d) == 10,
                "The display event captures direct aim coordinates without firing or WRIO gating");
            Check((bus.Read(0x4213) & 0x80) == (pioHigh ? 0x80 : 0),
                  "Frame-event gun latching does not pulse the I/O pin");
          }
        }
      }
    }
  }
}

std::vector<uint8_t> SplitPacketProgram(bool reverse, bool pal, uint8_t display, uint8_t pio) {
  std::vector<uint8_t> code;
  const auto emit = [&](std::initializer_list<uint8_t> bytes) {
    code.insert(code.end(), bytes.begin(), bytes.end());
  };
  const auto store = [&](uint16_t address, uint8_t value) {
    emit({0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  const auto wait = [&](uint8_t branch) { emit({0xad, 0x12, 0x42, branch, 0xfb}); };
  store(0x2100, 0x80);
  store(0x2133, display);
  store(0x4201, pio);
  if (reverse)
    wait(0x10);
  store(0x4016, 1);
  store(0x4016, 0);
  emit({0xa2, 6, 0xad, 0x17, 0x40, 0xca, 0xd0, 0xfa}); // Read six bits.
  if (reverse)
    wait(0x30);
  wait(0x10);
  emit({0xad, 0x17, 0x40, 0x29, 1, 0x8f, 0, 1, 0x7e, 0xa9, 0xa5, 0x8f, 1, 1, 0x7e, 0xdb});
  std::vector<uint8_t> rom(0x8000, 0xea);
  std::copy(code.begin(), code.end(), rom.begin());
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0x7fc0] = 'T';
  rom[0x7fd5] = 0x20;
  rom[0x7fd6] = 0;
  rom[0x7fd7] = 5;
  rom[0x7fd8] = 0;
  rom[0x7fd9] = pal ? 2 : 1;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7fde] = rom[0x7fdf] = 0;
  rom[0x7ffc] = 0;
  rom[0x7ffd] = 0x80;
  return rom;
}

void ScopePacketAcrossFrame() {
  for (bool reverse : {false, true}) {
    for (bool pal : {false, true}) {
      for (uint8_t display : {0, 1, 4, 5}) {
        for (uint8_t pio : {0x7f, 0xff}) {
          Input input;
          input.guns[0].x = -1;
          input.guns[0].y = 10;
          input.guns[0].offscreen = false;
          auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
          machine->AttachInputProvider(&input);
          Check(machine->LoadCartridge(SplitPacketProgram(reverse, pal, display, pio)),
                "Scope split-packet program loads");
          machine->GetAutoJoypad().Ports().Configure(1, ControllerDevice::SuperScope);
          machine->StepFrame();
          if (reverse)
            input.guns[0].x = 32;
          machine->StepFrame();
          if (reverse)
            machine->StepFrame();
          Check(machine->GetBus().Read(0x7e0100) == (reverse ? 0 : 1) &&
                    machine->GetBus().Read(0x7e0101) == 0xa5,
                "Scope offscreen status follows a frame event during an unfinished CPU packet");
        }
      }
    }
  }
}

void ScopeStatusHeightSampling() {
  ControllerPorts ports;
  LightGunState input;
  input.x = 100;
  input.y = 239;
  input.offscreen = false;
  ports.Configure(1, ControllerDevice::SuperScope);
  ports.SetGunCallback([&](int, int) { return input; });
  ports.SetVDisp(240);
  ports.LatchLightGuns();
  Latch(ports);
  Check(Packet(ports, 6) == 0, "Scope begins an onscreen packet in overscan");
  ports.SetVDisp(225);
  Check(ports.Read(1) == 0,
        "A height write alone retains the sampled Scope status within a packet");
  Latch(ports);
  Check(Packet(ports, 8) == 2, "The next Scope packet samples the new display height");
}
} // namespace
int main() {
  try {
    ScopePackets();
    ScopeReadBoundaries();
    ScopeTurboSampling();
    JustifierPackets();
    JustifierReadBoundaries();
    RiflePackets();
    RifleBeamLatches();
    RifleDeviceLifetimes();
    GunSelectionAndPorts();
    AutomaticPolling();
    ScopeAimAndOffscreen();
    FrameEventLatches();
    ScopePacketAcrossFrame();
    ScopeStatusHeightSampling();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("Light gun protocol, beam, and automatic polling checks passed");
  return 0;
}
