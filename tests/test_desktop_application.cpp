#include "desktop/DesktopApplication.hpp"
#include "settings/Preferences.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/AtomicFile.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void ConfigureSdl() {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
}
template <class Predicate> void Wait(Predicate predicate, const char *message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!predicate()) {
    Check(std::chrono::steady_clock::now() < deadline, message);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
bool SDLCALL WindowCreated(void *userdata, SDL_Event *event) {
  if (event->type == SDL_EVENT_WINDOW_SHOWN)
    static_cast<std::atomic<SDL_WindowID> *>(userdata)->store(event->window.windowID);
  return true;
}
void Push(SDL_Event event) {
  Check(SDL_PushEvent(&event), "Application input can be queued");
}
void Key(SDL_WindowID window, SDL_Scancode scan, SDL_Keycode key, SDL_Keymod modifiers = 0) {
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.windowID = window;
  event.key.scancode = scan;
  event.key.key = key;
  event.key.mod = modifiers;
  Push(event);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  event.type = SDL_EVENT_KEY_UP;
  event.key.mod = 0;
  Push(event);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
}
void Click(SDL_WindowID window, float x, float y) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.windowID = window;
  event.motion.x = x;
  event.motion.y = y;
  Push(event);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  event = {};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.windowID = window;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.down = true;
  Push(event);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  event.type = SDL_EVENT_MOUSE_BUTTON_UP;
  event.button.down = false;
  Push(event);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
}
std::string Read(const std::filesystem::path &path) {
  const auto bytes = snes::frontend::ReadFile(path, 1024 * 1024);
  return {bytes.begin(), bytes.end()};
}
void Quit() {
  SDL_Event event{};
  event.type = SDL_EVENT_QUIT;
  Push(event);
}

class ErrorCapture final : public std::streambuf {
public:
  ErrorCapture() : original_(std::cerr.rdbuf(this)) {}
  ~ErrorCapture() {
    std::cerr.rdbuf(original_);
  }
  bool Contains(std::string_view text) {
    std::lock_guard lock(mutex_);
    return text_.find(text) != std::string::npos;
  }

private:
  std::streamsize xsputn(const char *text, std::streamsize size) override {
    std::lock_guard lock(mutex_);
    text_.append(text, size);
    return original_->sputn(text, size);
  }
  int_type overflow(int_type value) override {
    if (traits_type::eq_int_type(value, traits_type::eof()))
      return traits_type::not_eof(value);
    const auto character = traits_type::to_char_type(value);
    return xsputn(&character, 1) == 1 ? value : traits_type::eof();
  }
  std::mutex mutex_;
  std::string text_;
  std::streambuf *original_;
};

void RecoverAutosave(const std::filesystem::path &directory) {
  ConfigureSdl();
  std::vector<uint8_t> image(32768);
  image[0x7fd5] = 0x20;
  image[0x7fd6] = 2;
  image[0x7fd7] = 5;
  image[0x7fd8] = 3;
  image[0x7fd9] = 1;
  image[0x7fdc] = image[0x7fdd] = 0xff;
  image[0x7ffd] = 0x80;
  const uint8_t code[] = {0x78, 0xa9, 0x80, 0x8d, 0, 0x21, 0xa9, 0x5a, 0x8f, 0, 0, 0x70, 0xdb};
  std::copy(std::begin(code), std::end(code), image.begin());
  const auto rom = directory / "autosave.sfc", save = directory / "autosave.srm";
  {
    std::ofstream file(rom, std::ios::binary);
    file.write(reinterpret_cast<const char *>(image.data()), image.size());
    Check(bool(file), "Autosave fixture writes");
  }
  snes::frontend::PreferencesFile file(directory / "autosave.cfg");
  auto preferences = file.Load();
  preferences.focus = snes::frontend::FocusBehavior::AllowInput;
  file.Save(preferences);
  Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
  std::atomic<SDL_WindowID> window{0};
  std::atomic<bool> returned{false};
  Check(SDL_AddEventWatch(WindowCreated, &window), SDL_GetError());
  ErrorCapture errors;
  std::exception_ptr failure;
  std::thread driver([&] {
    try {
      Wait([&] { return window.load() || returned.load(); },
           "Autosave application creates a window");
      Wait(
          [&] {
            return returned.load() ||
                   Read(file.Path()).find("recent.count \"1\"") != std::string::npos;
          },
          "Autosave cartridge reaches the running application");
      Check(!returned && std::filesystem::create_directory(save),
            "The test blocks its own live save destination");
      Wait([&] { return returned.load() || errors.Contains("Cannot replace file:"); },
           "The application attempts its periodic cartridge save");
      Check(!returned, "A periodic save error keeps the live application available for retry");
      std::filesystem::remove(save);
      Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
      Quit();
    } catch (...) {
      failure = std::current_exception();
      std::error_code ignored;
      if (std::filesystem::is_directory(save, ignored))
        std::filesystem::remove(save, ignored);
      if (!returned) {
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
      }
    }
  });
  snes::frontend::LaunchRequest request;
  request.paths[0] = snes::core::PathText(rom);
  const auto result = snes::frontend::RunDesktop({}, {}, request, file.Path());
  returned = true;
  driver.join();
  if (failure)
    std::rethrow_exception(failure);
  Check(result == 0 && Read(save).size() == 8192 && uint8_t(Read(save)[0]) == 0x5a,
        "After repairing the destination, quit saves the original guest battery byte normally");
}
} // namespace

int main() {
  ConfigureSdl();
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("cupid-desktop-app-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  if (!std::filesystem::create_directory(directory))
    return 2;
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
      SDL_Quit();
    }
  } cleanup{directory};
  try {
    snes::frontend::PreferencesFile file(directory / "preferences.cfg");
    auto preferences = file.Load();
    preferences.focus = snes::frontend::FocusBehavior::AllowInput;
    file.Save(preferences);
    const auto backup = directory / "original.cfg";
    Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
    std::atomic<SDL_WindowID> window{0};
    std::atomic<bool> returned{false};
    Check(SDL_AddEventWatch(WindowCreated, &window), SDL_GetError());
    std::exception_ptr failure;
    std::thread driver([&] {
      try {
        Wait([&] { return window.load() || returned.load(); }, "The application creates a window");
        Check(!returned, "The application stays open before input");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        Key(window, SDL_SCANCODE_COMMA, SDLK_COMMA, SDL_KMOD_CTRL);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Click(window, 94, 380);
        Click(window, 186, 624);
        Wait([&] { return Read(file.Path()).find("audio.mute \"1\"") != std::string::npos; },
             "Apply saves preferences through the running application");
        const auto saved = Read(file.Path());
        Click(window, 94, 380);
        Click(window, 258, 624);
        Check(Read(file.Path()) == saved, "Cancel preserves the last successful Apply");
        std::filesystem::rename(file.Path(), backup);
        std::filesystem::create_directory(file.Path());
        Quit();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        Check(!returned, "A failed quit-time preferences save keeps the application open");
        std::filesystem::remove(file.Path());
        std::filesystem::rename(backup, file.Path());
        Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
        Quit();
      } catch (...) {
        failure = std::current_exception();
        std::error_code ignored;
        if (std::filesystem::exists(backup, ignored)) {
          std::filesystem::remove(file.Path(), ignored);
          std::filesystem::rename(backup, file.Path(), ignored);
        }
        if (!returned) {
          SDL_Event event{};
          event.type = SDL_EVENT_QUIT;
          SDL_PushEvent(&event);
        }
      }
    });
    const auto result = snes::frontend::RunDesktop({}, {}, {}, file.Path());
    returned = true;
    driver.join();
    if (failure)
      std::rethrow_exception(failure);
    Check(result == 0, "The application exits normally after the destination is fixed");
    RecoverAutosave(directory);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
