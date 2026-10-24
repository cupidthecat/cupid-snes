#include "MovieDialog.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "snes/core/FilePath.hpp"
#include "storage/StateStorage.hpp"
#include <algorithm>
#include <utility>

namespace snes::frontend {
struct MovieDialog::Impl {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> files;
    std::string name, error, failure;
    int selected = -1;
    bool active = false, opening = false, recording = false, replaying = false, pending = false;
    size_t queries = 0;
    std::optional<Action> action, confirmation;
    bool confirmOpening = false;
    bool focusControls = false;

    void Refresh() {
        files.clear();
        if (std::filesystem::exists(directory)) {
            for (const auto& file : std::filesystem::directory_iterator(directory))
                if (file.is_regular_file() && file.path().extension() == ".cmv")
                    files.push_back(file.path());
        }
        std::sort(files.begin(), files.end());
        selected = files.empty() ? -1 : 0;
    }

    void Request(Action next, bool confirm = false) {
        if (action)
            return;
        error.clear();
        if (confirm) {
            confirmation = std::move(next);
            confirmOpening = true;
        } else
            action = std::move(next);
    }
};

MovieDialog::MovieDialog() : impl_(std::make_unique<Impl>()) {
}

MovieDialog::~MovieDialog() = default;

void MovieDialog::Open(std::filesystem::path directory, std::string name) {
    auto& self = *impl_;
    self.directory = std::move(directory);
    self.name = std::move(name);
    self.error.clear();
    self.action.reset();
    self.confirmation.reset();
    self.confirmOpening = false;
    self.Refresh();
    self.active = self.opening = true;
    self.focusControls = true;
}

bool MovieDialog::Active() const noexcept {
    return impl_->active;
}

void MovieDialog::Update(bool recording, bool replaying, bool pending, const char* failure,
                         size_t queries) {
    auto& self = *impl_;
    self.recording = recording;
    self.replaying = replaying;
    self.pending = pending;
    self.failure = failure ? failure : "";
    self.queries = queries;
}

void MovieDialog::Draw() {
    auto& self = *impl_;
    if (!self.active)
        return;
    if (self.opening) {
        ImGui::SetNextWindowFocus();
        self.opening = false;
    }
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(
        {std::min(660.0f, viewport->Size.x - 24), std::min(530.0f, viewport->Size.y - 36)},
        ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::Begin("Input Movies", &self.active,
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const bool busy = self.action.has_value();
    ImGui::BeginDisabled(busy);
    const auto mode = self.recording ? "Recording" : self.replaying ? "Playback" : "Live input";
    ImGui::Text("%s | %llu queries", mode, static_cast<unsigned long long>(self.queries));
    if (self.pending)
        ImGui::TextWrapped("A completed recording is held in memory until saved or discarded.");
    if (!self.failure.empty())
        ImGui::TextWrapped("%s", self.failure.c_str());
    if (self.recording || self.replaying) {
        if (std::exchange(self.focusControls, false))
            ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(self.recording ? "Stop and Save" : "Stop Playback"))
            self.Request({Action::Kind::Stop, {}});
    } else {
        if (!self.pending && std::exchange(self.focusControls, false))
            ImGui::SetKeyboardFocusHere();
        ImGui::BeginDisabled(self.pending);
        if (ImGui::Button("Record Current State"))
            self.Request({Action::Kind::Record, {}});
        ImGui::SameLine();
        if (ImGui::Button("Reset and Record"))
            self.Request({Action::Kind::RecordReset, {}});
        ImGui::EndDisabled();
    }
    if (self.pending) {
        if (std::exchange(self.focusControls, false))
            ImGui::SetKeyboardFocusHere();
        ImGui::InputText("Recording name", &self.name);
        if (ImGui::Button("Save Recording")) {
            try {
                StateStorage::ValidateName(self.name);
                auto path = self.directory / core::FilePath(self.name + ".cmv");
                const bool exists = std::filesystem::exists(path);
                self.Request({Action::Kind::Save, std::move(path)}, exists);
            } catch (const std::exception& failure) {
                self.error = failure.what();
            }
        }
    }
    if (self.pending || self.recording) {
        ImGui::SameLine();
        if (ImGui::Button("Discard Recording"))
            self.Request({Action::Kind::Discard, {}}, true);
    }
    ImGui::SeparatorText("Saved movies");
    const float listHeight = std::max(60.0f, ImGui::GetContentRegionAvail().y - 115);
    if (ImGui::BeginChild("##movies", {0, listHeight}, ImGuiChildFlags_Borders)) {
        if (self.files.empty())
            ImGui::TextDisabled("No saved movies for this game");
        for (size_t index = 0; index < self.files.size(); ++index) {
            ImGui::PushID(int(index));
            const auto text = core::PathText(self.files[index].filename());
            const auto position = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##movie", int(index) == self.selected))
                self.selected = int(index);
            ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text),
                                                text.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(self.recording || self.replaying || self.pending || self.selected < 0);
    if (ImGui::Button("Play Selected"))
        self.Request({Action::Kind::Play, self.files.at(size_t(self.selected))});
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        try {
            self.Refresh();
        } catch (const std::exception& failure) {
            self.error = failure.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close") || (!self.confirmation && ImGui::IsKeyPressed(ImGuiKey_Escape))) {
        self.active = false;
    }
    ImGui::TextWrapped("%s", core::PathText(self.directory).c_str());
    if (!self.error.empty())
        ImGui::TextWrapped("%s", self.error.c_str());
    ImGui::EndDisabled();
    if (self.confirmOpening) {
        ImGui::OpenPopup("Confirm movie change");
        self.confirmOpening = false;
    }
    if (ImGui::BeginPopupModal("Confirm movie change", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool discard = self.confirmation->kind == Action::Kind::Discard;
        ImGui::TextUnformatted(discard ? "Discard this recording?" : "Replace the saved movie?");
        if (ImGui::Button(discard ? "Discard" : "Replace")) {
            self.action = std::exchange(self.confirmation, {});
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            self.confirmation.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

std::optional<MovieDialog::Action> MovieDialog::TakeAction() {
    return std::exchange(impl_->action, {});
}

void MovieDialog::Completed() {
    impl_->error.clear();
    impl_->focusControls = true;
    impl_->Refresh();
}

void MovieDialog::Rejected(std::string error) {
    impl_->error = std::move(error);
    impl_->focusControls = true;
}
} // namespace snes::frontend
