#pragma once
#include "debug/CpuDebugger.hpp"
#include <optional>
#include <string>

namespace snes::frontend {
class CpuDebuggerDialog {
  public:
    void Open(CpuDebugger& debugger);

    bool Active() const noexcept {
        return active_;
    }

    void Draw();

    CpuDebugger& Model() const {
        return *debugger_;
    }

    void Rejected(std::string message);
    std::optional<std::string> TakeExport();

  private:
    void Attempt(const std::function<void()>& action);
    void Breakpoints();
    void Trace();
    void Disassembly();
    void TableNavigation(bool& previous);
    CpuDebugger* debugger_ = nullptr;
    bool active_ = false, opening_ = false;
    std::string first_ = "008000", last_, until_ = "008000", message_;
    bool execute_ = true, read_ = false, write_ = false;
    std::optional<size_t> selected_;
    std::optional<std::string> export_;
    std::string disassemblyAddress_ = "008000";
    uint32_t disassemblyStart_ = 0x8000;
    bool followPc_ = true, cpuWidths_ = true;
    CpuDecodeWidth width_;
    std::optional<uint32_t> selectedAddress_;
    bool focusClose_ = false, focusDisassembly_ = false, focusBreakpoint_ = false,
         focusFirst_ = false;
};
} // namespace snes::frontend
