#pragma once

#include "input/Hotkeys.hpp"
#include "session/LaunchRequest.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <optional>
#include <span>

namespace snes::frontend {
class DesktopMenu {
public:
  DesktopMenu();
  ~DesktopMenu();
  void Update(bool loaded, bool paused);
  void SetRecent(std::span<const LaunchRequest> requests);
  void Draw(bool enabled = true);
  std::optional<DesktopCommand> TakeCommand();
  void SetHotkeys(const Hotkeys &hotkeys);
  std::optional<DesktopCommand> KeyCommand(const SDL_KeyboardEvent &key);
  std::vector<DesktopCommand> GamepadCommands(const SDL_Event &event, bool enabled);
  void ResetHotkeys() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace snes::frontend
