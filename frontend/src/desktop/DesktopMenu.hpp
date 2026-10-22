#pragma once

#include "session/LaunchRequest.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <optional>
#include <span>

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
  Screenshot,
  ClearRecent,
  Recent0,
  Recent1,
  Recent2,
  Recent3,
  Recent4,
  Recent5,
  Recent6,
  Recent7,
  Recent8,
  Recent9,
  Preferences
};

class DesktopMenu {
public:
  DesktopMenu();
  ~DesktopMenu();
  void Update(bool loaded, bool paused);
  void SetRecent(std::span<const LaunchRequest> requests);
  void Draw(bool enabled = true);
  std::optional<DesktopCommand> TakeCommand();
  static std::optional<DesktopCommand> KeyCommand(const SDL_KeyboardEvent &key);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace snes::frontend
