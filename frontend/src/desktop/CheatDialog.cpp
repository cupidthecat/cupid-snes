#include "CheatDialog.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "snes/core/FilePath.hpp"

#include <algorithm>
#include <utility>

namespace snes::frontend {
void CheatDialog::Open(const CheatBook& book, core::CheatSystem system,
                       const std::filesystem::path& path, std::string error) {
    draft_ = book;
    system_ = system;
    path_ = core::PathText(path);
    fileError_ = std::move(error);
    error_.clear();
    change_.reset();
    selected_ = draft_.entries.empty() ? -1 : 0;
    reload_ = replaceUnreadable_ = false;
    active_ = opening_ = true;
}

void CheatDialog::Draw() {
    if (!active_)
        return;
    const auto* viewport = ImGui::GetMainViewport();
    if (opening_) {
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize(
            {std::min(720.0f, viewport->Size.x - 16), std::min(600.0f, viewport->Size.y - 56)},
            ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        opening_ = false;
    }
    if (ImGui::Begin("Cheats", &active_,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", path_.c_str());
        if (!fileError_.empty()) {
            ImGui::TextWrapped("File not loaded: %s", fileError_.c_str());
            ImGui::Checkbox("Replace unreadable file when applying", &replaceUnreadable_);
        }
        ImGui::Checkbox("Enable cheats", &draft_.enabled);
        ImGui::TextWrapped("Apply saves changes for this game. Close discards unapplied edits.");
        if (ImGui::BeginTable("Cheat panes", 2, ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Groups", ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableNextColumn();
            if (ImGui::BeginChild("Cheat list", {0, 180}, ImGuiChildFlags_Borders)) {
                for (size_t index = 0; index < draft_.entries.size(); ++index) {
                    auto& entry = draft_.entries[index];
                    ImGui::PushID(int(index));
                    ImGui::Checkbox("##enabled", &entry.enabled);
                    ImGui::SameLine();
                    const auto position = ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable("##name", selected_ == int(index)))
                        selected_ = int(index);
                    ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text),
                                                        entry.name.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::BeginDisabled(draft_.entries.size() >= 1024);
            if (ImGui::Button("Add")) {
                draft_.entries.push_back({"New cheat", "", false});
                selected_ = int(draft_.entries.size() - 1);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(selected_ < 0);
            if (ImGui::Button("Remove")) {
                draft_.entries.erase(draft_.entries.begin() + selected_);
                selected_ = std::min(selected_, int(draft_.entries.size()) - 1);
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            if (selected_ >= 0 && size_t(selected_) < draft_.entries.size()) {
                auto& entry = draft_.entries[size_t(selected_)];
                ImGui::TextUnformatted("Name");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##name", &entry.name);
                ImGui::TextUnformatted("Codes");
                ImGui::InputTextMultiline("##codes", &entry.codes, {-1, 140});
                ImGui::TextWrapped(
                    system_ == core::CheatSystem::Console
                        ? "Game Genie, PAR, AAAAAA=DD or AAAAAA=CC?DD."
                        : "Handheld Game Genie, GameShark 01, AAAA=DD or AAAA=CC?DD.");
                ImGui::TextWrapped(
                    "Separate codes with a new line or '+'. Group order controls write order.");
            } else
                ImGui::TextWrapped("Add a cheat group to enter codes.");
            ImGui::EndTable();
        }
        if (!error_.empty())
            ImGui::TextWrapped("%s", error_.c_str());
        ImGui::BeginDisabled(!fileError_.empty() && !replaceUnreadable_);
        if (ImGui::Button("Apply")) {
            try {
                draft_.Compile(system_);
                change_ = Change{draft_, replaceUnreadable_};
                error_.clear();
            } catch (const std::exception& failure) {
                error_ = failure.what();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reload file (discard edits)"))
            reload_ = true;
        ImGui::SameLine();
        if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            active_ = false;
    }
    ImGui::End();
}

std::optional<CheatDialog::Change> CheatDialog::TakeChanges() {
    return std::exchange(change_, {});
}

bool CheatDialog::TakeReload() {
    return std::exchange(reload_, false);
}

void CheatDialog::Applied(const CheatBook& book) {
    draft_ = book;
    fileError_.clear();
    replaceUnreadable_ = false;
    error_ = "Cheats saved and applied.";
}

void CheatDialog::Rejected(std::string error) {
    error_ = std::move(error);
}

void CheatDialog::Append(CheatEntry entry) {
    if (draft_.entries.size() >= 1024)
        throw std::runtime_error("At most 1024 cheat groups are supported");
    draft_.entries.push_back(std::move(entry));
    selected_ = int(draft_.entries.size() - 1);
}
} // namespace snes::frontend
