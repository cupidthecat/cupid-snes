#include "MemoryDialog.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
namespace {
std::string Address(uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", unsigned(value));
  return text;
}
uint32_t ParseAddress(std::string_view text) {
  if (text.starts_with("0x") || text.starts_with("0X"))
    text.remove_prefix(2);
  else if (text.starts_with('$'))
    text.remove_prefix(1);
  uint32_t address = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), address, 16);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    throw std::invalid_argument("Enter a hexadecimal address in the selected region");
  return address;
}
int64_t Adjustment(std::string_view text) {
  int64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value < -int64_t(UINT32_MAX) || value > int64_t(UINT32_MAX))
    throw std::invalid_argument(
        "Enter a decimal comparison adjustment between -4294967295 and 4294967295");
  return value;
}
} // namespace
void MemoryDialog::Open(MemoryTools &tools, bool search) {
  tools_ = &tools;
  config_ = tools.Search().Active() ? tools.Search().Configuration() : RamSearch::Config{};
  offset_ = 0;
  selected_.reset();
  cheat_.reset();
  dump_.reset();
  message_.clear();
  searchTab_ = search;
  focusFilter_ = false;
  Refresh();
  selectTab_ = active_ = opening_ = true;
}
void MemoryDialog::Refresh() {
  const auto &region = tools_->Selected();
  if (offset_ >= region.size)
    offset_ = 0;
  page_ = tools_->Read(offset_, std::min<size_t>(256, region.size - offset_));
  address_ = Address(region.base + uint32_t(offset_));
}
void MemoryDialog::Memory() {
  const auto &region = tools_->Selected();
  ImGui::SetNextItemWidth(130);
  ImGui::InputText("Address (hex)", &address_);
  ImGui::SameLine();
  if (ImGui::Button("Go"))
    Attempt([&] {
      const auto address = ParseAddress(address_);
      if (address < region.base || uint64_t(address) - region.base >= region.size)
        throw std::out_of_range("Address is outside the selected memory region");
      offset_ = size_t(address - region.base) & ~size_t(15);
      Refresh();
    });
  ImGui::SameLine();
  if (ImGui::Button("Refresh"))
    Attempt([&] { Refresh(); });
  ImGui::SameLine();
  ImGui::BeginDisabled(!offset_);
  if (ImGui::Button("Previous"))
    Attempt([&] {
      offset_ = offset_ > 256 ? offset_ - 256 : 0;
      Refresh();
    });
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(offset_ + page_.size() >= region.size);
  if (ImGui::Button("Next"))
    Attempt([&] {
      offset_ += 256;
      Refresh();
    });
  ImGui::EndDisabled();
  if (ImGui::BeginTable("Memory bytes", 18,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY,
                        {0, std::max(120.0f, ImGui::GetContentRegionAvail().y - 160)})) {
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 82);
    for (unsigned byte = 0; byte < 16; ++byte) {
      const auto label = Address(byte).substr(7);
      ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed, 22);
    }
    ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (size_t row = 0; row < page_.size(); row += 16) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(Address(region.base + uint32_t(offset_ + row)).c_str());
      std::string text;
      for (size_t byte = 0; byte < 16; ++byte) {
        ImGui::TableNextColumn();
        if (row + byte < page_.size()) {
          const auto value = page_[row + byte];
          ImGui::Text("%02X", unsigned(value));
          text += value >= 32 && value < 127 ? char(value) : '.';
        }
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(text.c_str());
    }
    ImGui::EndTable();
  }
  if (ImGui::Button("Export region"))
    Attempt([&] { dump_ = Dump{region.key, tools_->Read(0, region.size)}; });
  ImGui::SameLine();
  ImGui::TextUnformatted("Reads preserve machine clocks and device state.");
}
void MemoryDialog::Search() {
  auto &search = tools_->Search();
  int width = int(config_.bytes - 1);
  ImGui::SetNextItemWidth(100);
  if (ImGui::Combo("Width", &width,
                   "8 bit\0"
                   "16 bit\0"
                   "24 bit\0"
                   "32 bit\0"))
    config_.bytes = unsigned(width + 1);
  ImGui::SameLine();
  ImGui::Checkbox("Signed", &config_.signedValues);
  ImGui::SameLine();
  ImGui::Checkbox("Big endian", &config_.bigEndian);
  ImGui::SameLine();
  ImGui::Checkbox("Aligned", &config_.aligned);
  if (ImGui::Button("New search (unknown)"))
    Attempt([&] {
      tools_->Start(config_);
      selected_.reset();
      message_.clear();
    });
  ImGui::SameLine();
  ImGui::BeginDisabled(!search.CanUndo());
  if (ImGui::Button("Undo filter"))
    Attempt([&] {
      search.Undo();
      selected_.reset();
      message_.clear();
    });
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Clear search")) {
    search.Clear();
    selected_.reset();
    message_.clear();
  }
  ImGui::SetNextItemWidth(120);
  ImGui::Combo("Comparison", &relation_,
               "Equal\0Not equal\0Less\0Less or equal\0Greater\0Greater or equal\0");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140);
  ImGui::Combo("Compare to", &operand_, "Value\0Previous sample\0Initial sample\0");
  if (!operand_) {
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("Value (decimal, 0x or $ hex)", &value_);
  } else {
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("Add to compared value (decimal)", &adjustment_);
  }
  const bool compatible = !search.Active() || search.Configuration() == config_;
  ImGui::BeginDisabled(!compatible);
  if (focusFilter_) {
    ImGui::SetKeyboardFocusHere();
    focusFilter_ = false;
  }
  if (ImGui::Button("Search"))
    Attempt([&] {
      RamSearch::Filter filter{RamSearch::Relation(relation_), RamSearch::Operand(operand_)};
      if (!operand_)
        filter.value = RamSearch::ParseValue(value_, config_);
      else
        filter.adjustment = Adjustment(adjustment_);
      if (!search.Active())
        tools_->Start(config_);
      tools_->Filter(filter);
      selected_.reset();
      message_.clear();
    });
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (!compatible)
    ImGui::TextUnformatted("Start a new search after changing its type.");
  else
    ImGui::Text("%llu %s", static_cast<unsigned long long>(search.Count()),
                search.Count() == 1 ? "candidate" : "candidates");
  const auto &region = tools_->Selected();
  const float height = std::max(100.0f, ImGui::GetContentRegionAvail().y - 230);
  bool leaveResults = false;
  if (ImGui::BeginTable("Search results", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                        {0, height})) {
    leaveResults = ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Tab, false);
    ImGui::TableSetupColumn("Address");
    ImGui::TableSetupColumn("Current");
    ImGui::TableSetupColumn("Previous");
    ImGui::TableSetupColumn("Initial");
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(int(std::min<size_t>(search.Count(), INT_MAX)));
    while (clipper.Step()) {
      const auto rows = search.Results(size_t(clipper.DisplayStart),
                                       size_t(clipper.DisplayEnd - clipper.DisplayStart));
      for (const auto &result : rows) {
        ImGui::PushID(int(result.offset));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(Address(region.base + result.offset).c_str(),
                              selected_ && selected_->offset == result.offset,
                              ImGuiSelectableFlags_SpanAllColumns))
          selected_ = result;
        ImGui::TableNextColumn();
        ImGui::Text("%lld", static_cast<long long>(result.current));
        ImGui::TableNextColumn();
        ImGui::Text("%lld", static_cast<long long>(result.previous));
        ImGui::TableNextColumn();
        ImGui::Text("%lld", static_cast<long long>(result.initial));
        ImGui::PopID();
      }
    }
    ImGui::EndTable();
  }
  if (leaveResults) {
    if (ImGui::GetIO().KeyShift)
      focusFilter_ = true;
    else
      ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetNextItemWidth(180);
  ImGui::InputText("Cheat name", &name_);
  ImGui::BeginDisabled(!selected_);
  if (ImGui::Button("Copy address"))
    ImGui::SetClipboardText(Address(region.base + selected_->offset).c_str());
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!selected_ || !tools_->CanCreateCheat() || !compatible);
  if (ImGui::Button("Create cheat..."))
    Attempt([&] {
      cheat_ = tools_->MakeCheat(*selected_, name_);
      active_ = false;
    });
  ImGui::EndDisabled();
  if (!tools_->CanCreateCheat())
    ImGui::TextWrapped(
        "This region uses physical offsets. Enter a mapped code in the cheat editor to change it.");
  ImGui::TextWrapped(
      "Values come from the last scan. Close to play, then reopen to narrow the same search.");
}
void MemoryDialog::Draw() {
  if (!active_)
    return;
  const auto *viewport = ImGui::GetMainViewport();
  if (opening_) {
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(
        {std::min(850.0f, viewport->Size.x - 16), std::min(660.0f, viewport->Size.y - 56)},
        ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    opening_ = false;
  }
  if (ImGui::Begin("Memory and RAM Search", &active_,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    {
      ImGui::SetNextItemWidth(300);
      if (ImGui::BeginCombo("Region", tools_->Selected().label.c_str())) {
        for (const auto &region : tools_->Regions())
          if (ImGui::Selectable(region.label.c_str(), region.key == tools_->Selected().key)) {
            Attempt([&] {
              tools_->Select(region.key);
              offset_ = 0;
              selected_.reset();
              message_.clear();
              Refresh();
            });
          }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      ImGui::Text("%llu bytes%s", static_cast<unsigned long long>(tools_->Selected().size),
                  tools_->Selected().readOnly ? " (ROM)" : "");
      if (ImGui::BeginTabBar("Memory tools")) {
        if (ImGui::BeginTabItem("Memory", nullptr,
                                selectTab_ && !searchTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
          Memory();
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("RAM Search", nullptr,
                                selectTab_ && searchTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
          Search();
          ImGui::EndTabItem();
        }
        selectTab_ = false;
        ImGui::EndTabBar();
      }
    }
    if (!message_.empty()) {
      const auto height = std::clamp(
          ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing(), 24.0f, 48.0f);
      if (ImGui::BeginChild("Memory message", {0, height}, ImGuiChildFlags_Borders,
                            ImGuiWindowFlags_NoNav))
        ImGui::TextWrapped("%s", message_.c_str());
      ImGui::EndChild();
    }
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape))
      active_ = false;
  }
  ImGui::End();
}
std::optional<CheatEntry> MemoryDialog::TakeCheat() {
  return std::exchange(cheat_, {});
}
std::optional<MemoryDialog::Dump> MemoryDialog::TakeDump() {
  return std::exchange(dump_, {});
}
void MemoryDialog::Rejected(std::string message) {
  message_ = std::move(message);
}
void MemoryDialog::Attempt(const std::function<void()> &operation) {
  try {
    message_.clear();
    operation();
  } catch (const std::exception &failure) {
    message_ = failure.what();
  }
}
} // namespace snes::frontend
