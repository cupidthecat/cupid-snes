#include "settings/Preferences.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/AtomicFile.hpp"

#include <SDL3/SDL.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>

using snes::frontend::LaunchRequest;
using snes::frontend::Preferences;
using snes::frontend::PreferencesFile;
namespace fs = std::filesystem;
namespace {
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void Write(const fs::path &path, std::string_view text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
  Check(bool(file), "Preference fixture writes");
}
std::string Read(const fs::path &path) {
  const auto bytes = snes::frontend::ReadFile(path, 1024 * 1024);
  return {bytes.begin(), bytes.end()};
}
void RoundTrip(const fs::path &directory) {
  const auto path = directory / fs::path(u8"\u8a2d\u5b9a.cfg");
  PreferencesFile file(path);
  auto preferences = file.Load();
  Check(preferences.scale == 3 && preferences.volume == 100 && preferences.integerScaling,
        "Missing preferences retain current presentation defaults");
  preferences.volume = 73;
  preferences.scale = 5;
  preferences.mute = true;
  preferences.aspectCorrection = true;
  preferences.input.players[0].buttons[4][0] = {snes::frontend::InputBinding::Kind::Keyboard,
                                                SDL_SCANCODE_C};
  preferences.input.players[7].device = "saved device with spaces and \\\"quotes\\\"";
  preferences.input.players[7].axisThreshold = 12000;
  preferences.input.players[7].buttons[5][3] = {snes::frontend::InputBinding::Kind::AxisNegative,
                                                SDL_GAMEPAD_AXIS_RIGHTY};
  preferences.console.ports[0] = snes::core::ControllerDevice::Multitap;
  preferences.console.expansion = snes::core::Emulator::ExpansionDevice::Satellaview;
  preferences.hotkeys.bindings[size_t(snes::frontend::DesktopCommand::Pause)][0] = {
      SDL_SCANCODE_F8, snes::frontend::KeyChord::Control | snes::frontend::KeyChord::Shift};
  preferences.hotkeys.bindings[size_t(snes::frontend::DesktopCommand::Pause)][3] = {
      SDL_SCANCODE_F8, snes::frontend::KeyChord::Gui};
  preferences.audioDevice = "Headphones \\\"USB\\\"";
  preferences.screenshotDirectory = snes::core::PathText(directory / fs::path(u8"\u753b\u50cf"));
  preferences.firmwareDirectory = "spaces, \\slashes and\nnewlines";
  LaunchRequest request;
  request.kind = LaunchRequest::Kind::Campus92;
  request.paths = {"boot.sfc", "game1.sfc", "game2.sfc", "game3.sfc"};
  request.competitionDip = 0x83;
  preferences.Remember(request);
  const auto normalized = preferences.recent;
  file.Save(preferences);
  PreferencesFile fresh(path);
  const auto loaded = fresh.Load();
  Check(loaded.volume == 73 && loaded.scale == 5 && loaded.mute && loaded.aspectCorrection &&
            loaded.input == preferences.input && loaded.audioDevice == preferences.audioDevice &&
            loaded.firmwareDirectory == preferences.firmwareDirectory &&
            loaded.screenshotDirectory == preferences.screenshotDirectory &&
            loaded.recent == normalized,
        "Preferences preserve Unicode, escapes, keyboard bindings and complete launch requests");
  Check(loaded.console == preferences.console && loaded.hotkeys == preferences.hotkeys,
        "Hardware choices and all hotkey alternatives survive a profile restart");
  Write(path, Read(path) + "future.setting \"retain me\"\n");
  auto extended = fresh.Load();
  extended.volume = 21;
  fresh.Save(extended);
  Check(Read(path).find("future.setting \"retain me\"") != std::string::npos,
        "Older readers preserve unrecognized preferences");
  Write(path, Read(path) + "recent.0.future-path \"private launch metadata\"\n");
  extended = fresh.Load();
  extended.recent.clear();
  fresh.Save(extended);
  Check(Read(path).find("\nrecent.0.") == std::string::npos &&
            Read(path).find("future.setting") != std::string::npos,
        "Clearing history removes unknown per-launch metadata while preserving unrelated settings");
  Write(path, "\xef\xbb\xbf"
              "Cupid-SNES preferences 1\r\naudio.volume \"83\"\r\n");
  Check(fresh.Load().volume == 83, "UTF-8 byte order marks and Windows line endings are accepted");
}
void BadInput(const fs::path &directory) {
  const auto path = directory / "bad.cfg";
  for (const std::string text :
       {"Cupid-SNES preferences 2\n",
        "Cupid-SNES preferences 1\naudio.volume \"201\"\n",
        "Cupid-SNES preferences 1\nvideo.scale \"0\"\n",
        "Cupid-SNES preferences 1\naudio.mute \"2\"\n",
        "Cupid-SNES preferences 1\nkey.a \"9999\"\n",
        "Cupid-SNES preferences 1\nplayer.8.threshold \"0\"\n",
        "Cupid-SNES preferences 1\nplayer.2.device \"\"\n",
        "Cupid-SNES preferences 1\nplayer.1.up.0.type \"6\"\n",
        "Cupid-SNES preferences 1\nplayer.1.up.0.type \"0\"\n",
        "Cupid-SNES preferences 1\nplayer.1.up.0.type \"1\"\nplayer.1.up.0.control \"0\"\n",
        "Cupid-SNES preferences 1\nplayer.1.up.0.type \"2\"\nplayer.1.up.0.control \"999\"\n",
        "Cupid-SNES preferences 1\nplayer.1.up.0.type \"3\"\nplayer.1.up.0.control \"9\"\n",
        "Cupid-SNES preferences 1\nplayer.1.up.0.type \"5\"\nplayer.1.up.0.control \"6\"\n",
        "Cupid-SNES preferences 1\ninput.port1 \"4\"\n",
        "Cupid-SNES preferences 1\ninput.port1 \"2\"\ninput.port2 \"2\"\n",
        "Cupid-SNES preferences 1\ninput.expansion \"2\"\n",
        "Cupid-SNES preferences 1\nhotkey.pause.0.key \"9999\"\n",
        "Cupid-SNES preferences 1\nhotkey.pause.0.modifiers \"16\"\n",
        "Cupid-SNES preferences 1\nhotkey.pause.0.key \"0\"\nhotkey.pause.0.modifiers \"2\"\n",
        "Cupid-SNES preferences 1\nhotkey.open.0.key \"19\"\nhotkey.open.0.modifiers \"0\"\n",
        "Cupid-SNES preferences 1\nrecent.count \"11\"\n",
        "Cupid-SNES preferences 1\nrecent.count \"1\"\nrecent.0.kind \"9\"\n",
        "Cupid-SNES preferences 1\nrecent.count \"1\"\nrecent.0.kind \"1\"\nrecent.0.path0 "
        "\"bios.sfc\"\n",
        "Cupid-SNES preferences 1\naudio.volume \"10\"\naudio.volume \"20\"\n",
        "Cupid-SNES preferences 1\naudio.device \"unfinished"}) {
    Write(path, text);
    PreferencesFile file(path);
    bool rejected = false;
    try {
      file.Load();
    } catch (const std::exception &) {
      rejected = true;
    }
    Check(rejected, "Invalid preferences cannot be loaded");
    rejected = false;
    try {
      file.Save(Preferences{});
    } catch (const std::exception &) {
      rejected = true;
    }
    Check(rejected && Read(path) == text,
          "Rejected preferences cannot be silently replaced with defaults");
  }
}
void LegacyInput(const fs::path &directory) {
  using snes::frontend::InputBinding;
  const auto path = directory / "legacy.cfg";
  Write(path, "Cupid-SNES preferences 1\nkey.up \"0\"\nkey.a \"6\"\n");
  PreferencesFile file(path);
  const auto loaded = file.Load();
  Check(loaded.input.players[0].buttons[0][0] == InputBinding{} &&
            loaded.input.players[0].buttons[4][0] ==
                InputBinding{InputBinding::Kind::Keyboard, SDL_SCANCODE_C} &&
            loaded.input.players[0].buttons[0][1].kind == InputBinding::Kind::GamepadButton &&
            loaded.input.players[7].buttons[4][0] == InputBinding{},
        "Legacy key preferences migrate without duplicating keyboard input across players");
  file.Save(loaded);
  Check(file.Load().input == loaded.input, "Migrated input settings survive a second profile load");
}
void FailedReplacement(const fs::path &directory) {
  const auto folder = directory / "settings";
  const auto path = folder / "preferences.cfg";
  PreferencesFile file(path);
  auto preferences = file.Load();
  file.Save(preferences);
  const auto original = Read(path);
  fs::remove(path);
  fs::create_directory(path);
  preferences.volume = 41;
  bool rejected = false;
  try {
    file.Save(preferences);
  } catch (const std::exception &) {
    rejected = true;
  }
  Check(rejected && fs::is_directory(path), "Failed replacement preserves the destination");
  for (const auto &entry : fs::directory_iterator(folder))
    Check(entry.path() == path, "Failed replacement removes its temporary file");
  fs::remove(path);
  Write(path, original);
  file.Save(preferences);
  Check(PreferencesFile(path).Load().volume == 41,
        "Saving can be retried after the filesystem is fixed");
}
void RecentRequests() {
  Preferences preferences;
  for (unsigned index = 0; index < 12; ++index) {
    LaunchRequest request;
    request.paths[0] = "game" + std::to_string(index) + ".sfc";
    preferences.Remember(request);
  }
  Check(preferences.recent.size() == 10, "Recent games have a bounded history");
  const auto existing = preferences.recent[4];
  preferences.Remember(existing);
  Check(preferences.recent.size() == 10 && preferences.recent.front() == existing,
        "Reopening a game moves the complete request to the front");
  auto alternate = existing;
  alternate.kind = LaunchRequest::Kind::Broadcast;
  alternate.paths[1] = "pack.bs";
  preferences.Remember(alternate);
  Check(preferences.recent.front().kind == LaunchRequest::Kind::Broadcast &&
            preferences.recent[1] == existing,
        "Different cartridge modes and packs retain separate recent requests");
}
void RelativeDirectories(const fs::path &directory) {
  const auto profile = directory / "profiles" / "preferences.cfg";
  PreferencesFile file(profile);
  Check(file.ResolvePath("").empty(), "An empty directory retains its default");
  Check(file.ResolvePath("../screenshots") == directory / "screenshots",
        "Relative directories follow the profile, independent of the working directory");
  const auto absolute = directory / fs::path(u8"\u753b\u50cf");
  Check(file.ResolvePath(snes::core::PathText(absolute)) == absolute,
        "Absolute Unicode directories are preserved");
}
void DeviceOverrides() {
  using D = snes::core::ControllerDevice;
  using E = snes::core::Emulator::ExpansionDevice;
  snes::frontend::ConsoleDevices saved{{D::Multitap, D::SuperScope}, E::Satellaview};
  saved.Validate();
  snes::frontend::DeviceOverrides overrides;
  overrides.ports[0] = D::None;
  overrides.expansion = E::None;
  const auto actual = overrides.Resolve(saved);
  Check(actual.ports == std::array{D::None, D::SuperScope} && actual.expansion == E::None,
        "Explicit command-line overrides preserve unspecified profile devices");
  Check(saved.ports[0] == D::Multitap && saved.expansion == E::Satellaview,
        "Resolving session overrides does not change saved choices");
  overrides.ports[0] = D::Mouse;
  bool rejected = false;
  try {
    overrides.Resolve(saved);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Check(rejected, "Profile and command-line devices are validated as one configuration");
}
} // namespace

int main() {
  const auto directory =
      fs::temp_directory_path() / ("cupid-preferences-" + std::to_string(SDL_GetTicksNS()));
  if (!fs::create_directory(directory))
    return 2;
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{directory};
  try {
    RoundTrip(directory);
    BadInput(directory);
    LegacyInput(directory);
    FailedReplacement(directory);
    RecentRequests();
    RelativeDirectories(directory);
    DeviceOverrides();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
