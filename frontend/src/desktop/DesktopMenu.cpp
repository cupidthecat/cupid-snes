#include "DesktopMenu.hpp"
#include "SdlInputProvider.hpp"

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
    Hotkeys hotkeys;
    HotkeyDispatcher dispatcher;
    GamepadHotkeyDispatcher gamepadDispatcher;
    std::map<SDL_JoystickID, std::string> gamepadKeys;
    bool loaded = false, paused = false;
    bool undo = false, redo = false;
    unsigned stateSlot = 0;

    void Item(const char* label, DesktopCommand command, bool enabled = true,
              bool selected = false) {
        const auto shortcut = hotkeys.Label(command);
        if (ImGui::MenuItem(label, shortcut.c_str(), selected, enabled))
            commands.push_back(command);
    }
};

DesktopMenu::DesktopMenu() : impl_(std::make_unique<Impl>()) {
}

DesktopMenu::~DesktopMenu() = default;

void DesktopMenu::SetRecent(std::span<const LaunchRequest> requests) {
    auto& names = impl_->recent;
    names.clear();
    for (const auto& request : requests.first(std::min<size_t>(requests.size(), 10))) {
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

void DesktopMenu::UpdateStates(unsigned slot, bool undo, bool redo) {
    impl_->stateSlot = slot;
    impl_->undo = undo;
    impl_->redo = redo;
}

void DesktopMenu::Draw(bool enabled) {
    auto& self = *impl_;
    if (!ImGui::BeginMainMenuBar())
        return;
    using C = DesktopCommand;
    if (ImGui::BeginMenu("File", enabled)) {
        self.Item("Open Game...", C::Open);
        self.Item("Open Super Game Boy...", C::OpenHandheld);
        self.Item("Open Sufami Turbo...", C::OpenSufami);
        self.Item("Open Broadcast Cartridge...", C::OpenBroadcast);
        self.Item("Open Campus Challenge 92...", C::OpenCampus);
        self.Item("Open PowerFest 94...", C::OpenPowerFest);
        if (ImGui::BeginMenu("Recent Games")) {
            if (self.recent.empty())
                ImGui::TextDisabled("No recent games");
            for (unsigned i = 0; i < self.recent.size(); ++i) {
                ImGui::PushID(int(i));
                // Filename characters must not become widget identifiers.
                const auto size = ImGui::CalcTextSize(self.recent[i].c_str(), nullptr, false);
                const auto command = C(unsigned(C::Recent0) + i);
                const auto shortcut = self.hotkeys.Label(command);
                auto rowSize = size;
                if (!shortcut.empty())
                    rowSize.x += 24 + ImGui::CalcTextSize(shortcut.c_str()).x;
                const auto position = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##recent", false, 0, rowSize))
                    self.commands.push_back(command);
                ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text),
                                                    self.recent[i].c_str());
                if (!shortcut.empty())
                    ImGui::GetWindowDrawList()->AddText({position.x + size.x + 24, position.y},
                                                        ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                                        shortcut.c_str());
                ImGui::PopID();
            }
            if (!self.recent.empty()) {
                ImGui::Separator();
                self.Item("Clear Recent Games", C::ClearRecent);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        self.Item("Unload Game", C::Unload, self.loaded);
        self.Item("Preferences...", C::Preferences);
        self.Item("Quit", C::Quit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Game", enabled)) {
        self.Item("Pause", C::Pause, self.loaded, self.paused);
        self.Item("Frame Advance", C::FrameAdvance, self.loaded);
        self.Item("Reset", C::Reset, self.loaded);
        self.Item("Power Cycle", C::PowerCycle, self.loaded);
        ImGui::Separator();
        const auto save = "Save Quick State (" + std::to_string(self.stateSlot + 1) + ")";
        const auto load = "Load Quick State (" + std::to_string(self.stateSlot + 1) + ")";
        self.Item(save.c_str(), C::QuickSave, self.loaded);
        self.Item(load.c_str(), C::QuickLoad, self.loaded);
        if (ImGui::BeginMenu("Quick Slot", self.loaded)) {
            for (unsigned slot = 0; slot < 10; ++slot) {
                const auto label = std::to_string(slot + 1);
                self.Item(label.c_str(), C(unsigned(C::Slot0) + slot), true,
                          self.stateSlot == slot);
            }
            ImGui::Separator();
            self.Item("Previous Slot", C::PreviousSlot);
            self.Item("Next Slot", C::NextSlot);
            ImGui::EndMenu();
        }
        self.Item("State Manager...", C::StateManager, self.loaded);
        self.Item("Undo State Load", C::UndoState, self.loaded && self.undo);
        self.Item("Redo State Load", C::RedoState, self.loaded && self.redo);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View", enabled)) {
        self.Item("Fullscreen", C::Fullscreen);
        self.Item("Screenshot", C::Screenshot, self.loaded);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools", enabled)) {
        self.Item("Cheats...", C::CheatEditor, self.loaded);
        self.Item("Toggle Cheats", C::ToggleCheats, self.loaded);
        self.Item("Memory...", C::MemoryViewer, self.loaded);
        self.Item("RAM Search...", C::RamSearch, self.loaded);
        self.Item("CPU Debugger...", C::CpuDebugger, self.loaded);
        self.Item("Graphics Inspector...", C::GraphicsInspector, self.loaded);
        self.Item("Audio Inspector...", C::AudioInspector, self.loaded);
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

void DesktopMenu::SetHotkeys(const Hotkeys& hotkeys) {
    impl_->hotkeys = hotkeys;
    ResetHotkeys();
}

std::optional<DesktopCommand> DesktopMenu::KeyCommand(const SDL_KeyboardEvent& key) {
    return impl_->dispatcher.Process(impl_->hotkeys, key);
}

void DesktopMenu::ResetHotkeys() noexcept {
    impl_->dispatcher.Reset();
    impl_->gamepadDispatcher.Synchronize(impl_->hotkeys);
}

std::vector<DesktopCommand> DesktopMenu::GamepadCommands(const SDL_Event& event, bool enabled) {
    auto& self = *impl_;
    const auto type = event.type;
    SDL_JoystickID id = 0;
    if (type == SDL_EVENT_GAMEPAD_ADDED || type == SDL_EVENT_GAMEPAD_REMOVED)
        id = event.gdevice.which;
    else if (type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        id = event.gbutton.which;
        if (type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
            self.dispatcher.Reset();
    } else if (type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        id = event.gaxis.which;
        if (event.gaxis.value > 16000 || event.gaxis.value < -16000)
            self.dispatcher.Reset();
    } else
        return {};
    if (type == SDL_EVENT_GAMEPAD_REMOVED) {
        self.gamepadKeys.erase(id);
        return self.gamepadDispatcher.Process(self.hotkeys, event, {}, false);
    }
    if (!self.gamepadKeys.contains(id))
        for (const auto& pad : SdlInputProvider::ConnectedGamepads())
            self.gamepadKeys[pad.id] = pad.key;
    const auto found = self.gamepadKeys.find(id);
    if (found == self.gamepadKeys.end())
        return {};
    if (type == SDL_EVENT_GAMEPAD_ADDED) {
        if (auto* pad = SDL_OpenGamepad(id)) {
            SDL_Event snapshot{};
            snapshot.gbutton.which = id;
            for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
                snapshot.type = SDL_GetGamepadButton(pad, SDL_GamepadButton(button))
                                    ? SDL_EVENT_GAMEPAD_BUTTON_DOWN
                                    : SDL_EVENT_GAMEPAD_BUTTON_UP;
                snapshot.gbutton.button = Uint8(button);
                self.gamepadDispatcher.Process(self.hotkeys, snapshot, found->second, false);
            }
            snapshot = {};
            snapshot.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
            snapshot.gaxis.which = id;
            for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
                snapshot.gaxis.axis = Uint8(axis);
                snapshot.gaxis.value = SDL_GetGamepadAxis(pad, SDL_GamepadAxis(axis));
                self.gamepadDispatcher.Process(self.hotkeys, snapshot, found->second, false);
            }
            SDL_CloseGamepad(pad);
        }
        return {};
    }
    return self.gamepadDispatcher.Process(self.hotkeys, event, found->second, enabled);
}

} // namespace snes::frontend
