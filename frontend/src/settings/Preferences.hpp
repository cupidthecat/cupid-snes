#pragma once

#include "ConsoleDevices.hpp"
#include "input/Hotkeys.hpp"
#include "input/InputBindings.hpp"
#include "session/LaunchRequest.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace snes::frontend {
enum class FocusBehavior { Pause, BlockInput, AllowInput };

struct Preferences {
  int scale = 3;
  bool aspectCorrection = false, integerScaling = true, linearFilter = false;
  int volume = 100;
  bool mute = false, muteUnfocused = false;
  FocusBehavior focus = FocusBehavior::Pause;
  std::string audioDevice, firmwareDirectory, screenshotDirectory, saveDirectory;
  InputBindings input;
  ConsoleDevices console;
  Hotkeys hotkeys;
  std::vector<LaunchRequest> recent;

  void Remember(LaunchRequest request);
  void Validate() const;
};

class PreferencesFile {
public:
  explicit PreferencesFile(std::filesystem::path path);
  Preferences Load();
  void Save(const Preferences &preferences);
  const std::filesystem::path &Path() const noexcept {
    return path_;
  }
  static std::filesystem::path DefaultPath();
  std::filesystem::path ResolvePath(std::string_view path) const;

private:
  std::filesystem::path path_;
  std::map<std::string, std::string> unknown_;
  std::vector<LaunchRequest> loadedRecent_;
  bool writable_ = false;
};
} // namespace snes::frontend
