#pragma once

#include <SDL3/SDL.h>
#include <memory>
#include <optional>

namespace snes::frontend {
enum class DesktopCommand {
  Open,
  OpenHandheld,
  OpenSufami,
  OpenBroadcast,
  OpenCampus,
  OpenPowerFest,
  Unload,
  Quit,
  Pause,
  FrameAdvance,
  Reset,
  PowerCycle,
  Fullscreen,
  Screenshot
};

class DesktopMenu {
public:
  explicit DesktopMenu(SDL_Window *window);
  ~DesktopMenu();
  void Update(bool loaded, bool paused);
  std::optional<DesktopCommand> TakeCommand();
  static std::optional<DesktopCommand> KeyCommand(const SDL_KeyboardEvent &key);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace snes::frontend
