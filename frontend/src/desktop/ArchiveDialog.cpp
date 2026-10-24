#include "ArchiveDialog.hpp"
#include "imgui.h"
#include "snes/core/FilePath.hpp"
#include <algorithm>
#include <utility>

namespace snes::frontend {
struct ArchiveDialog::Impl {
    std::string archive;
    std::vector<RomArchive::Entry> entries;
    ImGuiTextFilter filter;
    int selected = 0;
    bool active = false, opening = false;
    std::optional<Result> result;
};

ArchiveDialog::ArchiveDialog() : impl_(std::make_unique<Impl>()) {
}

ArchiveDialog::~ArchiveDialog() = default;

void ArchiveDialog::Open(const ArchivePlan& plan) {
    auto& self = *impl_;
    self.archive = plan.request.paths.at(plan.asset);
    self.entries = plan.choices;
    self.selected = 0;
    self.filter.Clear();
    self.result.reset();
    self.active = self.opening = true;
}

bool ArchiveDialog::Active() const noexcept {
    return impl_->active;
}

void ArchiveDialog::Draw() {
    auto& self = *impl_;
    if (!self.active)
        return;
    const bool focusSearch = self.opening;
    const auto* viewport = ImGui::GetMainViewport();
    if (self.opening) {
        ImGui::SetNextWindowSize(
            {std::min(680.0f, viewport->Size.x - 24), std::min(480.0f, viewport->Size.y - 48)},
            ImGuiCond_Always);
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowFocus();
        self.opening = false;
    }
    if (ImGui::Begin("Choose ROM in Archive", &self.active,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", self.archive.c_str());
        if (focusSearch)
            ImGui::SetKeyboardFocusHere();
        self.filter.Draw("Find ROM");
        const float height = std::max(60.0f, ImGui::GetContentRegionAvail().y - 80);
        if (ImGui::BeginChild("Archive ROMs", {0, height},
                              ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened)) {
            for (size_t index = 0; index < self.entries.size(); ++index) {
                const auto& entry = self.entries[index];
                if (!self.filter.PassFilter(entry.name.c_str()))
                    continue;
                ImGui::PushID(int(index));
                const auto position = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##rom", int(index) == self.selected,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    self.selected = int(index);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        self.result = Result{entry.name};
                        self.active = false;
                    }
                }
                ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text),
                                                    entry.name.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%.2f MiB", entry.name.c_str(),
                                      double(entry.bytes) / (1024 * 1024));
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        const bool valid = self.selected >= 0 && size_t(self.selected) < self.entries.size() &&
                           self.filter.PassFilter(self.entries[size_t(self.selected)].name.c_str());
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Open Selected", {130, 0})) {
            self.result = Result{self.entries[size_t(self.selected)].name};
            self.active = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            self.active = false;
        ImGui::TextWrapped("Choose a ROM to load. The archive stays unchanged.");
    }
    ImGui::End();
    if (!self.active && !self.result)
        self.result = Result{};
}

std::optional<ArchiveDialog::Result> ArchiveDialog::TakeResult() {
    return std::exchange(impl_->result, {});
}
} // namespace snes::frontend
