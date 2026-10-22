#pragma once

#include <array>
#include <cstddef>
#include <string_view>

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
  Preferences,
  Count
};
struct CommandDescription {
  std::string_view key;
  const char *label;
};
inline constexpr std::array<CommandDescription, size_t(DesktopCommand::Count)> desktopCommands{
    {{"open", "Open game"},
     {"open-handheld", "Open Super Game Boy"},
     {"open-sufami", "Open Sufami Turbo"},
     {"open-broadcast", "Open broadcast cartridge"},
     {"open-campus", "Open Campus Challenge 92"},
     {"open-powerfest", "Open PowerFest 94"},
     {"unload", "Unload game"},
     {"quit", "Quit"},
     {"pause", "Pause or resume"},
     {"frame-advance", "Frame advance"},
     {"reset", "Reset"},
     {"power-cycle", "Power cycle"},
     {"fullscreen", "Fullscreen"},
     {"screenshot", "Screenshot"},
     {"clear-recent", "Clear recent games"},
     {"recent1", "Recent game 1"},
     {"recent2", "Recent game 2"},
     {"recent3", "Recent game 3"},
     {"recent4", "Recent game 4"},
     {"recent5", "Recent game 5"},
     {"recent6", "Recent game 6"},
     {"recent7", "Recent game 7"},
     {"recent8", "Recent game 8"},
     {"recent9", "Recent game 9"},
     {"recent10", "Recent game 10"},
     {"preferences", "Preferences"}}};
} // namespace snes::frontend
