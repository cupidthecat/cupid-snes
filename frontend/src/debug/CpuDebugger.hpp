#pragma once

#include "snes/core/Emulator.hpp"
#include "snes/core/Processor65816.hpp"
#include <optional>
#include <string>
#include <vector>

namespace snes::frontend {
class CpuDebugger final : private core::ITraceSink {
public:
  enum class Mode { Paused, Step, Run };
  struct Breakpoint {
    uint32_t first = 0, last = 0;
    bool execute = true, read = false, write = false, enabled = true;
  };
  struct Hit {
    enum class Kind { Execute, Read, Write } kind;
    uint32_t address;
    uint8_t value = 0;
    uint64_t cycle = 0;
  };
  struct TraceEntry {
    enum class Kind { Boundary, Read, Write } kind;
    uint64_t cycle;
    uint32_t address;
    uint8_t value = 0;
    core::Processor65816::Registers registers{};
  };

  explicit CpuDebugger(core::Emulator &machine);
  ~CpuDebugger() override;
  CpuDebugger(const CpuDebugger &) = delete;
  CpuDebugger &operator=(const CpuDebugger &) = delete;
  core::Processor65816::Registers Registers() const;
  uint64_t Clock() const noexcept;
  bool AtBoundary() const noexcept;
  Mode CurrentMode() const noexcept {
    return mode_;
  }
  void Step();
  void Run(std::optional<uint32_t> until = std::nullopt);
  void Pause() noexcept;
  void Repositioned() noexcept;
  // Bounded work, including display events inside an unfinished instruction.
  // The host calls again while running and handles every returned field.
  std::optional<core::FrameStepResult> Pump(size_t eventBudget = 4096);
  std::optional<Hit> LastHit() const noexcept {
    return hit_;
  }
  const std::vector<Breakpoint> &Breakpoints() const noexcept {
    return breakpoints_;
  }
  void AddBreakpoint(Breakpoint breakpoint);
  void RemoveBreakpoint(size_t index);
  void EnableBreakpoint(size_t index, bool enabled);
  void Capture(bool enabled);
  bool Capturing() const noexcept {
    return capturing_;
  }
  void ClearTrace() noexcept;
  size_t TraceSize() const noexcept {
    return traceSize_;
  }
  uint64_t DroppedTraceEntries() const noexcept {
    return dropped_;
  }
  const TraceEntry &TraceAt(size_t index) const;
  std::string ExportTrace() const;
  static constexpr size_t traceCapacity = 16384;
  static constexpr size_t breakpointLimit = 256;

private:
  void OnTrace(const core::TraceEvent &event) override;
  void Append(TraceEntry entry) noexcept;
  void Observe(bool enabled);
  bool ExecutionHit(uint32_t address);
  void RequirePaused() const;
  core::Emulator &machine_;
  Mode mode_ = Mode::Paused;
  std::vector<Breakpoint> breakpoints_;
  std::vector<TraceEntry> trace_;
  size_t traceStart_ = 0, traceSize_ = 0;
  uint64_t dropped_ = 0;
  std::optional<Hit> hit_, pendingHit_;
  std::optional<uint32_t> until_, skip_;
  bool capturing_ = false, observing_ = false;
};
} // namespace snes::frontend
