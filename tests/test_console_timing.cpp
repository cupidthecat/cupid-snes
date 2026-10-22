#include "snes/core/AutoJoypad.hpp"
#include "snes/core/IrqController.hpp"
#include "snes/core/SnesCpu.hpp"
#include "snes/core/Timing.hpp"

#include <array>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace snes::core;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct PhaseBus final : ICpuBus {
  uint64_t elapsed = 0;
  uint64_t accessedAt = 0;
  uint8_t speed = 6;
  uint8_t Read(uint32_t) override {
    accessedAt = elapsed;
    return 0x5a;
  }
  void Write(uint32_t, uint8_t) override {
    accessedAt = elapsed;
  }
  uint8_t Speed(uint32_t) const override {
    return speed;
  }
};

void CpuReadEdges() {
  for (uint8_t speed : {6, 8, 12}) {
    PhaseBus bus;
    bus.speed = speed;
    SnesCpu cpu(bus);
    cpu.SetClockCallback([&](uint32_t clocks) { bus.elapsed += clocks; });
    Check(cpu.read(0x2100) == 0x5a, "Read returns the sampled device value");
    Check(bus.accessedAt == speed - 4, "CPU read samples four clocks before its cycle ends");
    Check(bus.elapsed == speed, "Read advances exactly one complete bus cycle");
  }
}

void CpuWriteEdges() {
  for (uint8_t speed : {6, 8, 12}) {
    PhaseBus bus;
    bus.speed = speed;
    SnesCpu cpu(bus);
    cpu.SetClockCallback([&](uint32_t clocks) { bus.elapsed += clocks; });
    cpu.write(0x2100, 0xa5);
    Check(bus.accessedAt == speed, "CPU write reaches the device at the end of its cycle");
    Check(bus.elapsed == speed, "Write advances exactly one complete bus cycle");
  }
}

void JoypadClockEdges() {
  for (bool split : {false, true}) {
    Timing timing;
    std::vector<uint64_t> edges;
    timing.onJoypadPoll = [&](uint16_t, uint16_t, uint16_t) {
      edges.push_back(timing.MasterClocksElapsed());
    };
    if (split) {
      for (unsigned i = 0; i < 512; ++i)
        timing.Tick(1);
    } else {
      timing.Tick(512);
    }
    Check(edges == std::vector<uint64_t>{128, 256, 384, 512},
          "Joypad edges follow the global 128-clock divider for every batch size");
  }
}

void BeamDotEdges() {
  Timing timing;
  timing.Tick(1292);
  Check(timing.HDot() == 323, "First stretched dot begins at clock 1292");
  timing.Tick(18);
  Check(timing.HDot() == 327, "Second stretched dot begins at clock 1310");

  Timing pal(Region::PAL);
  pal.SetInterlace(true);
  pal.Tick((313 + 311) * 1364);
  Check(pal.HPeriod() == 1368, "Test reaches the long PAL line");
  pal.Tick(1296);
  Check(pal.HDot() == 323, "Long PAL lines retain the stretched beam dots");
}

void InterlaceSampling() {
  Timing timing(Region::PAL);
  timing.SetInterlace(true);
  timing.Tick((313 + 128) * 1364);
  timing.SetInterlace(false);
  timing.Tick((311 - 128) * 1364);
  Check(timing.HPeriod() == 1368,
        "Changing SETINI after line 128 cannot alter the current field's line lengths");
}

void IrqFieldHistory() {
  IrqController irq;
  irq.SetVTime(0);
  irq.NmitimenUpdate(0x20);
  irq.Poll(2, 0, 225, 1364);
  Check(!irq.IrqLine(), "V IRQ at line zero waits for the delayed counter to enter the field");

  irq.Reset();
  irq.SetVTime(261);
  irq.NmitimenUpdate(0x20);
  irq.Poll(2, 0, 225, 1364);
  Check(irq.IrqLine(), "Delayed vertical counter retains the previous field's final line");

  irq.Reset();
  irq.SetVTime(261);
  irq.NmitimenUpdate(0x20);
  irq.Poll(6, 0, 225, 1364);
  Check(!irq.IrqLine(),
        "IRQ guard uses the six-clock counter independently of the ten-clock comparison");
}

void IrqLineHistory() {
  for (uint16_t target : {338, 339}) {
    Timing timing;
    IrqController irq;
    timing.Tick(262 * 1364 + 240 * 1364);
    timing.Tick(10);
    irq.SetHTime(target);
    irq.NmitimenUpdate(0x10);
    timing.onIrqPoll = [&](uint16_t h, uint16_t v, uint16_t vdisp, uint16_t) {
      irq.Poll(h, v, vdisp, timing.PreviousHPeriod(), timing.PreviousVPeriod());
    };
    timing.Tick(1360 - 10 + 6);
    Check(irq.IrqLine() == (target == 338),
          "IRQ comparisons carry the short line's width into the next scanline");
  }

  Timing timing(Region::PAL);
  IrqController irq;
  timing.SetInterlace(true);
  timing.Tick((313 + 311) * 1364);
  irq.SetHTime(339);
  irq.SetVTime(311);
  irq.NmitimenUpdate(0x30);
  timing.onIrqPoll = [&](uint16_t h, uint16_t v, uint16_t vdisp, uint16_t) {
    irq.Poll(h, v, vdisp, timing.PreviousHPeriod(), timing.PreviousVPeriod());
  };
  timing.Tick(1368 + 2);
  Check(irq.IrqLine(),
        "HV IRQ can finish its communication delay across the long PAL field boundary");
  Check(timing.PreviousHPeriod() == 1368 && timing.PreviousVPeriod() == 312,
        "Long-line and field history remain available after wrap");

  timing.Reset();
  timing.SetInterlace(true);
  timing.Tick(313 * 1364 + 2);
  irq.Reset();
  irq.SetVTime(312);
  irq.NmitimenUpdate(0x20);
  irq.Poll(timing.HCounter(), timing.VCounter(), timing.VDisp(), timing.PreviousHPeriod(),
           timing.PreviousVPeriod());
  Check(irq.IrqLine(), "Delayed V IRQ retains an interlaced field's extra scanline");
}

void AutomaticInputEdges() {
  for (Region region : {Region::NTSC, Region::PAL}) {
    Timing timing(region);
    AutoJoypad joypad;
    joypad.SetAutoJoypadPoll(true);
    joypad.SetInputCallback([](int port) {
      InputState input;
      input.b = port == 0;
      return input;
    });
    std::vector<uint64_t> starts;
    timing.onFrameBegin = [&]() { joypad.FrameBegin(); };
    timing.onJoypadPoll = [&](uint16_t h, uint16_t v, uint16_t vdisp) {
      joypad.Tick128(h, v, vdisp, (timing.MasterClocksElapsed() & 255) == 0);
      if (joypad.Counter() == 0) {
        starts.push_back(timing.MasterClocksElapsed());
        Check(v == vdisp && h >= 130 && h <= 384,
              "Automatic input starts inside the VBlank latch interval");
        Check((starts.back() & 255) == 0, "Input latch uses the global 256-clock phase");
      }
    };
    const unsigned lines = region == Region::NTSC ? 262 : 312;
    timing.Tick(4 * lines * 1364);
    Check(starts.size() == 4 && joypad.Joy1() == 0x8000,
          "Input polling completes on successive fields in both video regions");
  }
}

void LatchEnableChanges() {
  AutoJoypad joypad;
  joypad.SetInputCallback([](int) {
    InputState input;
    input.b = true;
    return input;
  });
  joypad.SetAutoJoypadPoll(true);
  joypad.Tick128(256, 225, 225, true);
  joypad.SetAutoJoypadPoll(false);
  Check(joypad.Ports().Read(0) == 1 && joypad.Ports().Read(0) == 0,
        "Disabling automatic input during counter zero releases the serial latch immediately");
  joypad.SetAutoJoypadPoll(true);
  Check(joypad.Ports().Read(0) == 1 && joypad.Ports().Read(0) == 1,
        "Reenabling automatic input during counter zero relatches the controller");
  joypad.Tick128(384, 225, 225, false);
  Check(joypad.Counter() == 1, "Counter one releases the latch on the next 128-clock edge");
  Check(joypad.Ports().Read(0) == 1 && joypad.Ports().Read(0) == 0,
        "The released serial latch advances on manual reads");
}

void ScanlineEventSchedule() {
  using Event = std::tuple<char, uint64_t, uint16_t, uint16_t>;
  const auto capture = [](bool split) {
    Timing timing;
    std::vector<Event> events;
    const auto record = [&](char kind) {
      events.emplace_back(kind, timing.MasterClocksElapsed(), timing.HCounter(), timing.VCounter());
    };
    timing.onHdmaSetup = [&]() { record('S'); };
    timing.onDramRefresh = [&]() { record('D'); };
    timing.onHBlank = [&](uint16_t) { record('B'); };
    timing.onHdmaTransfer = [&](uint16_t) { record('H'); };
    timing.onScanline = [&](uint16_t) { record('L'); };
    timing.onRenderCycle = [&](uint16_t) { record('R'); };
    timing.onNmiPoint = [&]() { record('N'); };
    if (split) {
      for (unsigned i = 0; i < 2728; ++i)
        timing.Tick(1);
    } else {
      timing.Tick(2728);
    }
    return events;
  };
  const std::vector<Event> expected{
      {'S', 12, 12, 0},     {'D', 538, 538, 0},  {'B', 1096, 1096, 0}, {'H', 1104, 1104, 0},
      {'L', 1364, 0, 1},    {'R', 1876, 512, 1}, {'D', 1898, 534, 1},  {'B', 2460, 1096, 1},
      {'H', 2468, 1104, 1}, {'L', 2728, 0, 2}};
  Check(capture(false) == expected && capture(true) == expected,
        "Scanline callbacks retain their exact positions and ordering for arbitrary clock batches");

  Timing timing;
  timing.SetVDisp(2);
  unsigned renders = 0;
  timing.onRenderCycle = [&](uint16_t) { ++renders; };
  timing.Tick(2 * 1364 + 600);
  Check(renders == 1, "The old VBlank line has no render event");
  timing.SetVDisp(3);
  timing.Tick(2);
  Check(renders == 2, "Changing the display boundary reevaluates events on the next timing edge");
}

void TimingBatchEdges() {
  using Event = std::array<uint64_t, 11>;
  for (auto region : {Region::NTSC, Region::PAL}) {
    for (uint8_t revision : {uint8_t{1}, uint8_t{2}}) {
      for (bool interlace : {false, true}) {
        for (bool nested : {false, true}) {
          const auto capture = [&](bool split) {
            Timing timing(region, revision);
            timing.SetInterlace(interlace);
            std::vector<Event> events;
            bool active = false;
            const auto state = [&](char kind) -> Event {
              return {uint64_t(kind),
                      timing.MasterClocksElapsed(),
                      timing.HCounter(),
                      timing.VCounter(),
                      timing.HPeriod(),
                      timing.VPeriod(),
                      timing.PreviousHPeriod(),
                      timing.PreviousVPeriod(),
                      timing.FrameCount(),
                      timing.Field(),
                      timing.HDot()};
            };
            const auto record = [&](char kind) {
              events.push_back(state(kind));
              if (nested && !active && kind == 'R' && timing.VCounter() % 47 == 0) {
                active = true;
                timing.Tick(6);
                active = false;
                events.push_back(state('r'));
              }
            };
            timing.onFrameBegin = [&] {
              record('F');
              timing.SetVDisp(timing.FrameCount() & 1 ? 240 : 225);
            };
            timing.onHdmaSetup = [&] { record('S'); };
            timing.onScanline = [&](uint16_t v) {
              record('L');
              if (v % 43 == 0)
                timing.SetRenderCycle(std::array<uint16_t, 3>{2, 531, 1105}[v % 3]);
            };
            timing.onRenderCycle = [&](uint16_t) { record('R'); };
            timing.onVBlankBegin = [&] { record('V'); };
            timing.onHBlank = [&](uint16_t) { record('B'); };
            timing.onNmiPoint = [&] { record('N'); };
            timing.onDramRefresh = [&] { record('D'); };
            timing.onHdmaTransfer = [&](uint16_t) { record('H'); };
            timing.onIrqPoll = [&](uint16_t, uint16_t, uint16_t, uint16_t) { record('I'); };
            timing.onJoypadPoll = [&](uint16_t, uint16_t, uint16_t) { record('J'); };
            const unsigned clocks = (region == Region::PAL ? 626u : 526u) * 1364 + 129;
            if (split) {
              for (unsigned clock = 0; clock < clocks; ++clock)
                timing.Tick(1);
            } else {
              timing.Tick(clocks);
            }
            events.push_back(state('E'));
            return events;
          };
          Check(
              capture(false) == capture(true),
              "Timing batches retain every callback and counter across regional field boundaries");
        }
      }
    }
  }
}
} // namespace

int main() {
  const std::array<std::pair<const char *, std::function<void()>>, 11> tests{{
      {"CPU read phases", CpuReadEdges},
      {"CPU write phases", CpuWriteEdges},
      {"controller divider", JoypadClockEdges},
      {"beam dot edges", BeamDotEdges},
      {"interlace sampling", InterlaceSampling},
      {"IRQ field history", IrqFieldHistory},
      {"IRQ line history", IrqLineHistory},
      {"automatic input edges", AutomaticInputEdges},
      {"latch enable changes", LatchEnableChanges},
      {"scanline event schedule", ScanlineEventSchedule},
      {"timing batch edges", TimingBatchEdges},
  }};
  unsigned failures = 0;
  for (const auto &[name, run] : tests) {
    try {
      run();
      std::printf("PASS: %s\n", name);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "FAIL: %s: %s\n", name, error.what());
      ++failures;
    }
  }
  return failures ? 1 : 0;
}
