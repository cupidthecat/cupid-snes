#include "Hotkeys.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
namespace {
unsigned OwnModifier(SDL_Scancode key) {
    switch (key) {
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
        return KeyChord::Shift;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
        return KeyChord::Control;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT:
        return KeyChord::Alt;
    case SDL_SCANCODE_LGUI:
    case SDL_SCANCODE_RGUI:
        return KeyChord::Gui;
    default:
        return 0;
    }
}
} // namespace

bool GamepadChord::Assigned() const {
    return std::any_of(controls.begin(), controls.end(), [](const auto& control) {
        return control.kind != InputBinding::Kind::None;
    });
}

std::string GamepadChord::Label() const {
    std::string result;
    for (const auto& control : controls) {
        if (control.kind == InputBinding::Kind::None)
            continue;
        if (!result.empty())
            result += " + ";
        result += control.Label();
    }
    return result;
}

void GamepadChord::Validate() const {
    if (device.empty() || device.size() > 4096 || device.find('\0') != std::string::npos ||
        axisThreshold < 1 || axisThreshold > 32767)
        throw std::invalid_argument("Gamepad shortcut contains an invalid host or threshold");
    for (size_t index = 0; index < controls.size(); ++index) {
        const auto& control = controls[index];
        control.Validate();
        using Kind = InputBinding::Kind;
        if (control.kind == Kind::None)
            continue;
        if (control.kind != Kind::GamepadButton && control.kind != Kind::AxisPositive &&
            control.kind != Kind::AxisNegative)
            throw std::invalid_argument("Gamepad shortcuts accept gamepad buttons and signed axes");
        for (size_t prior = 0; prior < index; ++prior) {
            const auto& other = controls[prior];
            if (other == control ||
                (other.control == control.control &&
                 ((other.kind == Kind::AxisPositive && control.kind == Kind::AxisNegative) ||
                  (other.kind == Kind::AxisNegative && control.kind == Kind::AxisPositive))))
                throw std::invalid_argument(
                    "Gamepad shortcut contains a repeated or opposing control");
        }
    }
}

KeyChord KeyChord::From(const SDL_KeyboardEvent& key) {
    unsigned modifiers = 0;
    if (key.mod & SDL_KMOD_SHIFT)
        modifiers |= Shift;
    if (key.mod & SDL_KMOD_CTRL)
        modifiers |= Control;
    if (key.mod & SDL_KMOD_ALT)
        modifiers |= Alt;
    if (key.mod & SDL_KMOD_GUI)
        modifiers |= Gui;
    return {key.scancode, modifiers & ~OwnModifier(key.scancode)};
}

std::string KeyChord::Label() const {
    if (scancode == SDL_SCANCODE_UNKNOWN)
        return {};
    std::string text;
    if (modifiers & Control)
        text += "Ctrl+";
    if (modifiers & Alt)
        text += "Alt+";
    if (modifiers & Shift)
        text += "Shift+";
    if (modifiers & Gui)
        text += "Super+";
    text += SDL_GetScancodeName(scancode);
    return text;
}

Hotkeys::Hotkeys() {
    const auto set = [&](DesktopCommand command, SDL_Scancode key, unsigned mods = 0,
                         size_t slot = 0) { bindings[size_t(command)][slot] = {key, mods}; };
    set(DesktopCommand::Open, SDL_SCANCODE_O, KeyChord::Control);
    set(DesktopCommand::Unload, SDL_SCANCODE_W, KeyChord::Control);
    set(DesktopCommand::Quit, SDL_SCANCODE_Q, KeyChord::Control);
    set(DesktopCommand::Quit, SDL_SCANCODE_ESCAPE, 0, 1);
    set(DesktopCommand::Pause, SDL_SCANCODE_P);
    set(DesktopCommand::FrameAdvance, SDL_SCANCODE_N);
    set(DesktopCommand::Reset, SDL_SCANCODE_F5);
    set(DesktopCommand::Fullscreen, SDL_SCANCODE_F11);
    set(DesktopCommand::Fullscreen, SDL_SCANCODE_RETURN, KeyChord::Alt, 1);
    set(DesktopCommand::Screenshot, SDL_SCANCODE_F12);
    set(DesktopCommand::Preferences, SDL_SCANCODE_COMMA, KeyChord::Control);
    set(DesktopCommand::QuickSave, SDL_SCANCODE_F2);
    set(DesktopCommand::StateManager, SDL_SCANCODE_F3);
    set(DesktopCommand::QuickLoad, SDL_SCANCODE_F4);
    set(DesktopCommand::PreviousSlot, SDL_SCANCODE_F6);
    set(DesktopCommand::NextSlot, SDL_SCANCODE_F7);
    set(DesktopCommand::UndoState, SDL_SCANCODE_Z, KeyChord::Control);
    set(DesktopCommand::RedoState, SDL_SCANCODE_Y, KeyChord::Control);
    set(DesktopCommand::CheatEditor, SDL_SCANCODE_C, KeyChord::Control | KeyChord::Shift);
    set(DesktopCommand::ToggleCheats, SDL_SCANCODE_F8);
    set(DesktopCommand::MemoryViewer, SDL_SCANCODE_M, KeyChord::Control | KeyChord::Shift);
    set(DesktopCommand::RamSearch, SDL_SCANCODE_F, KeyChord::Control | KeyChord::Shift);
    set(DesktopCommand::CpuDebugger, SDL_SCANCODE_D, KeyChord::Control | KeyChord::Shift);
    set(DesktopCommand::GraphicsInspector, SDL_SCANCODE_V, KeyChord::Control | KeyChord::Shift);
    set(DesktopCommand::AudioInspector, SDL_SCANCODE_A, KeyChord::Control | KeyChord::Shift);
}

std::optional<DesktopCommand> Hotkeys::Match(const SDL_KeyboardEvent& key) const {
    if (key.type != SDL_EVENT_KEY_DOWN || key.repeat || key.scancode == SDL_SCANCODE_UNKNOWN)
        return {};
    const auto chord = KeyChord::From(key);
    for (size_t command = 0; command < bindings.size(); ++command)
        if (std::find(bindings[command].begin(), bindings[command].end(), chord) !=
            bindings[command].end())
            return DesktopCommand(command);
    return {};
}

std::string Hotkeys::Label(DesktopCommand command) const {
    std::string label;
    for (const auto& chord : bindings.at(size_t(command))) {
        const auto text = chord.Label();
        if (text.empty())
            continue;
        if (!label.empty())
            label += " / ";
        label += text;
    }
    for (const auto& chord : gamepads.at(size_t(command))) {
        const auto text = chord.Label();
        if (text.empty())
            continue;
        if (!label.empty())
            label += " / ";
        label += text;
    }
    return label;
}

void Hotkeys::Validate() const {
    std::map<std::pair<int, unsigned>, size_t> assigned;
    for (size_t command = 0; command < bindings.size(); ++command) {
        for (const auto& chord : gamepads[command])
            chord.Validate();
        for (const auto& chord : bindings[command]) {
            if (chord.scancode < SDL_SCANCODE_UNKNOWN || chord.scancode >= SDL_SCANCODE_COUNT ||
                chord.modifiers > 15 || (chord.modifiers & OwnModifier(chord.scancode)) ||
                (chord.scancode == SDL_SCANCODE_UNKNOWN && chord.modifiers))
                throw std::invalid_argument("Hotkey contains an invalid key or modifier");
            if (chord.scancode == SDL_SCANCODE_UNKNOWN)
                continue;
            const auto [entry, inserted] =
                assigned.emplace(std::pair{int(chord.scancode), chord.modifiers}, command);
            if (!inserted && entry->second != command)
                throw std::invalid_argument(chord.Label() + " is assigned to both " +
                                            desktopCommands[entry->second].label + " and " +
                                            desktopCommands[command].label);
        }
    }
    const auto subset = [](const GamepadChord& a, const GamepadChord& b) {
        return std::all_of(a.controls.begin(), a.controls.end(), [&](const auto& control) {
            return control.kind == InputBinding::Kind::None ||
                   std::find(b.controls.begin(), b.controls.end(), control) != b.controls.end();
        });
    };
    for (size_t command = 0; command < gamepads.size(); ++command) {
        for (const auto& chord : gamepads[command]) {
            if (!chord.Assigned())
                continue;
            for (size_t other = 0; other < command; ++other) {
                for (const auto& prior : gamepads[other]) {
                    if (!prior.Assigned() || (chord.device != "any" && prior.device != "any" &&
                                              chord.device != prior.device))
                        continue;
                    if (subset(chord, prior) || subset(prior, chord))
                        throw std::invalid_argument("Gamepad shortcuts overlap between " +
                                                    std::string(desktopCommands[other].label) +
                                                    " and " + desktopCommands[command].label);
                }
            }
        }
    }
}

void GamepadHotkeyDispatcher::Synchronize(const Hotkeys& hotkeys) noexcept {
    down_ = Active(hotkeys);
}

std::vector<DesktopCommand> GamepadHotkeyDispatcher::Process(const Hotkeys& hotkeys,
                                                             const SDL_Event& event,
                                                             const std::string& device,
                                                             bool enabled) {
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
        states_.erase(event.gdevice.which);
    else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
             event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        if (event.gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT || device.empty())
            return {};
        auto& state = states_[event.gbutton.which];
        state.device = device;
        state.buttons[event.gbutton.button] = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        if (event.gaxis.axis >= SDL_GAMEPAD_AXIS_COUNT || device.empty())
            return {};
        auto& state = states_[event.gaxis.which];
        state.device = device;
        state.axes[event.gaxis.axis] = event.gaxis.value;
    } else
        return {};
    std::vector<DesktopCommand> commands;
    const auto active = Active(hotkeys);
    for (size_t command = 0; command < active.size(); ++command)
        if (enabled && active[command] && !down_[command])
            commands.push_back(DesktopCommand(command));
    down_ = active;
    return commands;
}

std::array<bool, desktopCommands.size()>
GamepadHotkeyDispatcher::Active(const Hotkeys& hotkeys) const noexcept {
    std::array<bool, desktopCommands.size()> active{};
    for (size_t command = 0; command < hotkeys.gamepads.size(); ++command) {
        bool down = false;
        for (const auto& chord : hotkeys.gamepads[command]) {
            if (!chord.Assigned())
                continue;
            for (const auto& [id, state] : states_) {
                if (chord.device != "any" && chord.device != state.device)
                    continue;
                down |= std::all_of(
                    chord.controls.begin(), chord.controls.end(), [&](const auto& control) {
                        using Kind = InputBinding::Kind;
                        switch (control.kind) {
                        case Kind::GamepadButton:
                            return state.buttons[control.control];
                        case Kind::AxisPositive:
                            return state.axes[control.control] > chord.axisThreshold;
                        case Kind::AxisNegative:
                            return state.axes[control.control] < -chord.axisThreshold;
                        default:
                            return true;
                        }
                    });
            }
        }
        active[command] = down;
    }
    return active;
}

std::optional<DesktopCommand> HotkeyDispatcher::Process(const Hotkeys& hotkeys,
                                                        const SDL_KeyboardEvent& key) {
    if (key.type == SDL_EVENT_KEY_DOWN) {
        if (key.repeat) {
            if (!OwnModifier(key.scancode))
                pendingModifier_.reset();
            return {};
        }
        pendingModifier_.reset();
        if (OwnModifier(key.scancode)) {
            pendingModifier_ = key;
            return {};
        }
        return hotkeys.Match(key);
    }
    if (key.type == SDL_EVENT_KEY_UP && pendingModifier_ &&
        pendingModifier_->scancode == key.scancode) {
        const auto pressed = *pendingModifier_;
        pendingModifier_.reset();
        return hotkeys.Match(pressed);
    }
    return {};
}
} // namespace snes::frontend
