#include "DesktopMenu.hpp"

#include "imgui.h"
#include "snes/core/FilePath.hpp"
#include <algorithm>
#include <deque>
#include <string>
#include <vector>

namespace snes::frontend {
struct DesktopMenu::Impl {
  std::deque<DesktopCommand> commands;
  std::vector<std::string> recent;
  bool loaded = false, paused = false;

  void Item(const char *label, const char *shortcut, DesktopCommand command, bool enabled = true,
            bool selected = false) {
    if (ImGui::MenuItem(label, shortcut, selected, enabled))
      commands.push_back(command);
  }
};

DesktopMenu::DesktopMenu() : impl_(std::make_unique<Impl>()) {}
DesktopMenu::~DesktopMenu() = default;

void DesktopMenu::SetRecent(std::span<const LaunchRequest> requests) {
  auto &names = impl_->recent;
  names.clear();
  for (const auto &request : requests.first(std::min<size_t>(requests.size(), 10))) {
    std::string name;
    const auto asset = [&](unsigned index) {
      if (request.paths[index].empty() || request.paths[index] == "-")
        return;
      if (!name.empty())
        name += " + ";
      name += core::PathText(core::FilePath(request.paths[index]).filename());
    };
    if (request.kind == LaunchRequest::Kind::Handheld)
      asset(1);
    else if (request.kind == LaunchRequest::Kind::SufamiTurbo) {
      asset(1);
      asset(2);
      if (name.empty())
        asset(0);
    } else {
      asset(0);
      if (request.kind == LaunchRequest::Kind::Broadcast)
        asset(1);
      if (request.kind == LaunchRequest::Kind::Campus92 ||
          request.kind == LaunchRequest::Kind::PowerFest94) {
        asset(1);
        asset(2);
        asset(3);
        name += " [DIP " + std::to_string(request.competitionDip) + "]";
      }
    }
    names.push_back(std::to_string(names.size() + 1) + ". " + name);
  }
}

void DesktopMenu::Update(bool loaded, bool paused) {
  impl_->loaded = loaded;
  impl_->paused = paused;
}

void DesktopMenu::Draw(bool enabled) {
  auto &self = *impl_;
  if (!ImGui::BeginMainMenuBar())
    return;
  using C = DesktopCommand;
  if (ImGui::BeginMenu("File", enabled)) {
    self.Item("Open Game...", "Ctrl+O", C::Open);
    self.Item("Open Super Game Boy...", nullptr, C::OpenHandheld);
    self.Item("Open Sufami Turbo...", nullptr, C::OpenSufami);
    self.Item("Open Broadcast Cartridge...", nullptr, C::OpenBroadcast);
    self.Item("Open Campus Challenge 92...", nullptr, C::OpenCampus);
    self.Item("Open PowerFest 94...", nullptr, C::OpenPowerFest);
    if (ImGui::BeginMenu("Recent Games")) {
      if (self.recent.empty())
        ImGui::TextDisabled("No recent games");
      for (unsigned i = 0; i < self.recent.size(); ++i) {
        ImGui::PushID(int(i));
        // Filename characters must not become widget identifiers.
        const auto size = ImGui::CalcTextSize(self.recent[i].c_str(), nullptr, false);
        const auto position = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##recent", false, 0, size))
          self.commands.push_back(C(unsigned(C::Recent0) + i));
        ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text),
                                            self.recent[i].c_str());
        ImGui::PopID();
      }
      if (!self.recent.empty()) {
        ImGui::Separator();
        self.Item("Clear Recent Games", nullptr, C::ClearRecent);
      }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    self.Item("Unload Game", "Ctrl+W", C::Unload, self.loaded);
    self.Item("Preferences...", "Ctrl+,", C::Preferences);
    self.Item("Quit", "Ctrl+Q", C::Quit);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Game", enabled)) {
    self.Item("Pause", "P", C::Pause, self.loaded, self.paused);
    self.Item("Frame Advance", "N", C::FrameAdvance, self.loaded);
    self.Item("Reset", "F5", C::Reset, self.loaded);
    self.Item("Power Cycle", nullptr, C::PowerCycle, self.loaded);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View", enabled)) {
    self.Item("Fullscreen", "F11", C::Fullscreen);
    self.Item("Screenshot", "F12", C::Screenshot, self.loaded);
    ImGui::EndMenu();
  }
  ImGui::EndMainMenuBar();
}

std::optional<DesktopCommand> DesktopMenu::TakeCommand() {
  if (impl_->commands.empty())
    return {};
  const auto command = impl_->commands.front();
  impl_->commands.pop_front();
  return command;
}

std::optional<DesktopCommand> DesktopMenu::KeyCommand(const SDL_KeyboardEvent &key) {
  if (key.repeat || key.type != SDL_EVENT_KEY_DOWN)
    return {};
  if (key.mod & SDL_KMOD_CTRL) {
    if (key.scancode == SDL_SCANCODE_COMMA)
      return DesktopCommand::Preferences;
    if (key.scancode == SDL_SCANCODE_O)
      return DesktopCommand::Open;
    if (key.scancode == SDL_SCANCODE_W)
      return DesktopCommand::Unload;
    if (key.scancode == SDL_SCANCODE_Q)
      return DesktopCommand::Quit;
    return {};
  }
  if ((key.mod & SDL_KMOD_ALT) && key.scancode == SDL_SCANCODE_RETURN)
    return DesktopCommand::Fullscreen;
  if (key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))
    return {};
  switch (key.scancode) {
  case SDL_SCANCODE_ESCAPE:
    return DesktopCommand::Quit;
  case SDL_SCANCODE_P:
    return DesktopCommand::Pause;
  case SDL_SCANCODE_N:
    return DesktopCommand::FrameAdvance;
  case SDL_SCANCODE_F5:
    return DesktopCommand::Reset;
  case SDL_SCANCODE_F11:
    return DesktopCommand::Fullscreen;
  case SDL_SCANCODE_F12:
    return DesktopCommand::Screenshot;
  default:
    return {};
  }
}
} // namespace snes::frontend
