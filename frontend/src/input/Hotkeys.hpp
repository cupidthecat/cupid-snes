#pragma once

#include "desktop/Commands.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <optional>
#include <string>

namespace snes::frontend {
struct KeyChord {
  enum Modifier : unsigned { Shift = 1, Control = 2, Alt = 4, Gui = 8 };
  SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
  unsigned modifiers = 0;
  bool operator==(const KeyChord &) const = default;
  static KeyChord From(const SDL_KeyboardEvent &key);
  std::string Label() const;
};

struct Hotkeys {
  static constexpr size_t bindingCount = 4;
  std::array<std::array<KeyChord, bindingCount>, desktopCommands.size()> bindings{};
  Hotkeys();
  std::optional<DesktopCommand> Match(const SDL_KeyboardEvent &key) const;
  std::string Label(DesktopCommand command) const;
  void Validate() const;
  bool operator==(const Hotkeys &) const = default;
};
class HotkeyDispatcher {
public:
  std::optional<DesktopCommand> Process(const Hotkeys &hotkeys, const SDL_KeyboardEvent &key);
  void Reset() noexcept {
    pendingModifier_.reset();
  }

private:
  std::optional<SDL_KeyboardEvent> pendingModifier_;
};
} // namespace snes::frontend
