#include "PreferencesDialog.hpp"

#include "SdlAudioOutput.hpp"
#include "SdlInputProvider.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "snes/core/FilePath.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace snes::frontend {
struct PreferencesDialog::Impl {
  Preferences draft;
  std::string file, error;
  std::vector<std::string> audioDevices;
  std::optional<Preferences> changes;
  bool active = false, opening = false, closeOnApply = false;
  int capturing = -1;
  int inputPlayer = 0, inputSlot = 0, capturedPlayer = 0, capturedInputSlot = 0;
  std::vector<SdlInputProvider::Gamepad> gamepads;
  int hotkeyCommand = -1, hotkeySlot = 0, capturedSlot = 0;
  KeyChord pendingModifier;
  ImGuiTextFilter hotkeyFilter;

  bool Capturing() const {
    return capturing >= 0 || hotkeyCommand >= 0;
  }
  void AssignHotkey(KeyChord chord) {
    draft.hotkeys.bindings[hotkeyCommand][capturedSlot] = chord;
    hotkeyCommand = -1;
    pendingModifier = {};
  }

  void General() {
    ImGui::SeparatorText("Video");
    ImGui::SliderInt("Window scale", &draft.scale, 1, 6, "%dx");
    ImGui::Checkbox("Aspect correction (8:7)", &draft.aspectCorrection);
    ImGui::Checkbox("Prefer integer scaling", &draft.integerScaling);
    ImGui::Checkbox("Linear filtering", &draft.linearFilter);
    ImGui::SeparatorText("Audio");
    ImGui::SliderInt("Volume", &draft.volume, 0, 200, "%d%%");
    ImGui::Checkbox("Mute", &draft.mute);
    ImGui::SameLine();
    ImGui::Checkbox("Mute when unfocused", &draft.muteUnfocused);
    const auto *device = draft.audioDevice.empty() ? "System default" : draft.audioDevice.c_str();
    if (ImGui::BeginCombo("Output device", device)) {
      if (ImGui::Selectable("System default", draft.audioDevice.empty()))
        draft.audioDevice.clear();
      for (size_t i = 0; i < audioDevices.size(); ++i) {
        ImGui::PushID(int(i));
        if (ImGui::Selectable(audioDevices[i].c_str(), draft.audioDevice == audioDevices[i]))
          draft.audioDevice = audioDevices[i];
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::SeparatorText("Focus");
    int focus = int(draft.focus);
    if (ImGui::Combo("When unfocused", &focus,
                     "Pause emulation\0Block input\0Continue with input\0"))
      draft.focus = FocusBehavior(focus);
  }

  void Input() {
    if (ImGui::Combo("Player", &inputPlayer,
                     "1\0"
                     "2\0"
                     "3\0"
                     "4\0"
                     "5\0"
                     "6\0"
                     "7\0"
                     "8\0"))
      capturing = -1;
    auto &player = draft.input.players[inputPlayer];
    std::string selected = player.device == "auto"   ? "Automatic player slot"
                           : player.device == "none" ? "Disabled"
                                                     : "Unavailable saved gamepad";
    for (const auto &pad : gamepads)
      if (pad.key == player.device)
        selected = pad.name;
    ImGui::SetNextItemWidth(300);
    if (ImGui::BeginCombo("Gamepad", selected.c_str())) {
      if (ImGui::Selectable("Automatic player slot", player.device == "auto"))
        player.device = "auto";
      if (ImGui::Selectable("Disabled", player.device == "none"))
        player.device = "none";
      for (size_t index = 0; index < gamepads.size(); ++index) {
        ImGui::PushID(int(index));
        if (ImGui::Selectable(("Device " + std::to_string(index + 1)).c_str(),
                              player.device == gamepads[index].key))
          player.device = gamepads[index].key;
        ImGui::SameLine();
        ImGui::TextUnformatted(gamepads[index].name.c_str());
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
      gamepads = SdlInputProvider::ConnectedGamepads();
    ImGui::SliderInt("Axis threshold", &player.axisThreshold, 1, 32767);
    if (ImGui::Combo("Binding", &inputSlot,
                     "1\0"
                     "2\0"
                     "3\0"
                     "4\0"))
      capturing = -1;
    ImGui::TextWrapped(
        "Select a binding, then press a key, pad control, or mouse button. Escape cancels.");
    if (ImGui::BeginTable(
            "Bindings", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
            {0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing()})) {
      ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Clear", ImGuiTableColumnFlags_WidthFixed, 64);
      for (unsigned i = 0; i < controllerButtons.size(); ++i) {
        ImGui::PushID(int(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(controllerButtons[i].label);
        ImGui::TableNextColumn();
        auto &binding = player.buttons[i][inputSlot];
        if (ImGui::Button(capturing == int(i) ? "Press a control..." : binding.Label().c_str(),
                          {-1, 0})) {
          capturing = int(i);
          capturedPlayer = inputPlayer;
          capturedInputSlot = inputSlot;
          hotkeyCommand = -1;
        }
        ImGui::TableNextColumn();
        if (ImGui::Button("Clear")) {
          binding = {};
          capturing = -1;
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (ImGui::Button("Restore player defaults")) {
      player = PlayerBindings::Defaults(inputPlayer);
      capturing = -1;
    }
  }

  void Paths() {
    ImGui::TextWrapped("Relative paths start at the folder containing the preferences file.");
    ImGui::TextUnformatted("Firmware directory (used when opening the next game)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##firmware", &draft.firmwareDirectory);
    ImGui::TextUnformatted("Screenshot directory (blank uses the default)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##screenshots", &draft.screenshotDirectory);
    ImGui::Spacing();
    ImGui::TextUnformatted("Preferences file");
    ImGui::TextWrapped("%s", file.c_str());
  }
  void Devices() {
    ImGui::TextWrapped("Apply connects these devices without restarting the game.");
    constexpr std::array names{"None",        "Gamepad",   "Mouse",      "Multitap",
                               "Super Scope", "Justifier", "Justifiers", "MACS rifle"};
    int player = 1;
    for (size_t port = 0; port < draft.console.ports.size(); ++port) {
      ImGui::PushID(int(port));
      ImGui::Text("Port %u", unsigned(port + 1));
      auto &device = draft.console.ports[port];
      if (ImGui::BeginCombo("##device", names[size_t(device)])) {
        const size_t count = port == 0 ? 4 : names.size();
        for (size_t index = 0; index < count; ++index) {
          const bool selected = size_t(device) == index;
          if (ImGui::Selectable(names[index], selected))
            device = core::ControllerDevice(index);
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      const int count = device == core::ControllerDevice::Multitap ? 4 : 1;
      if (device == core::ControllerDevice::Gamepad)
        ImGui::TextDisabled("Player %d", player);
      else if (device == core::ControllerDevice::Multitap)
        ImGui::TextDisabled("Players %d through %d", player, player + count - 1);
      player += count;
      ImGui::PopID();
    }
    ImGui::SeparatorText("Expansion");
    int expansion = int(draft.console.expansion);
    if (ImGui::Combo("Device", &expansion, "None\0Satellaview\0"))
      draft.console.expansion = core::Emulator::ExpansionDevice(expansion);
  }
  void HotkeyBindings() {
    ImGui::TextWrapped("Hold modifiers and press a key. Escape cancels assignment.");
    ImGui::Combo("Binding", &hotkeySlot,
                 "1\0"
                 "2\0"
                 "3\0"
                 "4\0");
    hotkeyFilter.Draw("Find action");
    if (hotkeyCommand >= 0) {
      ImGui::TextWrapped("Assign: %s", desktopCommands[hotkeyCommand].label);
      if (ImGui::Button("Use Escape"))
        AssignHotkey({SDL_SCANCODE_ESCAPE, 0});
      ImGui::SameLine();
      if (ImGui::Button("Cancel assignment"))
        hotkeyCommand = -1;
    }
    const float tableHeight =
        std::max(ImGui::GetFrameHeight(),
                 ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing());
    if (ImGui::BeginTable("Hotkeys", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_ScrollY,
                          {0, tableHeight})) {
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Clear", ImGuiTableColumnFlags_WidthFixed, 64);
      for (size_t command = 0; command < desktopCommands.size(); ++command) {
        if (!hotkeyFilter.PassFilter(desktopCommands[command].label))
          continue;
        ImGui::PushID(int(command));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(desktopCommands[command].label);
        ImGui::TableNextColumn();
        auto &chord = draft.hotkeys.bindings[command][hotkeySlot];
        auto label = chord.Label();
        if (label.empty())
          label = "Unassigned";
        if (hotkeyCommand == int(command) && capturedSlot == hotkeySlot)
          label = "Press a key...";
        if (ImGui::Button(label.c_str(), {-1, 0})) {
          hotkeyCommand = int(command);
          capturedSlot = hotkeySlot;
          pendingModifier = {};
          capturing = -1;
        }
        ImGui::TableNextColumn();
        if (ImGui::Button("Clear")) {
          chord = {};
          hotkeyCommand = -1;
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (ImGui::Button("Restore hotkey defaults")) {
      draft.hotkeys = {};
      hotkeyCommand = -1;
    }
  }
};

PreferencesDialog::PreferencesDialog() : impl_(std::make_unique<Impl>()) {}
PreferencesDialog::~PreferencesDialog() = default;
void PreferencesDialog::Open(const Preferences &current, const std::filesystem::path &file) {
  if (impl_->active)
    return;
  impl_->draft = current;
  impl_->file = core::PathText(file);
  impl_->audioDevices = SdlAudioOutput::DeviceNames();
  impl_->gamepads = SdlInputProvider::ConnectedGamepads();
  impl_->error.clear();
  impl_->changes.reset();
  impl_->active = impl_->opening = true;
  impl_->capturing = -1;
  impl_->inputPlayer = impl_->inputSlot = 0;
  impl_->hotkeyCommand = -1;
  impl_->pendingModifier = {};
}
bool PreferencesDialog::Active() const noexcept {
  return impl_->active;
}
bool PreferencesDialog::ProcessEvent(const SDL_Event &event) {
  auto &self = *impl_;
  if (!self.active)
    return false;
  if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED)
    self.gamepads = SdlInputProvider::ConnectedGamepads();
  if (self.capturing >= 0) {
    InputBinding binding;
    auto &player = self.draft.input.players[self.capturedPlayer];
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
      if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
        self.capturing = -1;
        return true;
      }
      if (event.key.scancode > SDL_SCANCODE_UNKNOWN && event.key.scancode < SDL_SCANCODE_COUNT)
        binding = {InputBinding::Kind::Keyboard, event.key.scancode};
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button >= 1 &&
               event.button.button <= 5)
      binding = {InputBinding::Kind::MouseButton, event.button.button};
    else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
             event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
      const auto id =
          event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? event.gbutton.which : event.gaxis.which;
      const auto found = std::find_if(self.gamepads.begin(), self.gamepads.end(),
                                      [&](const auto &pad) { return pad.id == id; });
      if (found == self.gamepads.end() || player.device == "none" ||
          (player.device != "auto" && player.device != found->key))
        return true;
      if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        binding = {InputBinding::Kind::GamepadButton, event.gbutton.button};
      else if (event.gaxis.value > player.axisThreshold ||
               event.gaxis.value < -player.axisThreshold)
        binding = {event.gaxis.value > 0 ? InputBinding::Kind::AxisPositive
                                         : InputBinding::Kind::AxisNegative,
                   event.gaxis.axis};
    }
    if (binding.kind != InputBinding::Kind::None) {
      binding.Validate();
      player.buttons[self.capturing][self.capturedInputSlot] = binding;
      self.capturing = -1;
    }
    return event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
           event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
           event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
  }
  if (self.hotkeyCommand >= 0 && event.type == SDL_EVENT_KEY_UP &&
      self.pendingModifier.scancode != SDL_SCANCODE_UNKNOWN &&
      event.key.scancode == self.pendingModifier.scancode) {
    self.AssignHotkey(self.pendingModifier);
    return true;
  }
  if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat)
    return false;
  if (self.hotkeyCommand >= 0) {
    const auto chord = KeyChord::From(event.key);
    if (chord.scancode == SDL_SCANCODE_ESCAPE && chord.modifiers == 0)
      self.hotkeyCommand = -1;
    else if (chord.scancode >= SDL_SCANCODE_LCTRL && chord.scancode <= SDL_SCANCODE_RGUI)
      self.pendingModifier = chord;
    else
      self.AssignHotkey(chord);
    return true;
  }
  if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
    self.active = false;
    return true;
  }
  return false;
}
void PreferencesDialog::Draw() {
  auto &self = *impl_;
  if (!self.active)
    return;
  const auto *viewport = ImGui::GetMainViewport();
  if (self.opening) {
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(
        {std::min(620.0f, viewport->Size.x - 16), std::min(560.0f, viewport->Size.y - 56)},
        ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    self.opening = false;
  }
  if (ImGui::Begin("Preferences", &self.active,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    const float errorHeight = ImGui::GetTextLineHeightWithSpacing() * 3;
    const float footer = ImGui::GetFrameHeightWithSpacing() +
                         (self.error.empty() ? 0 : errorHeight + ImGui::GetStyle().ItemSpacing.y);
    if (ImGui::BeginChild("Pages", {0, -footer})) {
      if (ImGui::BeginTabBar("Categories")) {
        if (ImGui::BeginTabItem("General")) {
          self.General();
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Input")) {
          self.Input();
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Paths")) {
          self.Paths();
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Devices")) {
          self.Devices();
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Hotkeys")) {
          self.HotkeyBindings();
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    }
    ImGui::EndChild();
    if (!self.error.empty()) {
      ImGui::BeginChild("Apply error", {0, errorHeight});
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 0.55f, 0.45f, 1});
      ImGui::TextWrapped("%s", self.error.c_str());
      ImGui::PopStyleColor();
      ImGui::EndChild();
    }
    ImGui::BeginDisabled(self.Capturing());
    if (ImGui::Button("OK", {64, 0})) {
      self.changes = self.draft;
      self.closeOnApply = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply", {64, 0})) {
      self.changes = self.draft;
      self.closeOnApply = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {64, 0}))
      self.active = false;
  }
  ImGui::End();
}
std::optional<Preferences> PreferencesDialog::TakeChanges() {
  return std::exchange(impl_->changes, {});
}
void PreferencesDialog::Applied() {
  impl_->error.clear();
  if (impl_->closeOnApply)
    impl_->active = false;
}
void PreferencesDialog::Rejected(std::string error) {
  impl_->error = std::move(error);
}
} // namespace snes::frontend
