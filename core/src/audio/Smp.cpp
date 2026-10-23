// SMP memory, timers, ports, and synchronization with the DSP.

// Provides the SPC700's bus interface: 64KB RAM, IPL ROM overlay,
// I/O registers ($00F0-$00FF), and bidirectional CPU communication ports.
//

#include "snes/core/Smp.hpp"
#include "snes/core/Dsp.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace snes::core {

namespace {
bool SmpPortTraceEnabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char *v = std::getenv("SNES_SMP_PORT_TRACE");
    enabled = (v && *v && *v != '0') ? 1 : 0;
  }
  return enabled == 1;
}
} // namespace

// IPL boot ROM - 64 bytes loaded at $FFC0-$FFFF when enabled
// This is the standard SPC700 boot ROM that handles the initial CPU↔APU
// handshake and data transfer protocol.
static constexpr uint8_t kIplRom[64] = {
    0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0, 0xFC, 0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5, 0x78,
    0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4, 0xD0, 0xFC, 0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5,
    0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3, 0xAB, 0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA,
    0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4, 0xDD, 0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF,
};

// Timer implementation

template <unsigned Frequency>
void Smp::Timer<Frequency>::Step(unsigned clocks, bool timersEnable, bool timersDisable) {
  // Preserve every divider edge, including a batched step crossing more than
  // one period or exceeding the eight-bit accumulator's range.
  const uint64_t total = uint64_t(stage0) + clocks;
  stage0 = uint8_t(total % Frequency);
  for (uint64_t edges = total / Frequency; edges; --edges) {
    stage1 ^= 1;
    SynchronizeStage1(timersEnable, timersDisable);
  }
}

template <unsigned Frequency>
void Smp::Timer<Frequency>::SynchronizeStage1(bool timersEnable, bool timersDisable) {
  // Gate the stage1 signal with global enable/disable from TEST register
  bool level = (stage1 != 0);
  if (!timersEnable)
    level = false;
  if (timersDisable)
    level = false;

  // Falling-edge detection: only trigger on 1→0 transition
  bool fallingEdge = line && !level;
  line = level;
  if (!fallingEdge)
    return;

  // Stage 2: divider counter (only advances when timer is enabled)
  if (!enable)
    return;
  stage2++;
  if (stage2 != target)
    return;

  // Stage 3: 4-bit output counter
  stage2 = 0;
  stage3 = (stage3 + 1) & 0x0F;
}

// Explicit template instantiations
template struct Smp::Timer<128>;
template struct Smp::Timer<16>;

// Constructor / Power

Smp::Smp() {
  std::memcpy(iplRom_.data(), kIplRom, IplRomSize);
  Power();
}

void Smp::Power(bool retainRam) {
  // Reset the SPC700 processor core
  Spc700::Power();

  if (!retainRam)
    ram_.fill(0);

  // Reset I/O state to power-on defaults
  io_ = IO{}; // uses default member initializers

  // Clear port latches
  apuInput_.fill(0);
  cpuOutput_.fill(0);

  // Reset timers
  timer0_ = {};
  timer1_ = {};
  timer2_ = {};
  busWait_ = {};
  hardwareClocks_ = 0;
  dspClockBalance_ = 0;
  synchronizingCpu_ = yieldedCpu_ = false;
}

// Bus interface - Spc700 overrides

void Smp::Idle() {
  BusWait wait;
  while (!ClockBusCycle(PendingCycle{PendingCycleKind::Idle}, wait)) {
  }
}

uint8_t Smp::Read(uint16_t address) {
  BusWait wait;
  uint8_t result = 0;
  const PendingCycle cycle{PendingCycleKind::Read, address, 0, &result};
  while (!ClockBusCycle(cycle, wait)) {
  }
  return result;
}

uint8_t Smp::ReadBus(uint16_t address) {
  uint8_t data = readRam(address);
  // I/O registers at $00F0-$00FF override RAM reads
  if ((address & 0xFFF0) == 0x00F0) {
    data = readIO(address);
  }
  return data;
}

void Smp::Write(uint16_t address, uint8_t data) {
  BusWait wait;
  const PendingCycle cycle{PendingCycleKind::Write, address, data, nullptr};
  while (!ClockBusCycle(cycle, wait)) {
  }
}

// The DSP shares this clock. Its own phase counter determines when a sample is
// published; register and shared-RAM operations happen on the intervening clocks.

void Smp::tickDsp() {
  if (dsp_)
    dsp_->Tick();
}

// Batch execution - stop exactly at the requested complete-cycle deadline.

void Smp::RunUntil(uint64_t targetCycles) {
  RunUntilHardwareClock(targetCycles * 2);
}

void Smp::RunUntilHardwareClock(uint64_t targetClocks) {
  // StepCycle can pause an instruction inside a stretched access, keeping
  // timers and DSP on the same clock deadline even while the CPU is halted.
  while (hardwareClocks_ < targetClocks) {
    StepCycle();
  }
}

int64_t Smp::CpuClockLead() const {
  const auto distance = hardwareClocks_ >= synchronizationTarget_
                            ? int64_t(hardwareClocks_ - synchronizationTarget_)
                            : -int64_t(synchronizationTarget_ - hardwareClocks_);
  return distance * synchronizationFrequency_ - synchronizationPhase_;
}

void Smp::SetSynchronizationTarget(uint64_t masterClocks, uint32_t masterFrequency) {
  synchronizationFrequency_ = masterFrequency;
  const auto hardwareHz = HardwareClockFrequency();
  const auto fraction = (masterClocks % masterFrequency) * hardwareHz;
  synchronizationTarget_ =
      (masterClocks / masterFrequency) * hardwareHz + fraction / masterFrequency;
  synchronizationPhase_ = uint32_t(fraction % masterFrequency);
}

void Smp::SynchronizeCpu(uint64_t masterClocks, uint32_t masterFrequency) {
  SetSynchronizationTarget(masterClocks, masterFrequency);
  if (CpuClockLead() >= 0)
    return;
  synchronizingCpu_ = true;
  yieldedCpu_ = false;
  while (!yieldedCpu_)
    StepCycle();
  synchronizingCpu_ = false;
}

bool Smp::TrySynchronizeState(uint64_t masterClocks, uint32_t masterFrequency) {
  if (!InstructionInProgress())
    return true;
  SetSynchronizationTarget(masterClocks, masterFrequency);
  synchronizingCpu_ = true;
  yieldedCpu_ = false;
  try {
    while (InstructionInProgress() && CpuClockLead() < 0 && !yieldedCpu_)
      StepCycle();
  } catch (...) {
    synchronizingCpu_ = false;
    throw;
  }
  synchronizingCpu_ = false;
  return !InstructionInProgress();
}

bool Smp::ExecuteBusCycle(const PendingCycle &cycle) {
  return ClockBusCycle(cycle, busWait_);
}

bool Smp::ClockBusCycle(const PendingCycle &cycle, BusWait &wait) {
  if (!wait.duration) {
    // TEST divides internal and external accesses independently. This
    // deterministic model uses the regular ratios; some S-SMP revisions
    // have much longer internal waits or lock up at the slowest settings.
    static constexpr uint8_t durations[]{2, 4, 10, 20};
    static constexpr uint8_t timerTicks[]{2, 4, 8, 16};
    const bool internal = cycle.kind == PendingCycleKind::Idle ||
                          (cycle.address & 0xfff0) == 0x00f0 ||
                          (cycle.address >= 0xffc0 && io_.iplRomEnable);
    const unsigned setting = (internal ? io_.internalWaitStates : io_.externalWaitStates) & 3;
    wait.duration = durations[setting];
    wait.timerTicks = timerTicks[setting];
  }

  // Model the communication input latch at the midpoint and hold it through
  // completion, including accesses whose midpoint falls between full cycles.
  const bool inputPort = cycle.kind == PendingCycleKind::Read && (cycle.address & 0xfffc) == 0x00f4;
  const bool resumed = std::exchange(wait.deferred, false);
  if (resumed && inputPort && !wait.sampled && 2 * wait.elapsed >= wait.duration) {
    wait.readData = ReadBus(cycle.address);
    wait.sampled = true;
  }
  if (wait.elapsed < wait.duration) {
    ++wait.elapsed;
    ++hardwareClocks_;
    cycles_ = hardwareClocks_ / 2;
    // Stretched accesses clock the timers on four of every five hardware edges.
    StepTimers(wait.timerTicks == wait.duration || wait.elapsed % 5 != 1 ? 1 : 0);
    // The DSP starts on the first half-cycle, then clocks on alternate edges.
    if (dspMode_ == DspMode::PerCycle && (hardwareClocks_ & 1))
      tickDsp();
    if (dspMode_ == DspMode::Batch32)
      --dspClockBalance_;
  }

  const bool midpoint = inputPort && !wait.sampled && 2 * wait.elapsed >= wait.duration;
  const bool finished = wait.elapsed == wait.duration;
  if (dspMode_ == DspMode::Batch32 && cycle.kind != PendingCycleKind::Idle &&
      (midpoint || finished)) {
    while (dspClockBalance_ < 0) {
      for (unsigned clock = 0; clock < 32; ++clock)
        tickDsp();
      dspClockBalance_ += 64;
    }
  }
  const bool outputPort =
      cycle.kind == PendingCycleKind::Write &&
      ((cycle.address & 0xfffc) == 0x00f4 || (cycle.address == 0x00f1 && (cycle.data & 0x30)));
  // Communication writes reach shared RAM before yielding to the console.
  // The port latch changes only after that handoff returns.
  if (finished && outputPort && !wait.wroteRam) {
    writeRam(cycle.address, cycle.data);
    wait.wroteRam = true;
  }
  // Keep the bus operation suspended before changing a communication latch.
  // Bound execution even when a program never touches those latches.
  constexpr int64_t maximumClockLead = 442'368'000'000;
  if (synchronizingCpu_ && !resumed && cycle.kind != PendingCycleKind::Idle &&
      (midpoint || finished) &&
      (((midpoint || (finished && outputPort)) && CpuClockLead() >= 0) ||
       CpuClockLead() > maximumClockLead)) {
    wait.deferred = yieldedCpu_ = true;
    return false;
  }
  if (inputPort && !wait.sampled && 2 * wait.elapsed >= wait.duration) {
    wait.readData = ReadBus(cycle.address);
    wait.sampled = true;
  }
  if (wait.elapsed < wait.duration)
    return false;

  switch (cycle.kind) {
  case PendingCycleKind::Read:
    *cycle.readResult = wait.sampled ? wait.readData : ReadBus(cycle.address);
    break;
  case PendingCycleKind::Write:
    // I/O writes also reach underlying RAM, once per completed operation.
    if (!wait.wroteRam)
      writeRam(cycle.address, cycle.data);
    if ((cycle.address & 0xfff0) == 0x00f0)
      writeIO(cycle.address, cycle.data);
    break;
  case PendingCycleKind::Idle:
  case PendingCycleKind::None:
    break;
  }
  wait = {};
  return true;
}

// Timer stepping - advance all three timers by the given clock ticks

void Smp::StepTimers(unsigned clocks) {
  timer0_.Step(clocks, io_.timersEnable, io_.timersDisable);
  timer1_.Step(clocks, io_.timersEnable, io_.timersDisable);
  timer2_.Step(clocks, io_.timersEnable, io_.timersDisable);
}

// CPU-side port access

uint8_t Smp::PortRead(uint8_t port) const {
  if (synchronizeCpu_)
    synchronizeCpu_();
  // CPU reads $2140+n → returns what SMP wrote to $F4+n
  return cpuOutput_[port & 3];
}

void Smp::PortWrite(uint8_t port, uint8_t data) {
  if (synchronizeCpu_)
    synchronizeCpu_();
  // CPU writes $2140+n → sets data for SMP to read from $F4+n
  apuInput_[port & 3] = data;
}

// Internal RAM access (with IPL ROM overlay)

uint8_t Smp::readRam(uint16_t address) const {
  // IPL ROM overlay: $FFC0-$FFFF when enabled
  if (address >= 0xFFC0 && io_.iplRomEnable) {
    return iplRom_[address & 0x3F];
  }
  if (io_.ramDisable) {
    return 0x5A; // RAM disabled returns $5A.
  }
  return ram_[address];
}

void Smp::writeRam(uint16_t address, uint8_t data) {
  // Writes always go to RAM (even under IPL ROM overlay)
  if (io_.ramWritable && !io_.ramDisable) {
    ram_[address] = data;
  }
}

// I/O register read ($00F0-$00FF)

uint8_t Smp::readIO(uint16_t address) {
  switch (address) {
  case 0x00F0: // TEST - write-only
    return 0x00;

  case 0x00F1: // CONTROL - write-only
    return 0x00;

  case 0x00F2: // DSPADDR
    return io_.dspAddr;

  case 0x00F3: // DSPDATA - read DSP register at DSPADDR
    return dsp_ ? dsp_->Read(io_.dspAddr & 0x7F) : 0x00;

  case 0x00F4: // CPUIO0 - reads what CPU wrote
    return apuInput_[0];

  case 0x00F5: // CPUIO1
    return apuInput_[1];

  case 0x00F6: // CPUIO2
    return apuInput_[2];

  case 0x00F7: // CPUIO3
    return apuInput_[3];

  case 0x00F8: // AUXIO4
    return io_.aux4;

  case 0x00F9: // AUXIO5
    return io_.aux5;

  case 0x00FA: // T0TARGET - write-only
  case 0x00FB: // T1TARGET - write-only
  case 0x00FC: // T2TARGET - write-only
    return 0x00;

  case 0x00FD: { // T0OUT - read-and-clear (4-bit)
    uint8_t val = timer0_.stage3;
    timer0_.stage3 = 0;
    return val;
  }

  case 0x00FE: { // T1OUT - read-and-clear (4-bit)
    uint8_t val = timer1_.stage3;
    timer1_.stage3 = 0;
    return val;
  }

  case 0x00FF: { // T2OUT - read-and-clear (4-bit)
    uint8_t val = timer2_.stage3;
    timer2_.stage3 = 0;
    return val;
  }

  default:
    return 0x00;
  }
}

// I/O register write ($00F0-$00FF)

void Smp::writeIO(uint16_t address, uint8_t data) {
  switch (address) {
  case 0x00F0: // TEST
    // Writes only valid when P flag is clear.
    if (r.p.p)
      break;
    io_.timersDisable = (data & 0x01) != 0;
    io_.ramWritable = (data & 0x02) != 0;
    io_.ramDisable = (data & 0x04) != 0;
    io_.timersEnable = (data & 0x08) != 0;
    io_.externalWaitStates = (data >> 4) & 0x03;
    io_.internalWaitStates = (data >> 6) & 0x03;
    // Re-evaluate timer gate signals immediately
    timer0_.SynchronizeStage1(io_.timersEnable, io_.timersDisable);
    timer1_.SynchronizeStage1(io_.timersEnable, io_.timersDisable);
    timer2_.SynchronizeStage1(io_.timersEnable, io_.timersDisable);
    break;

  case 0x00F1: // CONTROL
    // Bits 4-5: clear input ports (CPU→SMP latches)
    if (data & 0x10) {
      apuInput_[0] = 0x00;
      apuInput_[1] = 0x00;
    }
    if (data & 0x20) {
      apuInput_[2] = 0x00;
      apuInput_[3] = 0x00;
    }

    // Bit 7: IPL ROM enable/disable
    io_.iplRomEnable = (data & 0x80) != 0;

    // Timers 0 and 1 reset on enable; timer 2 retains its counters on
    // that transition and resets them on every other CONTROL write.
    {
      bool wasEnabled0 = timer0_.enable;
      timer0_.enable = (data & 0x01) != 0;
      if (!wasEnabled0 && timer0_.enable) {
        timer0_.stage2 = 0;
        timer0_.stage3 = 0;
      }

      bool wasEnabled1 = timer1_.enable;
      timer1_.enable = (data & 0x02) != 0;
      if (!wasEnabled1 && timer1_.enable) {
        timer1_.stage2 = 0;
        timer1_.stage3 = 0;
      }

      bool wasEnabled2 = timer2_.enable;
      timer2_.enable = (data & 0x04) != 0;
      if (wasEnabled2 || !timer2_.enable) {
        timer2_.stage2 = 0;
        timer2_.stage3 = 0;
      }
    }
    break;

  case 0x00F2: // DSPADDR
    io_.dspAddr = data;
    break;

  case 0x00F3:                         // DSPDATA - write DSP register at DSPADDR
    if (dsp_ && !(io_.dspAddr & 0x80)) // $80-$FF are read-only mirrors
      dsp_->Write(io_.dspAddr & 0x7F, data);
    break;

  case 0x00F4: // CPUIO0 - SMP writes output for CPU to read
  {
    static int port0WriteCount = 0;
    if (SmpPortTraceEnabled() && data != cpuOutput_[0] && ++port0WriteCount <= 200) {
      fprintf(stderr, "[SMP-PORT0 #%d cyc=%llu] PC=%04X wrote %02X (was %02X)\n", port0WriteCount,
              static_cast<unsigned long long>(cycles_), r.pc, data, cpuOutput_[0]);
    }
    cpuOutput_[0] = data;
    break;
  }

  case 0x00F5: // CPUIO1
    cpuOutput_[1] = data;
    break;

  case 0x00F6: // CPUIO2
    cpuOutput_[2] = data;
    break;

  case 0x00F7: // CPUIO3
    cpuOutput_[3] = data;
    break;

  case 0x00F8: // AUXIO4
    io_.aux4 = data;
    break;

  case 0x00F9: // AUXIO5
    io_.aux5 = data;
    break;

  case 0x00FA: // T0TARGET
    timer0_.target = data;
    break;

  case 0x00FB: // T1TARGET
    timer1_.target = data;
    break;

  case 0x00FC: // T2TARGET
    timer2_.target = data;
    break;

  case 0x00FD: // T0OUT - read-only
  case 0x00FE: // T1OUT - read-only
  case 0x00FF: // T2OUT - read-only
    break;
  }
}

} // namespace snes::core
