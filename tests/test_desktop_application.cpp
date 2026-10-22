#include "SdlInputProvider.hpp"
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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
void ReplaceText(SDL_WindowID window, const char *text) {
  Key(window, SDL_SCANCODE_A, SDLK_A, SDL_KMOD_CTRL);
  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  event.text.windowID = window;
  event.text.text = text;
  Push(event);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
}
std::string Read(const std::filesystem::path &path) {
  const auto bytes = snes::frontend::ReadFile(path, 1024 * 1024);
  return {bytes.begin(), bytes.end()};
}
struct EditorPicture {
  SDL_WindowID window;
  bool valid = false, open = false, error = false, textInput = false;
};
EditorPicture ReadEditorPicture(SDL_WindowID window) {
  EditorPicture result{window};
  Check(SDL_RunOnMainThread(
            [](void *userdata) {
              auto &picture = *static_cast<EditorPicture *>(userdata);
              auto *window = SDL_GetWindowFromID(picture.window);
              auto *renderer = window ? SDL_GetRenderer(window) : nullptr;
              std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
                  renderer ? SDL_RenderReadPixels(renderer, nullptr) : nullptr, SDL_DestroySurface);
              if (!surface || surface->w < 700 || surface->h < 640)
                return;
              picture.valid = true;
              picture.textInput = SDL_TextInputActive(window);
              Uint8 r = 0, g = 0, b = 0, a = 0;
              if (SDL_ReadSurfacePixel(surface.get(), 100, 200, &r, &g, &b, &a))
                picture.open = unsigned(r) + g + b > 40;
              for (int y = 500; y < 610 && !picture.error; ++y)
                for (int x = 80; x < 680; ++x)
                  if (SDL_ReadSurfacePixel(surface.get(), x, y, &r, &g, &b, &a) && r > 180 &&
                      g > 60 && g < 180 && b < 180 && r > unsigned(g) + 60 &&
                      r > unsigned(b) + 60) {
                    picture.error = true;
                    break;
                  }
            },
            &result, true),
        "The test reads its software renderer on the application's main thread");
  return result;
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

void SaveDirectoryApplication(const std::filesystem::path &directory) {
  ConfigureSdl();
  const auto original = Read(directory / "autosave.srm");
  const auto selected = directory / "save-files", final = directory / "save-final";
  std::filesystem::create_directory(selected);
  {
    const std::string bytes(8192, char(0xc3));
    std::ofstream output(selected / "autosave.srm", std::ios::binary);
    output.write(bytes.data(), bytes.size());
    Check(bool(output), "The GUI save destination starts with different battery data");
    std::ofstream blocked(directory / "blocked-save-folder", std::ios::binary);
    blocked << 'x';
    Check(bool(blocked), "The invalid GUI save folder is a file");
  }
  snes::frontend::PreferencesFile file(directory / "save-paths.cfg");
  auto preferences = file.Load();
  preferences.focus = snes::frontend::FocusBehavior::AllowInput;
  file.Save(preferences);
  const auto backup = directory / "save-paths-backup.cfg";
  Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
  std::atomic<SDL_WindowID> window{0};
  std::atomic<bool> returned{false};
  Check(SDL_AddEventWatch(WindowCreated, &window), SDL_GetError());
  std::exception_ptr failure;
  std::thread driver([&] {
    try {
      Wait([&] { return window.load() || returned.load(); },
           "The save-folder application creates a window");
      Wait([&] { return Read(file.Path()).find("recent.count \"1\"") != std::string::npos; },
           "The save-folder cartridge reaches the running application");
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      Key(window, SDL_SCANCODE_COMMA, SDLK_COMMA, SDL_KMOD_CTRL);
      Click(window, 229, 128);
      Click(window, 224, 320);
      ReplaceText(window, "save-files");
      Click(window, 186, 624);
      Wait([&] { return Read(file.Path()).find("path.saves \"save-files\"") != std::string::npos; },
           "The running Paths editor saves the selected relative battery directory");
      const auto applied = Read(file.Path());
      Click(window, 224, 320);
      ReplaceText(window, "blocked-save-folder");
      Click(window, 186, 624);
      Wait([&] { return ReadEditorPicture(window).error; },
           "An invalid save folder displays its Apply error");
      Check(Read(file.Path()) == applied, "An invalid save folder preserves the saved preferences");
      Click(window, 224, 320);
      ReplaceText(window, "save-final");
      Click(window, 186, 624);
      Wait([&] { return Read(file.Path()).find("path.saves \"save-final\"") != std::string::npos; },
           "The same editor retries with a valid save directory");
      Wait([&] { return !ReadEditorPicture(window).error; },
           "A successful Apply clears the prior error");
      Click(window, 224, 320);
      ReplaceText(window, "save-rejected");
      std::filesystem::rename(file.Path(), backup);
      std::filesystem::create_directory(file.Path());
      Click(window, 186, 624);
      Wait([&] { return ReadEditorPicture(window).error; },
           "A failed preference write displays its Apply error before repair");
      Check(!returned, "A failed preference write keeps the save-folder editor and game alive");
      std::filesystem::remove(file.Path());
      std::filesystem::rename(backup, file.Path());
      Click(window, 258, 624);
      unsigned closed = 0;
      Wait(
          [&] {
            const auto picture = ReadEditorPicture(window);
            closed = picture.valid && !picture.open && !picture.textInput ? closed + 1 : 0;
            return closed >= 4;
          },
          "Cancel closes the rendered path editor and releases text input");
      Key(window, SDL_SCANCODE_Q, SDLK_Q, SDL_KMOD_CTRL);
      Wait([&] { return returned.load(); }, "Ctrl+Q quits after closing the path editor");
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
  snes::frontend::LaunchRequest request;
  request.paths[0] = snes::core::PathText(directory / "autosave.sfc");
  const auto result = snes::frontend::RunDesktop({}, {}, request, file.Path());
  returned = true;
  driver.join();
  if (failure)
    std::rethrow_exception(failure);
  Check(result == 0 && Read(selected / "autosave.srm") == std::string(8192, char(0xc3)) &&
            Read(final / "autosave.srm") == original &&
            Read(directory / "autosave.srm") == original &&
            !std::filesystem::exists(directory / "save-rejected/autosave.srm") &&
            Read(file.Path()).find("path.saves \"save-final\"") != std::string::npos,
        "Apply and retry retain the live battery bytes, while failed Apply leaves the last saved "
        "folder active");
  std::puts("Save directory application checks passed");
}

struct DrivenGamepad {
  SDL_Joystick *joystick = nullptr;
  std::atomic<Uint32> buttons{0};
  std::atomic<Uint32> sampledButtons{0};
  std::atomic<SDL_WindowID> window{0};
  std::atomic<bool> focused{false}, paused{false};
  std::atomic<int> visibility{-1};
  DrivenGamepad() {
    SDL_VirtualJoystickDesc description;
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    description.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    description.name = "Desktop shortcut test gamepad";
    description.userdata = this;
    description.Update = [](void *userdata) {
      auto &self = *static_cast<DrivenGamepad *>(userdata);
      if (!self.joystick)
        return;
      const auto down = self.buttons.load();
      for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button)
        SDL_SetJoystickVirtualButton(self.joystick, button, (down & (1u << button)) != 0);
      self.sampledButtons = down;
      if (self.window.load()) {
        const auto visibility = self.visibility.exchange(-1);
        if (auto *window = SDL_GetWindowFromID(self.window)) {
          if (visibility == 0)
            SDL_HideWindow(window);
          else if (visibility == 1)
            SDL_ShowWindow(window);
        }
      }
      if (auto *window = SDL_GetKeyboardFocus()) {
        self.window = SDL_GetWindowID(window);
        self.focused =
            (SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
        self.paused =
            std::string_view(SDL_GetWindowTitle(window)).find("(Paused)") != std::string_view::npos;
      } else
        self.focused = false;
    };
    const auto id = SDL_AttachVirtualJoystick(&description);
    Check(id != 0, "Attach application shortcut gamepad");
    joystick = SDL_OpenJoystick(id);
    Check(joystick != nullptr, "Open application shortcut gamepad");
  }
  void Hold(Uint32 value) {
    buttons = value;
    Wait([&] { return sampledButtons.load() == value; },
         "The application samples its virtual controller state");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  void Press(Uint32 value) {
    const auto previous = buttons.load();
    Hold(previous | value);
    Hold(previous);
  }
};

unsigned BackdropColor(const std::string &picture) {
  const auto number = [&](size_t offset, size_t bytes) {
    Check(offset + bytes <= picture.size(), "The captured bitmap has its expected header");
    unsigned value = 0;
    for (size_t byte = 0; byte < bytes; ++byte)
      value |= unsigned(uint8_t(picture[offset + byte])) << (8 * byte);
    return value;
  };
  const auto width = number(18, 4), rawHeight = number(22, 4), depth = number(28, 2);
  const auto height = int32_t(rawHeight) < 0 ? unsigned(-int32_t(rawHeight)) : rawHeight;
  Check(width == 256 && height == 224 && (depth == 24 || depth == 32),
        "The shortcut screenshot keeps the generated picture dimensions");
  const auto stride = ((width * depth + 31) / 32) * 4;
  const auto center = number(10, 4) + height / 2 * stride + width / 2 * (depth / 8);
  const auto b = number(center, 1), g = number(center + 1, 1), r = number(center + 2, 1);
  return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
}

void GamepadApplication(const std::filesystem::path &directory, bool idle) {
  using namespace snes::frontend;
  ConfigureSdl();
  Check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), SDL_GetError());
  DrivenGamepad pad;
  const auto mask = [](SDL_GamepadButton button) { return 1u << unsigned(button); };
  const auto pause = mask(SDL_GAMEPAD_BUTTON_GUIDE) | mask(SDL_GAMEPAD_BUTTON_START);
  const auto quit = mask(SDL_GAMEPAD_BUTTON_GUIDE) | mask(SDL_GAMEPAD_BUTTON_BACK);
  const auto step = mask(SDL_GAMEPAD_BUTTON_SOUTH), shot = mask(SDL_GAMEPAD_BUTTON_WEST);
  const auto editor = mask(SDL_GAMEPAD_BUTTON_NORTH);
  PreferencesFile file(directory / (idle ? "idle-gamepad.cfg" : "gamepad.cfg"));
  auto preferences = file.Load();
  preferences.screenshotDirectory = "gamepad-pictures";
  preferences.focus = FocusBehavior::AllowInput;
  std::string device;
  for (const auto &connected : SdlInputProvider::ConnectedGamepads())
    if (connected.name == "Desktop shortcut test gamepad")
      device = connected.key;
  Check(!device.empty(), "Application shortcut gamepad has a persistent selection");
  const auto shortcut = [&](DesktopCommand command,
                            std::initializer_list<SDL_GamepadButton> buttons) {
    auto &chord = preferences.hotkeys.gamepads[size_t(command)][3];
    chord.device = device;
    size_t index = 0;
    for (const auto button : buttons)
      chord.controls[index++] = {InputBinding::Kind::GamepadButton, button};
  };
  shortcut(DesktopCommand::Pause, {SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START});
  shortcut(DesktopCommand::Quit, {SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_BACK});
  shortcut(DesktopCommand::FrameAdvance, {SDL_GAMEPAD_BUTTON_SOUTH});
  shortcut(DesktopCommand::Screenshot, {SDL_GAMEPAD_BUTTON_WEST});
  shortcut(DesktopCommand::Preferences, {SDL_GAMEPAD_BUTTON_NORTH});
  file.Save(preferences);
  std::optional<LaunchRequest> request;
  if (!idle) {
    std::vector<uint8_t> image(32768);
    // Increment the backdrop color once per vertical blank.
    const uint8_t code[] = {0x78, 0xd8, 0xa9, 0x80, 0x8d, 0,    0x21, 0x64, 0,    0x64, 1,    0xa9,
                            0x0f, 0x8d, 0,    0x21, 0xad, 0x12, 0x42, 0x10, 0xfb, 0xe6, 0,    0xd0,
                            2,    0xe6, 1,    0x9c, 0x21, 0x21, 0xa5, 0,    0x8d, 0x22, 0x21, 0xa5,
                            1,    0x8d, 0x22, 0x21, 0xad, 0x12, 0x42, 0x30, 0xfb, 0x80, 0xe1};
    std::copy(std::begin(code), std::end(code), image.begin());
    image[0x7fd5] = 0x20;
    image[0x7fd7] = 5;
    image[0x7fdc] = image[0x7fdd] = 0xff;
    image[0x7ffd] = 0x80;
    const auto rom = directory / "gamepad.sfc";
    std::ofstream output(rom, std::ios::binary);
    output.write(reinterpret_cast<const char *>(image.data()), image.size());
    Check(bool(output), "Write the application shortcut cartridge");
    request.emplace();
    request->paths[0] = snes::core::PathText(rom);
  }
  std::atomic<bool> returned{false};
  std::exception_ptr failure;
  std::thread driver([&] {
    try {
      Wait([&] { return pad.focused.load() || returned.load(); },
           "The owned native application receives focus");
      Check(!returned, "The application stays open before gamepad input");
      if (!idle) {
        Wait([&] { return Read(file.Path()).find("recent.count \"1\"") != std::string::npos; },
             "The shortcut cartridge reaches the running application");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        pad.Hold(pause);
        Wait([&] { return pad.paused.load() || returned.load(); },
             "A gamepad chord pauses playback");
        Check(!returned, "Pausing keeps the application open");
        pad.Hold(0);
        const auto pictures = directory / "gamepad-pictures";
        const auto capture = [&] {
          std::vector<std::filesystem::path> before;
          if (std::filesystem::exists(pictures))
            for (const auto &entry : std::filesystem::directory_iterator(pictures))
              before.push_back(entry.path());
          pad.Press(shot);
          std::filesystem::path path;
          Wait(
              [&] {
                if (!std::filesystem::exists(pictures))
                  return false;
                for (const auto &entry : std::filesystem::directory_iterator(pictures))
                  if (std::find(before.begin(), before.end(), entry.path()) == before.end()) {
                    path = entry.path();
                    return std::filesystem::file_size(path) >= 256 * 224 * 4;
                  }
                return false;
              },
              "A gamepad shortcut saves the paused native picture");
          return Read(path);
        };
        const auto first = capture();
        pad.Hold(pause);
        Wait([&] { return !pad.paused.load() || returned.load(); },
             "A rearmed gamepad chord resumes playback");
        pad.Hold(0);
        pad.Press(pause);
        Wait([&] { return pad.paused.load() || returned.load(); }, "A gamepad chord pauses again");
        const auto second = capture();
        Check(first != second, "Resuming advances the guest backdrop counter");
        pad.Hold(step);
        const auto advanced = capture();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        Check(((BackdropColor(advanced) - BackdropColor(second)) & 0x7fff) == 1 &&
                  capture() == advanced,
              "Holding frame advance runs one field and leaves subsequent pictures unchanged");
        pad.Hold(0);
        pad.Press(step);
        Check(((BackdropColor(capture()) - BackdropColor(advanced)) & 0x7fff) == 1,
              "Releasing and pressing frame advance runs another field");
        pad.visibility = 0;
        Wait([&] { return !pad.focused.load() || returned.load(); },
             "The test hides its own application window");
        pad.Hold(quit);
        Check(!returned, "Background gameplay input does not enable hidden-window shortcuts");
        pad.visibility = 1;
        Wait([&] { return pad.focused.load() || returned.load(); },
             "The owned application window regains focus");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Check(!returned, "A shortcut held while unfocused stays disarmed when focus returns");
        pad.Hold(0);
        pad.Press(pause);
        Wait([&] { return !pad.paused.load() || returned.load(); },
             "Gamepad resume works after frame advance");
        pad.Press(editor);
        Wait([&] { return pad.paused.load() || returned.load(); },
             "A gamepad shortcut opens preferences and pauses");
        pad.Hold(quit);
        Check(!returned, "Preferences capture the gamepad quit shortcut");
        Key(pad.window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
        Wait([&] { return !pad.paused.load() || returned.load(); },
             "Closing preferences restores running playback");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Check(!returned, "A shortcut held during preferences stays disarmed after closing them");
        pad.Hold(0);
      }
      pad.Hold(quit);
      Wait([&] { return returned.load(); }, "The gamepad quit chord closes the application");
    } catch (...) {
      failure = std::current_exception();
      if (!returned) {
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
      }
    }
  });
  const auto result = RunDesktop({}, {}, request, file.Path());
  returned = true;
  driver.join();
  if (failure)
    std::rethrow_exception(failure);
  Check(result == 0, "The application exits normally after gamepad shortcuts");
  std::printf("Gamepad shortcuts passed in the %s application\n", idle ? "idle" : "running");
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
    SaveDirectoryApplication(directory);
    if (SDL_getenv("SNES_TEST_WINDOW")) {
      GamepadApplication(directory, false);
      GamepadApplication(directory, true);
    }
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
