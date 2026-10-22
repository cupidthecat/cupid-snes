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
  preferences.keyboard.a = SDL_SCANCODE_C;
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
            loaded.keyboard.a == SDL_SCANCODE_C && loaded.audioDevice == preferences.audioDevice &&
            loaded.firmwareDirectory == preferences.firmwareDirectory &&
            loaded.screenshotDirectory == preferences.screenshotDirectory &&
            loaded.recent == normalized,
        "Preferences preserve Unicode, escapes, keyboard bindings and complete launch requests");
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
  Check(Read(path).find("recent.0.") == std::string::npos &&
            Read(path).find("future.setting") != std::string::npos,
        "Clearing history removes unknown per-launch metadata while preserving unrelated settings");
  Write(path, "\xef\xbb\xbf"
              "Cupid-SNES preferences 1\r\naudio.volume \"83\"\r\n");
  Check(fresh.Load().volume == 83, "UTF-8 byte order marks and Windows line endings are accepted");
}
void BadInput(const fs::path &directory) {
  const auto path = directory / "bad.cfg";
  for (const std::string text :
       {"Cupid-SNES preferences 2\n", "Cupid-SNES preferences 1\naudio.volume \"201\"\n",
        "Cupid-SNES preferences 1\nvideo.scale \"0\"\n",
        "Cupid-SNES preferences 1\naudio.mute \"2\"\n",
        "Cupid-SNES preferences 1\nkey.a \"9999\"\n",
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
    FailedReplacement(directory);
    RecentRequests();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
