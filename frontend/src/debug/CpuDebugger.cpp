#include "CpuDebugger.hpp"
#include "snes/core/SnesCpu.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace snes::frontend {
namespace {
void Address(uint32_t address) {
  if (address > 0xffffff)
    throw std::invalid_argument("CPU addresses must be between 000000 and FFFFFF");
}
} // namespace
CpuDebugger::CpuDebugger(core::Emulator &machine) : machine_(machine) {
  if (!machine.Initialized())
    throw std::invalid_argument("The CPU debugger requires a loaded game");
}
CpuDebugger::~CpuDebugger() {
  Observe(false);
}
core::Processor65816::Registers CpuDebugger::Registers() const {
  return machine_.GetCpu()->regs();
}
uint64_t CpuDebugger::Clock() const noexcept {
  return machine_.CurrentMasterCycles();
}
bool CpuDebugger::AtBoundary() const noexcept {
  return machine_.AtInstructionBoundary();
}
void CpuDebugger::RequirePaused() const {
  if (mode_ != Mode::Paused)
    throw std::logic_error("Pause the debugger before changing breakpoints or trace capture");
}
void CpuDebugger::Observe(bool enabled) {
  if (observing_ == enabled)
    return;
  if (enabled)
    machine_.AddTraceSink(this);
  else
    machine_.RemoveTraceSink(this);
  observing_ = enabled;
}
void CpuDebugger::Step() {
  RequirePaused();
  hit_.reset();
  pendingHit_.reset();
  until_.reset();
  skip_.reset();
  Observe(true);
  mode_ = Mode::Step;
}
void CpuDebugger::Run(std::optional<uint32_t> until) {
  RequirePaused();
  if (until)
    Address(*until);
  const auto r = Registers();
  const auto address = (uint32_t(r.pb) << 16) | r.pc;
  skip_ = hit_ && hit_->kind == Hit::Kind::Execute && hit_->address == address
              ? std::optional(address)
              : std::nullopt;
  hit_.reset();
  pendingHit_.reset();
  until_ = until;
  Observe(true);
  mode_ = Mode::Run;
}
void CpuDebugger::Pause() noexcept {
  mode_ = Mode::Paused;
  until_.reset();
  skip_.reset();
  pendingHit_.reset();
  Observe(false);
}
void CpuDebugger::Repositioned() noexcept {
  Pause();
  hit_.reset();
  ClearTrace();
}
void CpuDebugger::AddBreakpoint(Breakpoint breakpoint) {
  RequirePaused();
  Address(breakpoint.first);
  Address(breakpoint.last);
  if (breakpoint.last < breakpoint.first ||
      (!breakpoint.execute && !breakpoint.read && !breakpoint.write))
    throw std::invalid_argument("Choose an ordered address range and at least one access type");
  if (breakpoints_.size() == breakpointLimit)
    throw std::length_error("The debugger supports at most 256 breakpoints");
  breakpoints_.push_back(breakpoint);
}
void CpuDebugger::RemoveBreakpoint(size_t index) {
  RequirePaused();
  if (index >= breakpoints_.size())
    throw std::out_of_range("Select an existing breakpoint");
  breakpoints_.erase(breakpoints_.begin() + index);
}
void CpuDebugger::EnableBreakpoint(size_t index, bool enabled) {
  RequirePaused();
  breakpoints_.at(index).enabled = enabled;
}
void CpuDebugger::Capture(bool enabled) {
  RequirePaused();
  if (enabled && trace_.empty())
    trace_.resize(traceCapacity);
  capturing_ = enabled;
}
void CpuDebugger::ClearTrace() noexcept {
  traceStart_ = traceSize_ = 0;
  dropped_ = 0;
}
void CpuDebugger::Append(TraceEntry entry) noexcept {
  if (traceSize_ == traceCapacity) {
    trace_[traceStart_] = entry;
    traceStart_ = (traceStart_ + 1) % traceCapacity;
    ++dropped_;
  } else {
    trace_[(traceStart_ + traceSize_) % traceCapacity] = entry;
    ++traceSize_;
  }
}
const CpuDebugger::TraceEntry &CpuDebugger::TraceAt(size_t index) const {
  if (index >= traceSize_)
    throw std::out_of_range("Trace row is outside the captured history");
  return trace_[(traceStart_ + index) % traceCapacity];
}
bool CpuDebugger::ExecutionHit(uint32_t address) {
  if (skip_ == address) {
    skip_.reset();
    return false;
  }
  if (until_ == address ||
      std::any_of(breakpoints_.begin(), breakpoints_.end(), [&](const auto &b) {
        return b.enabled && b.execute && address >= b.first && address <= b.last;
      })) {
    hit_ = Hit{Hit::Kind::Execute, address, 0, Clock()};
    Pause();
    return true;
  }
  return false;
}
std::optional<core::FrameStepResult> CpuDebugger::Pump(size_t eventBudget) {
  try {
    while (mode_ != Mode::Paused && eventBudget--) {
      if (AtBoundary()) {
        const auto r = Registers();
        const auto address = (uint32_t(r.pb) << 16) | r.pc;
        if (mode_ == Mode::Run && machine_.GetCpu()->NextStepIsInstruction() &&
            ExecutionHit(address))
          return std::nullopt;
        if (capturing_)
          Append({TraceEntry::Kind::Boundary, Clock(), address, 0, r});
      }
      const auto result = machine_.StepInstruction();
      if (result.instructionComplete) {
        if (pendingHit_) {
          hit_ = pendingHit_;
          Pause();
        } else if (mode_ == Mode::Step)
          Pause();
      }
      if (result.field)
        return result.field;
    }
  } catch (...) {
    Pause();
    throw;
  }
  return std::nullopt;
}
void CpuDebugger::OnTrace(const core::TraceEvent &event) {
  const auto *bus = std::get_if<core::CpuBusTraceEvent>(&event);
  if (!bus)
    return;
  if (capturing_)
    Append({bus->isWrite ? TraceEntry::Kind::Write : TraceEntry::Kind::Read, bus->cycle,
            bus->address, bus->value});
  if (!pendingHit_)
    for (const auto &b : breakpoints_)
      if (b.enabled && (bus->isWrite ? b.write : b.read) && bus->address >= b.first &&
          bus->address <= b.last) {
        pendingHit_ = Hit{bus->isWrite ? Hit::Kind::Write : Hit::Kind::Read, bus->address,
                          bus->value, bus->cycle};
        break;
      }
}
std::string CpuDebugger::ExportTrace() const {
  std::ostringstream out;
  out << "# Oldest retained entry first; overwritten entries: " << dropped_
      << "\nkind\tmaster_clock\taddress\tvalue\tA\tX\tY\tS\tD\tDB\tP\tE\n";
  out << std::uppercase << std::setfill('0');
  for (size_t index = 0; index < TraceSize(); ++index) {
    const auto &entry = TraceAt(index);
    const bool boundary = entry.kind == TraceEntry::Kind::Boundary;
    out << (boundary                               ? "boundary"
            : entry.kind == TraceEntry::Kind::Read ? "read"
                                                   : "write")
        << '\t' << std::dec << entry.cycle << '\t' << std::hex << std::setw(6) << entry.address
        << '\t';
    if (boundary) {
      const auto &r = entry.registers;
      out << "\t" << std::setw(4) << r.a << '\t' << std::setw(4) << r.x << '\t' << std::setw(4)
          << r.y << '\t' << std::setw(4) << r.s << '\t' << std::setw(4) << r.d << '\t'
          << std::setw(2) << unsigned(r.db) << '\t' << std::setw(2) << unsigned(r.p) << '\t'
          << unsigned(r.e);
    } else
      out << std::setw(2) << unsigned(entry.value) << "\t\t\t\t\t\t\t\t";
    out << '\n';
  }
  return out.str();
}
} // namespace snes::frontend
