#include "CpuDebuggerDialog.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
namespace {
uint32_t Address(std::string_view text) {
  if (text.starts_with('$'))
    text.remove_prefix(1);
  else if (text.starts_with("0x") || text.starts_with("0X"))
    text.remove_prefix(2);
  uint32_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > 0xffffff)
    throw std::invalid_argument("Enter a hexadecimal CPU address between 000000 and FFFFFF");
  return value;
}
const char *Kind(CpuDebugger::TraceEntry::Kind kind) {
  switch (kind) {
  case CpuDebugger::TraceEntry::Kind::Boundary:
    return "CPU boundary";
  case CpuDebugger::TraceEntry::Kind::Read:
    return "Read";
  case CpuDebugger::TraceEntry::Kind::Write:
    return "Write";
  }
  return "";
}
} // namespace
void CpuDebuggerDialog::Open(CpuDebugger &debugger) {
  debugger.Pause();
  debugger_ = &debugger;
  active_ = opening_ = true;
  selected_.reset();
  export_.reset();
  message_.clear();
  const auto r = debugger.Registers();
  char address[8];
  std::snprintf(address, sizeof(address), "%02X%04X", unsigned(r.pb), unsigned(r.pc));
  first_ = until_ = address;
  last_.clear();
}
void CpuDebuggerDialog::Attempt(const std::function<void()> &action) {
  try {
    action();
    message_.clear();
  } catch (const std::exception &error) {
    Rejected(error.what());
  }
}
void CpuDebuggerDialog::Rejected(std::string message) {
  message_ = std::move(message);
}
std::optional<std::string> CpuDebuggerDialog::TakeExport() {
  return std::exchange(export_, {});
}
void CpuDebuggerDialog::Breakpoints() {
  const bool running = debugger_->CurrentMode() != CpuDebugger::Mode::Paused;
  ImGui::BeginDisabled(running);
  ImGui::SetNextItemWidth(100);
  ImGui::InputText("First (hex)", &first_);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(100);
  ImGui::InputText("Last (blank = first)", &last_);
  ImGui::Checkbox("Execute", &execute_);
  ImGui::SameLine();
  ImGui::Checkbox("Read", &read_);
  ImGui::SameLine();
  ImGui::Checkbox("Write", &write_);
  ImGui::SameLine();
  if (ImGui::Button("Add breakpoint"))
    Attempt([&] {
      const auto first = Address(first_);
      debugger_->AddBreakpoint(
          {first, last_.empty() ? first : Address(last_), execute_, read_, write_});
    });
  const bool selected = selected_ && *selected_ < debugger_->Breakpoints().size();
  ImGui::BeginDisabled(!selected);
  if (ImGui::Button("Enable/disable selected"))
    Attempt([&] {
      debugger_->EnableBreakpoint(*selected_, !debugger_->Breakpoints()[*selected_].enabled);
    });
  ImGui::SameLine();
  if (ImGui::Button("Remove selected"))
    Attempt([&] {
      debugger_->RemoveBreakpoint(*selected_);
      selected_.reset();
    });
  ImGui::EndDisabled();
  if (ImGui::BeginTable("Breakpoints", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                        {0, 0})) {
    ImGui::TableSetupColumn("Range");
    ImGui::TableSetupColumn("Access");
    ImGui::TableSetupColumn("Enabled");
    ImGui::TableHeadersRow();
    size_t index = 0;
    for (const auto &b : debugger_->Breakpoints()) {
      ImGui::PushID(int(index));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      char text[32];
      std::snprintf(text, sizeof(text), "%06X - %06X", unsigned(b.first), unsigned(b.last));
      if (ImGui::Selectable(text, selected_ == index, ImGuiSelectableFlags_SpanAllColumns))
        selected_ = index;
      ImGui::TableNextColumn();
      ImGui::Text("%s%s%s", b.execute ? "X " : "", b.read ? "R " : "", b.write ? "W" : "");
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(b.enabled ? "Yes" : "No");
      ImGui::PopID();
      ++index;
    }
    ImGui::EndTable();
  }
  ImGui::EndDisabled();
}
void CpuDebuggerDialog::Trace() {
  const bool running = debugger_->CurrentMode() != CpuDebugger::Mode::Paused;
  bool capture = debugger_->Capturing();
  ImGui::BeginDisabled(running);
  if (ImGui::Checkbox("Capture trace", &capture))
    Attempt([&] { debugger_->Capture(capture); });
  ImGui::SameLine();
  if (ImGui::Button("Clear trace"))
    debugger_->ClearTrace();
  ImGui::SameLine();
  if (ImGui::Button("Export trace"))
    Attempt([&] { export_ = debugger_->ExportTrace(); });
  ImGui::EndDisabled();
  ImGui::Text("%zu / %zu retained, %llu overwritten", debugger_->TraceSize(),
              CpuDebugger::traceCapacity,
              static_cast<unsigned long long>(debugger_->DroppedTraceEntries()));
  ImGui::TextWrapped("Boundary rows include reset, interrupts, WAI and STP cycles. Bus rows "
                     "include CPU and DMA accesses.");
  if (ImGui::BeginTable("Trace", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                        {0, 0})) {
    ImGui::TableSetupColumn("Master clock", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("000000000000").x);
    ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("CPU boundary").x);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("Address").x);
    ImGui::TableSetupColumn("Value / registers", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(int(debugger_->TraceSize()));
    while (clipper.Step())
      for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
        const auto &entry = debugger_->TraceAt(size_t(index));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(entry.cycle));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(Kind(entry.kind));
        ImGui::TableNextColumn();
        ImGui::Text("%06X", unsigned(entry.address));
        ImGui::TableNextColumn();
        if (entry.kind == CpuDebugger::TraceEntry::Kind::Boundary)
          ImGui::Text("A:%04X X:%04X Y:%04X P:%02X E:%u", unsigned(entry.registers.a),
                      unsigned(entry.registers.x), unsigned(entry.registers.y),
                      unsigned(entry.registers.p), unsigned(entry.registers.e));
        else
          ImGui::Text("%02X", unsigned(entry.value));
      }
    ImGui::EndTable();
  }
}
void CpuDebuggerDialog::Draw() {
  if (!active_)
    return;
  if (opening_) {
    ImGui::OpenPopup("Console CPU debugger");
    opening_ = false;
  }
  const auto *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
  ImGui::SetNextWindowSize(
      {std::min(900.0f, viewport->Size.x - 24), std::min(670.0f, viewport->Size.y - 48)},
      ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("Console CPU debugger", &active_)) {
    auto &debug = *debugger_;
    const auto r = debug.Registers();
    const bool stopped = debug.CurrentMode() == CpuDebugger::Mode::Paused;
    ImGui::Text("PC %02X:%04X  A %04X  X %04X  Y %04X  S %04X  D %04X  DB %02X", unsigned(r.pb),
                unsigned(r.pc), unsigned(r.a), unsigned(r.x), unsigned(r.y), unsigned(r.s),
                unsigned(r.d), unsigned(r.db));
    ImGui::Text("P %02X  NV-MXDIZC: %u%u-%u%u%u%u%u%u  E %u  WAI %u  STP %u  Clock %llu",
                unsigned(r.p), bool(r.p & 128), bool(r.p & 64), bool(r.p & 32), bool(r.p & 16),
                bool(r.p & 8), bool(r.p & 4), bool(r.p & 2), bool(r.p & 1), unsigned(r.e),
                unsigned(r.wai), unsigned(r.stp), static_cast<unsigned long long>(debug.Clock()));
    ImGui::TextUnformatted(debug.AtBoundary() ? "At an instruction boundary"
                                              : "CPU/DMA operation in progress; Step completes it");
    if (const auto hit = debug.LastHit()) {
      if (hit->kind == CpuDebugger::Hit::Kind::Execute)
        ImGui::Text("Stopped before instruction at %06X, clock %llu", unsigned(hit->address),
                    static_cast<unsigned long long>(hit->cycle));
      else
        ImGui::Text("Stopped on %s at %06X, value %02X, clock %llu",
                    hit->kind == CpuDebugger::Hit::Kind::Read ? "read" : "write",
                    unsigned(hit->address), unsigned(hit->value),
                    static_cast<unsigned long long>(hit->cycle));
    }
    const bool shortcut = !ImGui::GetIO().WantTextInput;
    ImGui::BeginDisabled(!stopped);
    if (ImGui::Button("Step (F10)") || (stopped && shortcut && ImGui::IsKeyPressed(ImGuiKey_F10)))
      Attempt([&] { debug.Step(); });
    ImGui::SameLine();
    if (ImGui::Button("Run (F9)") ||
        (stopped && shortcut && ImGui::IsKeyPressed(ImGuiKey_F9, false)))
      Attempt([&] { debug.Run(); });
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(stopped);
    if (ImGui::Button("Pause (F9)") ||
        (!stopped && shortcut && ImGui::IsKeyPressed(ImGuiKey_F9, false)))
      debug.Pause();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!stopped);
    ImGui::SetNextItemWidth(100);
    ImGui::InputText("Run to (hex)", &until_);
    ImGui::SameLine();
    if (ImGui::Button("Run to address"))
      Attempt([&] { debug.Run(Address(until_)); });
    ImGui::EndDisabled();
    ImGui::TextWrapped("Run is silent and unthrottled. Tool input does not reach the game. "
                       "Watchpoints stop after the current CPU operation completes.");
    const float footer = ImGui::GetFrameHeightWithSpacing() + (message_.empty() ? 0 : 56);
    if (ImGui::BeginChild("Debugger content", {0, -footer}, ImGuiChildFlags_NavFlattened)) {
      if (ImGui::BeginTabBar("Debugger views")) {
        if (ImGui::BeginTabItem("Breakpoints")) {
          Breakpoints();
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Trace")) {
          Trace();
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    }
    ImGui::EndChild();
    if (!message_.empty()) {
      if (ImGui::BeginChild("Debugger message", {0, 48}, ImGuiChildFlags_Borders,
                            ImGuiWindowFlags_NoNav))
        ImGui::TextWrapped("%s", message_.c_str());
      ImGui::EndChild();
    }
    if (ImGui::Button("Close", {100, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      active_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (!active_)
    debugger_->Pause();
}
} // namespace snes::frontend
