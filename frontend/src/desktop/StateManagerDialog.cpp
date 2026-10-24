#include "StateManagerDialog.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "snes/core/FilePath.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
namespace {
std::string Date(std::filesystem::file_time_type time) {
#ifdef _MSC_VER
  const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
#else
  const auto system = std::chrono::file_clock::to_sys(time);
#endif
  const auto stamp = std::chrono::system_clock::to_time_t(system);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &stamp);
#else
  localtime_r(&stamp, &local);
#endif
  char text[32]{};
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
  return text;
}
} // namespace

struct StateManagerDialog::Impl {
  SDL_Renderer *renderer;
  SDL_Texture *preview = nullptr;
  std::optional<StateStorage> storage;
  std::vector<StateStorage::Entry> entries;
  std::optional<Action> action, confirmation;
  std::optional<StateStorage::Key> desiredSelection;
  std::optional<StateStorage::Key> previewRequest;
  std::optional<core::VideoFrame> pendingPreview;
  std::string name, error;
  int selected = -1;
  bool active = false, opening = false, quick = false, undo = false, redo = false;
  bool refreshPending = false;
  bool previewCurrent = false, previewLoading = false, retirePreview = false;

  ~Impl() {
    SDL_DestroyTexture(preview);
  }
  void Select(int index) {
    selected = index;
    previewCurrent = previewLoading = false;
    retirePreview = true;
    previewRequest.reset();
    pendingPreview.reset();
    error.clear();
    if (index < 0 || size_t(index) >= entries.size())
      return;
    const auto &entry = entries[size_t(index)];
    name = entry.key.name;
    if (!entry.bytes)
      return;
    previewRequest = entry.key;
    previewLoading = true;
  }
  bool Selected(const StateStorage::Key &key) const {
    return active && selected >= 0 && size_t(selected) < entries.size() &&
           entries[size_t(selected)].key == key;
  }
  void ApplyPreview() {
    if (retirePreview) {
      SDL_DestroyTexture(std::exchange(preview, nullptr));
      retirePreview = false;
    }
    auto picture = std::exchange(pendingPreview, {});
    if (!picture || picture->pixels.empty())
      return;
    try {
      preview = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                  int(picture->width), int(picture->height));
      if (!preview || !SDL_UpdateTexture(preview, nullptr, picture->pixels.data(),
                                         int(picture->width * sizeof(uint32_t))))
        throw std::runtime_error(SDL_GetError());
      SDL_SetTextureScaleMode(preview, SDL_SCALEMODE_LINEAR);
      previewCurrent = true;
    } catch (const std::exception &failure) {
      error = failure.what();
      SDL_DestroyTexture(std::exchange(preview, nullptr));
    }
  }
  void Refresh() {
    auto previous = std::exchange(desiredSelection, {});
    if (!previous && selected >= 0 && size_t(selected) < entries.size())
      previous = entries[size_t(selected)].key;
    entries = storage->List(quick);
    if (quick) {
      auto saved = std::move(entries);
      entries.clear();
      for (unsigned slot = 0; slot < StateStorage::quickSlots; ++slot) {
        const auto key = StateStorage::Quick(slot);
        const auto found = std::find_if(saved.begin(), saved.end(),
                                        [&](const auto &entry) { return entry.key == key; });
        entries.push_back(found == saved.end() ? StateStorage::Entry{key, 0, {}} : *found);
      }
    }
    int index = entries.empty() ? -1 : 0;
    if (previous)
      for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].key == *previous)
          index = int(i);
    Select(index);
  }
  void Request(Action request, bool confirm) {
    error.clear();
    if (confirm) {
      confirmation = std::move(request);
      ImGui::OpenPopup("Confirm state change");
    } else
      action = std::move(request);
  }
};

StateManagerDialog::StateManagerDialog(SDL_Renderer *renderer) : impl_(std::make_unique<Impl>()) {
  impl_->renderer = renderer;
}
StateManagerDialog::~StateManagerDialog() = default;
void StateManagerDialog::Open(const StateStorage &storage, bool undo, bool redo) {
  auto &self = *impl_;
  self.storage = storage;
  self.quick = false;
  self.selected = -1;
  self.action.reset();
  self.desiredSelection.reset();
  self.Refresh();
  self.name = "New state";
  self.undo = undo;
  self.redo = redo;
  self.confirmation.reset();
  self.refreshPending = false;
  self.active = self.opening = true;
}
bool StateManagerDialog::Active() const noexcept {
  return impl_->active;
}
void StateManagerDialog::Draw() {
  auto &self = *impl_;
  if (!self.active)
    return;
  self.ApplyPreview();
  const auto *viewport = ImGui::GetMainViewport();
  if (self.opening) {
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(
        {std::min(700.0f, viewport->Size.x - 16), std::min(570.0f, viewport->Size.y - 56)},
        ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    self.opening = false;
  }
  if (ImGui::Begin("State Manager", &self.active,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    try {
      int category = self.quick ? 1 : 0;
      ImGui::SetNextItemWidth(170);
      if (ImGui::Combo("Category", &category, "Managed states\0Quick states\0")) {
        self.quick = category == 1;
        self.selected = -1;
        self.Refresh();
      }
      if (ImGui::BeginTable("State panes", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("States", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 220);
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("State list", {0, 240}, ImGuiChildFlags_Borders)) {
          if (self.entries.empty())
            ImGui::TextDisabled("No saved states");
          for (size_t i = 0; i < self.entries.size(); ++i) {
            const auto &entry = self.entries[i];
            ImGui::PushID(int(i));
            const auto pos = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##state", self.selected == int(i),
                                  ImGuiSelectableFlags_AllowDoubleClick,
                                  {0, ImGui::GetTextLineHeightWithSpacing() * 2})) {
              const bool activate = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
              self.Select(int(i));
              if (activate && entry.bytes)
                self.Request({Action::Kind::Load, entry.key, {}}, false);
            }
            const auto label = self.quick ? "Slot " + entry.key.name : entry.key.name;
            auto *draw = ImGui::GetWindowDrawList();
            draw->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
            const auto date = entry.bytes ? Date(entry.modified) : "Empty";
            draw->AddText({pos.x, pos.y + ImGui::GetTextLineHeightWithSpacing()},
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), date.c_str());
            ImGui::PopID();
          }
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Preview");
        const float width = std::min(220.0f, ImGui::GetContentRegionAvail().x);
        if (self.previewCurrent && self.preview)
          ImGui::Image(ImTextureID(reinterpret_cast<uintptr_t>(self.preview)),
                       {width, width * 240 / 256});
        else if (self.previewLoading)
          ImGui::TextUnformatted("Reading preview...");
        else
          ImGui::TextWrapped("No picture is available for this state.");
        ImGui::EndTable();
      }
      const bool selected = self.selected >= 0 && size_t(self.selected) < self.entries.size();
      const StateStorage::Entry *entry = selected ? &self.entries[size_t(self.selected)] : nullptr;
      ImGui::BeginDisabled(!entry || !entry->bytes);
      if (ImGui::Button("Load"))
        self.Request({Action::Kind::Load, entry->key, {}}, false);
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(!entry);
      if (ImGui::Button("Save"))
        self.Request({Action::Kind::Save, entry->key, {}}, entry->bytes != 0);
      ImGui::SameLine();
      ImGui::BeginDisabled(!entry || !entry->bytes);
      if (ImGui::Button("Remove"))
        self.Request({Action::Kind::Remove, entry->key, {}}, true);
      ImGui::EndDisabled();
      ImGui::EndDisabled();
      if (!self.quick) {
        ImGui::SetNextItemWidth(-60);
        ImGui::InputText("Name", &self.name);
        if (ImGui::Button("Add State")) {
          const StateStorage::Key key{false, self.name};
          const auto path = self.storage->Path(key);
          self.Request({Action::Kind::Save, key, {}}, std::filesystem::exists(path));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!entry || !entry->bytes);
        if (ImGui::Button("Rename")) {
          StateStorage::ValidateName(self.name);
          self.Request({Action::Kind::Rename, entry->key, self.name}, false);
        }
        ImGui::EndDisabled();
      }
      ImGui::BeginDisabled(!self.undo);
      if (ImGui::Button("Undo Load"))
        self.Request({Action::Kind::Undo, {}, {}}, false);
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(!self.redo);
      if (ImGui::Button("Redo Load"))
        self.Request({Action::Kind::Redo, {}, {}}, false);
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Close") ||
          (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen("Confirm state change")))
        self.active = false;
      if (!self.error.empty())
        ImGui::TextWrapped("%s", self.error.c_str());
      ImGui::TextDisabled("State folder");
      ImGui::TextWrapped("%s", core::PathText(self.storage->Directory()).c_str());
    } catch (const std::exception &failure) {
      self.error = failure.what();
    }
    if (ImGui::BeginPopupModal("Confirm state change", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      const bool remove = self.confirmation && self.confirmation->kind == Action::Kind::Remove;
      ImGui::TextUnformatted(remove ? "Remove the selected state file?"
                                    : "Replace the existing state?");
      if (ImGui::Button(remove ? "Remove" : "Replace") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        self.action = std::move(self.confirmation);
        self.confirmation.reset();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        self.confirmation.reset();
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }
  ImGui::End();
}
std::optional<StateManagerDialog::Action> StateManagerDialog::TakeAction() {
  if (impl_->action && impl_->action->kind == Action::Kind::Save)
    impl_->desiredSelection = impl_->action->key;
  if (impl_->action && impl_->action->kind == Action::Kind::Rename)
    impl_->desiredSelection = StateStorage::Key{false, impl_->action->name};
  return std::exchange(impl_->action, {});
}
std::optional<StateStorage::Key> StateManagerDialog::TakePreview() {
  auto &self = *impl_;
  if (!self.active) {
    self.previewRequest.reset();
    return {};
  }
  if (self.refreshPending) {
    self.refreshPending = false;
    try {
      self.Refresh();
    } catch (const std::exception &failure) {
      self.error = failure.what();
      self.previewRequest.reset();
      self.previewLoading = false;
    }
  }
  return std::exchange(self.previewRequest, {});
}
void StateManagerDialog::PreviewReady(const StateStorage::Key &key, core::VideoFrame picture) {
  auto &self = *impl_;
  if (!self.Selected(key))
    return;
  self.pendingPreview = std::move(picture);
  self.retirePreview = true;
  self.previewLoading = false;
}
void StateManagerDialog::PreviewRejected(const StateStorage::Key &key, std::string error) {
  auto &self = *impl_;
  if (!self.Selected(key))
    return;
  self.previewLoading = false;
  if (self.error.empty())
    self.error = std::move(error);
}
void StateManagerDialog::Completed(bool undo, bool redo) {
  impl_->undo = undo;
  impl_->redo = redo;
  // The current draw data still borrows its preview texture until the next frame.
  impl_->refreshPending = true;
}
void StateManagerDialog::Rejected(std::string error) {
  impl_->desiredSelection.reset();
  impl_->error = std::move(error);
}
} // namespace snes::frontend
