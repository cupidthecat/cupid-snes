#pragma once

#include "InputBindings.hpp"
#include "desktop/Commands.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace snes::frontend {
struct KeyChord {
    enum Modifier : unsigned { Shift = 1, Control = 2, Alt = 4, Gui = 8 };

    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    unsigned modifiers = 0;
    bool operator==(const KeyChord&) const = default;
    static KeyChord From(const SDL_KeyboardEvent& key);
    std::string Label() const;
};

struct GamepadChord {
    std::string device = "any";
    int axisThreshold = 16000;
    InputAlternatives controls{};
    bool operator==(const GamepadChord&) const = default;
    bool Assigned() const;
    std::string Label() const;
    void Validate() const;
};

struct Hotkeys {
    static constexpr size_t bindingCount = 4;
    std::array<std::array<KeyChord, bindingCount>, desktopCommands.size()> bindings{};
    std::array<std::array<GamepadChord, bindingCount>, desktopCommands.size()> gamepads{};
    Hotkeys();
    std::optional<DesktopCommand> Match(const SDL_KeyboardEvent& key) const;
    std::string Label(DesktopCommand command) const;
    void Validate() const;
    bool operator==(const Hotkeys&) const = default;
};

class HotkeyDispatcher {
  public:
    std::optional<DesktopCommand> Process(const Hotkeys& hotkeys, const SDL_KeyboardEvent& key);

    bool Held(DesktopCommand command) const noexcept {
        return held_[size_t(command)].scancode != SDL_SCANCODE_UNKNOWN;
    }

    void Reset() noexcept {
        pendingModifier_.reset();
        held_.fill({});
    }

  private:
    std::optional<SDL_KeyboardEvent> pendingModifier_;
    std::array<KeyChord, desktopCommands.size()> held_{};
};

class GamepadHotkeyDispatcher {
  public:
    std::vector<DesktopCommand> Process(const Hotkeys& hotkeys, const SDL_Event& event,
                                        const std::string& device, bool enabled = true);
    void Synchronize(const Hotkeys& hotkeys) noexcept;

    bool Held(DesktopCommand command) const noexcept {
        return held_[size_t(command)];
    }

  private:
    struct State {
        std::string device;
        std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
        std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> axes{};
    };

    std::map<SDL_JoystickID, State> states_;
    std::array<bool, desktopCommands.size()> down_{};
    std::array<bool, desktopCommands.size()> held_{};
    std::array<bool, desktopCommands.size()> Active(const Hotkeys& hotkeys) const noexcept;
};
} // namespace snes::frontend
