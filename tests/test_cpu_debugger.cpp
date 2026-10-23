#include "ConsoleConfig.hpp"
#include "debug/CpuDebugger.hpp"
#include "snes/core/SnesCpu.hpp"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace snes::core;
using snes::frontend::CpuDebugger;
namespace {
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class Function> void Reject(Function function) {
  bool rejected = false;
  try {
    function();
  } catch (const std::exception &) {
    rejected = true;
  }
  Check(rejected, "Invalid debugger action rejects");
}
std::vector<uint8_t> Rom(bool pal = false) {
  std::vector<uint8_t> rom(0x8000, 0xea);
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0x7fc0] = 'D';
  rom[0x7fd5] = 0x20;
  rom[0x7fd6] = 0;
  rom[0x7fd7] = 5;
  rom[0x7fd8] = 0;
  rom[0x7fd9] = pal ? 2 : 1;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7fde] = rom[0x7fdf] = 0;
  rom[0x7ffc] = 0;
  rom[0x7ffd] = 0x80;
  // LDA #$5A; STA $7E1234; LDA $7E1234; INC A; BRA $8002.
  const std::vector<uint8_t> program{0xa9, 0x5a, 0x8f, 0x34, 0x12, 0x7e, 0xaf,
                                     0x34, 0x12, 0x7e, 0x1a, 0x80, 0xf5};
  std::copy(program.begin(), program.end(), rom.begin());
  return rom;
}
auto Machine(bool pal = false) {
  auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Check(machine->LoadCartridge(Rom(pal)), "Debugger fixture loads");
  return machine;
}
void Drain(CpuDebugger &debugger) {
  for (unsigned count = 0; count < 100000 && debugger.CurrentMode() != CpuDebugger::Mode::Paused;
       ++count)
    debugger.Pump(32);
  Check(debugger.CurrentMode() == CpuDebugger::Mode::Paused, "Debugger reaches its stop");
}
void Controls() {
  auto machine = Machine();
  CpuDebugger debugger(*machine);
  const auto clock = machine->CurrentMasterCycles();
  debugger.Registers();
  Check(clock == machine->CurrentMasterCycles(), "Reading registers never advances hardware");
  debugger.AddBreakpoint({0x8002, 0x8002});
  debugger.Capture(true);
  debugger.Run();
  Reject([&] { debugger.AddBreakpoint({0, 0}); });
  Drain(debugger);
  Check(debugger.Registers().pc == 0x8002 && debugger.Registers().a == 0x5a &&
            debugger.LastHit()->kind == CpuDebugger::Hit::Kind::Execute,
        "Execution breakpoint stops before the instruction");
  Check(machine->GetBus().WramData()[0x1234] == 0, "The stopped instruction has not written RAM");
  debugger.Run();
  Drain(debugger);
  Check(debugger.Registers().a == 0x5b && machine->GetBus().WramData()[0x1234] == 0x5a,
        "Continuing skips the stopped breakpoint once, then catches the next visit");
  debugger.Step();
  Drain(debugger);
  Check(debugger.Registers().pc == 0x8006 && machine->GetBus().WramData()[0x1234] == 0x5b,
        "Single step completes one instruction despite an execution breakpoint");
  debugger.RemoveBreakpoint(0);
  debugger.AddBreakpoint({0x7e1234, 0x7e1234, false, true, false});
  debugger.Run();
  Drain(debugger);
  Check(debugger.LastHit()->kind == CpuDebugger::Hit::Kind::Read &&
            debugger.LastHit()->value == 0x5b && debugger.Registers().pc == 0x800a,
        "Read watchpoint records the sampled byte and stops at the completed instruction");
  debugger.RemoveBreakpoint(0);
  debugger.AddBreakpoint({0x7e1230, 0x7e123f, false, false, true});
  debugger.Run();
  Drain(debugger);
  Check(debugger.LastHit()->kind == CpuDebugger::Hit::Kind::Write &&
            debugger.LastHit()->address == 0x7e1234 && debugger.LastHit()->value == 0x5c &&
            debugger.Registers().pc == 0x8006,
        "Write watchpoint supports ranges and reports the first matching access");
  debugger.EnableBreakpoint(0, false);
  debugger.Run(0x800a);
  Drain(debugger);
  Check(debugger.Registers().pc == 0x800a,
        "Run to address stops independently of disabled entries");
  const auto text = debugger.ExportTrace();
  Check(text.find("write\t") != std::string::npos && text.find("7E1234\t5C") != std::string::npos &&
            text.find("boundary\t") != std::string::npos,
        "Export contains observed bytes and boundary register snapshots");
  Reject([&] { debugger.AddBreakpoint({0x1000000, 0x1000000}); });
  Reject([&] { debugger.AddBreakpoint({8, 4}); });
  Reject([&] { debugger.AddBreakpoint({0, 0, false, false, false}); });
  Reject([&] { debugger.Run(0x1000000); });
  Reject([&] { debugger.RemoveBreakpoint(500); });
  Reject([&] { debugger.TraceAt(debugger.TraceSize()); });
  debugger.Run();
  Check(!debugger.Pump(0), "A zero event budget performs no work");
  debugger.Pump(10000);
  debugger.Pause();
  Check(debugger.TraceSize() == CpuDebugger::traceCapacity && debugger.DroppedTraceEntries() > 0,
        "Trace capture is bounded and reports overwritten entries");
  for (size_t index = 1; index < debugger.TraceSize(); ++index)
    Check(debugger.TraceAt(index - 1).cycle <= debugger.TraceAt(index).cycle,
          "Ring buffer retains chronological order after wrapping");
  debugger.ClearTrace();
  Check(debugger.TraceSize() == 0 && debugger.DroppedTraceEntries() == 0,
        "Clear resets trace counters");
}
struct Outputs : IVideoOutput, IAudioOutput, IInputProvider {
  unsigned fields = 0, blocks = 0, polls = 0, advanced = 0;
  uint64_t hash = 1469598103934665603ull;
  void Add(uint64_t value) {
    hash = (hash ^ value) * 1099511628211ull;
  }
  void Present(const VideoFrame &frame) override {
    ++fields;
    Add(frame.width);
    Add(frame.height);
    for (auto value : frame.pixels)
      Add(value);
  }
  void Submit(const AudioBuffer &buffer) override {
    ++blocks;
    Add(buffer.sampleRate);
    for (auto value : buffer.interleavedStereo)
      Add(std::bit_cast<uint32_t>(value));
  }
  InputState Poll(uint64_t frame) override {
    ++polls;
    return {.a = bool(frame & 1)};
  }
  void AdvanceFrame() noexcept override {
    ++advanced;
  }
};
void FieldEquivalence() {
  for (bool pal : {false, true}) {
    auto normal = Machine(pal), stepped = Machine(pal);
    Outputs a, b;
    normal->AttachVideoOutput(&a);
    normal->AttachAudioOutput(&a);
    normal->AttachInputProvider(&a);
    stepped->AttachVideoOutput(&b);
    stepped->AttachAudioOutput(&b);
    stepped->AttachInputProvider(&b);
    for (unsigned field = 0; field < 5; ++field) {
      const auto expected = normal->StepFrame();
      InstructionStepResult event;
      size_t count = 0;
      do {
        event = stepped->StepInstruction();
        Check(++count < 1000000, "Stepping eventually returns a video field");
      } while (!event.field);
      Check(event.field->masterCycles == expected.masterCycles && a.hash == b.hash &&
                a.fields == b.fields && a.blocks == b.blocks && a.polls == b.polls &&
                a.advanced == b.advanced,
            "Instruction stepping preserves every display event, PCM block and input poll");
    }
    Check(normal->CaptureState() == stepped->CaptureState(),
          "Mixed execution modes retain exact serialized machine state");
  }
  Emulator idle;
  Reject([&] { idle.StepInstruction(); });
  Reject([&] { CpuDebugger debugger(idle); });
}
void LongDma() {
  auto machine = Machine();
  unsigned writes = 0;
  machine->GetBus().Map(
      0, 0, 0x21e0, 0x21e0, [](uint32_t, uint8_t value) { return value; },
      [&](uint32_t, uint8_t) { ++writes; });
  for (unsigned index = 0; index < 8; ++index) {
    auto &channel = machine->GetDma().Channel(index);
    channel.writeControl(8);
    channel.sourceBank = 0x7e;
    channel.sourceAddress = 0;
    channel.targetAddress = 0xe0;
    channel.transferSize = 0;
  }
  machine->GetDma().EnableDma(0xff);
  Outputs output;
  machine->AttachVideoOutput(&output);
  machine->AttachAudioOutput(&output);
  CpuDebugger debugger(*machine);
  debugger.AddBreakpoint({0x21e0, 0x21e0, false, false, true});
  debugger.Step();
  auto field = debugger.Pump(1);
  Check(field && writes > 0 && writes < 65536 && !debugger.AtBoundary(),
        "A debugger step yields display events inside long DMA");
  debugger.Pause();
  const auto pausedWrites = writes;
  debugger.Pump();
  Check(writes == pausedWrites, "Manual pause retains a live DMA operation");
  debugger.Step();
  Drain(debugger);
  Check(writes == 8 * 65536 && debugger.AtBoundary() && output.fields > 10 &&
            output.fields == output.blocks &&
            debugger.LastHit()->kind == CpuDebugger::Hit::Kind::Write,
        "Resumed stepping delivers all fields and completes each DMA byte once before a watchpoint "
        "stop");
  const auto fields = output.fields;
  machine->StepFrame();
  Check(output.fields == fields + 1, "Normal stepping resumes after debugger control");
}
struct Sink : ITraceSink {
  unsigned calls = 0;
  void OnTrace(const TraceEvent &) override {
    ++calls;
  }
};
void ObserverLifetime() {
  auto machine = Machine();
  Sink sink;
  machine->AddTraceSink(&sink);
  machine->AddTraceSink(&sink);
  machine->GetBus().Read(0x7e0000);
  Check(sink.calls == 1, "A bus read notifies a sink once despite repeated registration");
  machine->GetBus().WriteCheat(0x7e0000, 1);
  Check(sink.calls == 1, "Host patch writes are excluded from CPU bus tracing");
  {
    CpuDebugger debugger(*machine);
    debugger.Run();
    debugger.Pump(1);
  }
  const auto before = sink.calls;
  machine->StepInstruction();
  Check(sink.calls > before, "Destroying a debugger retains other trace subscriptions");
  machine->RemoveTraceSink(&sink);
  const auto detached = sink.calls;
  machine->StepFrame();
  Check(sink.calls == detached, "Detached sinks are never called");
}
void StoppedCpu() {
  auto image = Rom();
  image[0] = 0xdb;
  auto machine = Machine();
  Check(machine->LoadCartridge(image), "Stopped CPU fixture loads");
  CpuDebugger debugger(*machine);
  debugger.Step();
  Drain(debugger);
  debugger.Step();
  Drain(debugger);
  Check(debugger.Registers().stp && debugger.Registers().pc == 0x8001, "Fixture stops after STP");
  debugger.AddBreakpoint({0x8001, 0x8001});
  debugger.Capture(true);
  debugger.Run();
  debugger.Pump(1000);
  Check(debugger.CurrentMode() == CpuDebugger::Mode::Run && !debugger.LastHit(),
        "An execution breakpoint never mistakes stopped CPU cycles for instruction fetches");
  debugger.Pause();
  debugger.Repositioned();
  Check(debugger.Breakpoints().size() == 1 && debugger.Capturing() && debugger.TraceSize() == 0 &&
            !debugger.LastHit(),
        "A changed execution position retains settings and clears old hits and trace");
  for (size_t index = 1; index < CpuDebugger::breakpointLimit; ++index)
    debugger.AddBreakpoint({0, 0});
  Reject([&] { debugger.AddBreakpoint({0, 0}); });
}
} // namespace
int main() {
  try {
    Controls();
    FieldEquivalence();
    LongDma();
    ObserverLifetime();
    StoppedCpu();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("CPU debugger checks passed");
}
