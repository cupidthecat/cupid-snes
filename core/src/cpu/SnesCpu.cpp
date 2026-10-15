// snes emulator
// core/src/cpu/SnesCpu.cpp
// The SNES CPU wrapper and interrupt integration.

// SnesCpu.cpp — SNES-specific CPU wrapper implementation
//
// Implements the Processor65816 virtual bus interface with SNES timing,
// NMI/IRQ state machine, and cycle counting.

#include "snes/core/SnesCpu.hpp"

namespace snes::core {

// Construction

SnesCpu::SnesCpu(ICpuBus& bus)
    : bus_(bus)
{
}

// Reset

void SnesCpu::Reset() {
  if (discardStep_)
    discardStep_();
  power();

  cycles_ = 0;
  instructionClocks_ = 0;

  nmiLine_ = false;
  nmiEdge_ = false;
  nmiPending_ = false;
  irqLine_ = false;
  irqPending_ = false;

  resetPending_ = true;
  dramRefreshState_ = 0;
  bus_.SetOpenBus(0);
}

// Step — execute one instruction

uint32_t SnesCpu::Step() {
  if (stepSuspended_ && resumeStep_)
    return resumeStep_();
  instructionClocks_ = 0;

  if (resetPending_) {
    resetPending_ = false;
    // Hold the processor while the rest of the console continues clocking.
    for (unsigned clocks = 0; clocks < 132; clocks += 6)
      addClocks(6);
    enterInterrupt(Interrupt::Reset);
    return instructionClocks_;
  }

    // Only reset releases STP. Keep the scheduler running without bus access.
    if (regs().stp) {
        idle();
        return instructionClocks_;
    }

    // Check for pending NMI/IRQ before the instruction
    if (nmiPending_) {
        nmiPending_ = false;
        nmiEdge_ = false;
        enterInterrupt(Interrupt::Nmi);
        return instructionClocks_;
    }

    // The I flag was sampled before the instruction's final cycle. CLI, SEI
    // and PLP can change it after that sample; do not test the new value here.
    if (irqPending_) {
        irqPending_ = false;
        enterInterrupt(Interrupt::Irq);
        return instructionClocks_;
    }

    // WAI: if the CPU is in WAI state, consume an idle cycle and return
    if (regs().wai) {
        idle();
        lastCycle();
        // A masked IRQ releases WAI without entering the interrupt handler.
        if (nmiPending_ || irqLine_) {
            regs().wai = false;
        }
        return instructionClocks_;
    }

    // Execute one instruction
    instruction();

    return instructionClocks_;
}

// External interrupt requests

void SnesCpu::RequestNmi() {
    // NMI is edge-sensitive.  The IrqController already performs the
    // false→true edge detection and only calls us once per VBlank,
    // so we can set the edge flag unconditionally here.
    nmiEdge_ = true;
}

void SnesCpu::SetIrqLevel(bool active) {
    irqLine_ = active;
}

// Bus interface overrides

void SnesCpu::idle() {
    if (beforeCycle_) beforeCycle_(6);
    addClocks(6);
    if (onBusPhase_)
      onBusPhase_();
    if (onAluStep_) onAluStep_(false);
}

uint8_t SnesCpu::read(uint32_t address) {
    const uint8_t speed = bus_.Speed(address);
    if (beforeCycle_)
      beforeCycle_(speed);
    regs().mar = address & 0x00ffffff;
    addClocks(speed - 4);
    if (onCoprocessorSync_)
      onCoprocessorSync_();
    if (onBusPhase_)
      onBusPhase_();
    uint8_t data = bus_.ReadCpu(address);
    addClocks(4);
    if (onAluStep_)
      onAluStep_(false);
    // Internal register reads leave the external memory data latch intact.
    if ((address & 0x40fc00) != 0x4000) {
      regs().mdr = data;
      bus_.SetOpenBus(data);
    }
    return data;
}

void SnesCpu::write(uint32_t address, uint8_t data) {
    if (onAluStep_) onAluStep_(true);
    const uint8_t speed = bus_.Speed(address);
    if (beforeCycle_) beforeCycle_(speed);
    regs().mar = address & 0x00ffffff;
    addClocks(speed);
    if (onCoprocessorSync_)
      onCoprocessorSync_();
    if (onBusPhase_)
      onBusPhase_();
    bus_.Write(address, regs().mdr = data);
}

void SnesCpu::lastCycle() {
    // Instruction handlers call this before their final bus or idle cycle.
    if (onInterruptPoll_ && !onInterruptPoll_()) return;
    pollInterrupts();
}

bool SnesCpu::interruptPending() const {
    return nmiEdge_ || nmiPending_ || irqPending_ || (irqLine_ && !flagI());
}

// Internal helpers

void SnesCpu::addClocks(uint32_t clocks) {
    cycles_ += clocks;
    instructionClocks_ += clocks;
    if (onClock_) onClock_(clocks);
}

void SnesCpu::pollInterrupts() {
    // NMI edge detection
    if (nmiEdge_) {
        nmiPending_ = true;
        nmiEdge_ = false;
    }

    irqPending_ = irqLine_ && !flagI();
}

// DRAM Refresh — 40-master-clock penalty per scanline
//
// Refresh consists of five (6 halt + 2 interleave) substeps, totaling 40 clocks.
// Between each sub-step, the ALU pipeline (multiply/divide) is advanced.
//
// The refresh state identifies the current bus phase:
//   0 = not refreshing
//   1 = bus halted (6 clocks)  — coprocessors see the bus as unavailable
//   2 = interleave  (2 clocks) — brief gap between halt phases

void SnesCpu::ApplyDramRefreshPenalty() {
    for (int i = 0; i < 5; i++) {
        dramRefreshState_ = 1;
        cycles_ += 6;
        dramRefreshState_ = 2;
        cycles_ += 2;
        if (onAluStep_) onAluStep_(false);
    }
    dramRefreshState_ = 0;
}

} // namespace snes::core
