#include "desktop/DesktopApplication.hpp"
#include "settings/Preferences.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/AtomicFile.hpp"
#include "storage/StateStorage.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
using namespace snes::frontend;
namespace {
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class Predicate> void Wait(Predicate predicate, const char *message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!predicate()) {
    Check(std::chrono::steady_clock::now() < deadline, message);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
void Push(SDL_Event event) {
  Check(SDL_PushEvent(&event), "Application event can be queued");
}
void Key(SDL_WindowID window, SDL_Scancode scan, SDL_Keycode key, SDL_Keymod modifiers = 0) {
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.windowID = window;
  event.key.scancode = scan;
  event.key.key = key;
  event.key.mod = modifiers;
  Push(event);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  event.type = SDL_EVENT_KEY_UP;
  event.key.mod = 0;
  Push(event);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
}
void Quit() {
  SDL_Event event{};
  event.type = SDL_EVENT_QUIT;
  Push(event);
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
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
}
Preferences Profile(const fs::path &path) {
  struct Request {
    const fs::path &path;
    Preferences preferences;
    std::exception_ptr failure;
  } request{path, {}, {}};
  Check(SDL_RunOnMainThread(
            [](void *userdata) {
              auto &request = *static_cast<Request *>(userdata);
              try {
                request.preferences = PreferencesFile(request.path).Load();
              } catch (...) {
                request.failure = std::current_exception();
              }
            },
            &request, true),
        "Profile observer runs between application writes");
  if (request.failure)
    std::rethrow_exception(request.failure);
  return request.preferences;
}
bool SDLCALL WindowCreated(void *userdata, SDL_Event *event) {
  if (event->type == SDL_EVENT_WINDOW_SHOWN)
    static_cast<std::atomic<SDL_WindowID> *>(userdata)->store(event->window.windowID);
  return true;
}
struct Picture {
  SDL_WindowID window;
  std::string path, title;
  uint32_t center = 0;
  int width = 0, height = 0;
  bool valid = false;
};
Picture ReadPicture(SDL_WindowID window, const fs::path &path = {}) {
  Picture result{window, snes::core::PathText(path), {}};
  Check(SDL_RunOnMainThread(
            [](void *userdata) {
              auto &picture = *static_cast<Picture *>(userdata);
              auto *window = SDL_GetWindowFromID(picture.window);
              auto *renderer = window ? SDL_GetRenderer(window) : nullptr;
              auto *surface = renderer ? SDL_RenderReadPixels(renderer, nullptr) : nullptr;
              if (!surface)
                return;
              picture.title = SDL_GetWindowTitle(window);
              picture.width = surface->w;
              picture.height = surface->h;
              Uint8 r = 0, g = 0, b = 0, a = 0;
              picture.valid =
                  SDL_ReadSurfacePixel(surface, surface->w / 2, surface->h / 2, &r, &g, &b, &a);
              picture.center = 0xff000000u | uint32_t(b) << 16 | uint32_t(g) << 8 | r;
              if (!picture.path.empty())
                picture.valid &= SDL_SaveBMP(surface, picture.path.c_str());
              SDL_DestroySurface(surface);
            },
            &result, true),
        "Rendered picture can be read on the main thread");
  Check(result.valid, "Application has a rendered picture");
  return result;
}
class ErrorCapture : public std::streambuf {
public:
  ErrorCapture() : original_(std::cerr.rdbuf(this)) {}
  ~ErrorCapture() {
    std::cerr.rdbuf(original_);
  }
  bool Contains(const std::string &text) {
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
    const char c = traits_type::to_char_type(value);
    return xsputn(&c, 1) == 1 ? value : traits_type::eof();
  }
  std::mutex mutex_;
  std::string text_;
  std::streambuf *original_;
};
std::vector<uint8_t> Image() {
  std::vector<uint8_t> image(32768);
  std::fill_n(image.begin() + 0x7fc0, 21, ' ');
  image[0x7fd5] = 0x20;
  image[0x7fd7] = 5;
  image[0x7fd9] = 1;
  image[0x7fdc] = image[0x7fdd] = 0xff;
  image[0x7ffd] = 0x80;
  image[0x7feb] = image[0x7ffb] = 0x81;
  const uint8_t boot[] = {0x78, 0x64, 0x00, 0xa9, 0x0f, 0x8d, 0x00, 0x21,
                          0xa9, 0x80, 0x8d, 0x00, 0x42, 0xcb, 0x80, 0xfd};
  const uint8_t nmi[] = {0x48, 0xe6, 0x00, 0x9c, 0x21, 0x21, 0xa5, 0x00,
                         0x8d, 0x22, 0x21, 0x8d, 0x22, 0x21, 0x68, 0x40};
  std::copy(std::begin(boot), std::end(boot), image.begin());
  std::copy(std::begin(nmi), std::end(nmi), image.begin() + 0x100);
  return image;
}
uint64_t FrameCount(const StateFile &state, const fs::path &rom) {
  snes::core::Emulator machine;
  std::string error;
  Check(machine.LoadCartridgeFromFile(snes::core::PathText(rom), &error),
        "Independent machine opens the generated cartridge");
  machine.RestoreState(state.machine);
  return machine.CurrentFrame();
}
void Workflow(const fs::path &directory, const fs::path &pictures) {
  const auto rom = directory / "state-game.sfc";
  {
    const auto bytes = Image();
    std::ofstream out(rom, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    Check(bool(out), "State workflow cartridge writes");
  }
  const auto profile = directory / "preferences.cfg";
  PreferencesFile file(profile);
  auto preferences = file.Load();
  preferences.focus = FocusBehavior::AllowInput;
  preferences.stateDirectory = snes::core::PathText(directory / fs::path(u8"\u72b6\u614b"));
  file.Save(preferences);
  LaunchRequest request;
  request.paths[0] = snes::core::PathText(rom);
  StateStorage states(request, snes::core::FilePath(preferences.stateDirectory));
  if (!SDL_getenv("SNES_TEST_WINDOW"))
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
  std::atomic<SDL_WindowID> window{0};
  std::atomic<bool> returned{false};
  Check(SDL_AddEventWatch(WindowCreated, &window), SDL_GetError());
  ErrorCapture errors;
  std::exception_ptr failure;
  std::thread driver([&] {
    try {
      Wait([&] { return window.load() || returned.load(); },
           "State application creates its window");
      Wait([&] { return returned.load() || Profile(profile).recent.size() == 1; },
           "State cartridge opens in the application");
      Check(!returned, "State application stays open");
      Wait(
          [&] {
            const auto color = ReadPicture(window).center;
            return color != 0xff000000 && color != 0xff1e1816;
          },
          "Generated cartridge presents a completed picture before pausing");
      Key(window, SDL_SCANCODE_P, SDLK_P);
      Wait([&] { return ReadPicture(window).title.find("Paused") != std::string::npos; },
           "State workflow pauses playback");
      const auto select = [&](unsigned slot) {
        unsigned attempts = 0;
        while (Profile(profile).stateSlot != slot) {
          Check(++attempts <= 12 && !returned, "Quick-slot shortcut remains available");
          Key(window, SDL_SCANCODE_F7, SDLK_F7);
        }
      };
      const auto save = [&](unsigned slot) {
        select(slot);
        Key(window, SDL_SCANCODE_F2, SDLK_F2);
        const auto path = states.Path(StateStorage::Quick(slot));
        Wait([&] { return returned.load() || fs::is_regular_file(path); },
             "Quick state reaches disk");
        Check(!returned, "Quick save retains the running application");
        return StateFile::Read(path);
      };
      const auto first = save(0);
      Check(!first.preview.pixels.empty(), "Quick state stores the last completed picture");
      Key(window, SDL_SCANCODE_N, SDLK_N);
      const auto second = save(1);
      Check(first.machine != second.machine && first.preview.pixels != second.preview.pixels,
            "Frame advance changes both whole-machine state and completed picture");
      select(0);
      Key(window, SDL_SCANCODE_F4, SDLK_F4);
      const auto expectedPixel = first.preview.pixels[120 * 256 + 128];
      Wait([&] { return ReadPicture(window).center == expectedPixel; },
           "Paused restore presents the saved picture without stepping");
      Check(save(2).machine == first.machine, "Quick load restores exact execution while paused");
      Key(window, SDL_SCANCODE_Z, SDLK_Z, SDL_KMOD_CTRL);
      Check(save(3).machine == second.machine, "Undo hotkey restores the exact pre-load machine");
      Key(window, SDL_SCANCODE_Y, SDLK_Y, SDL_KMOD_CTRL);
      Check(save(4).machine == first.machine, "Redo hotkey restores the loaded machine");
      auto corrupt = ReadFile(states.Path(StateStorage::Quick(0)), 140 * 1024 * 1024);
      corrupt.back() ^= 1;
      WriteFileAtomically(states.Path(StateStorage::Quick(0)), corrupt);
      select(0);
      Key(window, SDL_SCANCODE_F4, SDLK_F4);
      Wait([&] { return errors.Contains("checksum"); },
           "Malformed quick state reports a visible load error");
      if (!pictures.empty())
        ReadPicture(window, pictures / "state-load-error.bmp");
      Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
      Check(save(5).machine == first.machine, "Rejected state file preserves paused execution");
      Key(window, SDL_SCANCODE_Z, SDLK_Z, SDL_KMOD_CTRL);
      Check(save(6).machine == second.machine, "Rejected load preserves the previous undo");
      Key(window, SDL_SCANCODE_Y, SDLK_Y, SDL_KMOD_CTRL);
      Check(save(7).machine == first.machine, "Undo and redo remain available after a failed load");
      Key(window, SDL_SCANCODE_F3, SDLK_F3);
      const auto manager = ReadPicture(window);
      const float left = float(manager.width - 700) / 2;
      const float top = float(manager.height - 570) / 2;
      const auto named = states.Path({false, "New state"});
      Click(window, left + 42, top + 393);
      Wait([&] { return fs::is_regular_file(named); }, "Manager Add writes a named state");
      Check(StateFile::Read(named).machine == first.machine,
            "Manager capture preserves the exact paused checkpoint");
      if (!pictures.empty())
        ReadPicture(window, pictures / "state-manager.bmp");
      const auto originalNamed = ReadFile(named, 140 * 1024 * 1024);
      Click(window, left + 47, top + 425);
      Click(window, left + 78, top + 329);
      Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
      Check(ReadFile(named, 140 * 1024 * 1024) == originalNamed,
            "Cancelling manager replacement preserves the original file");
      Click(window, left + 78, top + 329);
      Key(window, SDL_SCANCODE_RETURN, SDLK_RETURN);
      Wait([&] { return StateFile::Read(named).machine == second.machine; },
           "Manager undo and confirmed save retain the exact pre-load machine");
      const auto namedBytes = ReadFile(named, 140 * 1024 * 1024);
      Click(window, left + 127, top + 425);
      Click(window, left + 28, top + 329);
      Click(window, left + 47, top + 425);
      Click(window, left + 160, top + 361);
      Key(window, SDL_SCANCODE_A, SDLK_A, SDL_KMOD_CTRL);
      SDL_Event text{};
      text.type = SDL_EVENT_TEXT_INPUT;
      text.text.windowID = window;
      text.text.text = "Renamed state";
      Push(text);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      Click(window, left + 114, top + 393);
      const auto renamed = states.Path({false, "Renamed state"});
      Wait([&] { return fs::exists(renamed) && !fs::exists(named); },
           "Manager Rename preserves the state under its new name");
      Check(ReadFile(renamed, 140 * 1024 * 1024) == namedBytes,
            "Manager rename does not rewrite the state payload");
      Click(window, left + 137, top + 329);
      Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
      Check(fs::exists(renamed), "Cancelling removal preserves the selected state");
      Click(window, left + 137, top + 329);
      Key(window, SDL_SCANCODE_RETURN, SDLK_RETURN);
      Wait([&] { return !fs::exists(renamed); },
           "Confirmed manager removal deletes the selected file");
      Click(window, left + 160, top + 44);
      Key(window, SDL_SCANCODE_DOWN, SDLK_DOWN);
      Key(window, SDL_SCANCODE_RETURN, SDLK_RETURN);
      if (!pictures.empty())
        ReadPicture(window, pictures / "state-manager-quick.bmp");
      Click(window, left + 78, top + 329);
      Key(window, SDL_SCANCODE_RETURN, SDLK_RETURN);
      Wait(
          [&] {
            try {
              return StateFile::Read(states.Path(StateStorage::Quick(0))).machine == first.machine;
            } catch (const std::exception &) {
              return false;
            }
          },
          "Manager quick category repairs only the selected quick slot");
      Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
      Check(save(8).machine == first.machine,
            "Opening and closing the manager preserves paused execution");
      Key(window, SDL_SCANCODE_P, SDLK_P);
      Wait([&] { return ReadPicture(window).title.find("Paused") == std::string::npos; },
           "Playback resumes after paused state operations");
      Key(window, SDL_SCANCODE_F3, SDLK_F3);
      Wait([&] { return ReadPicture(window).title.find("Paused") != std::string::npos; },
           "Opening the manager suspends running playback");
      Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
      Wait([&] { return ReadPicture(window).title.find("Paused") == std::string::npos; },
           "Closing the manager resumes the prior running mode");
      select(0);
      Key(window, SDL_SCANCODE_F4, SDLK_F4);
      Check(ReadPicture(window).title.find("Paused") == std::string::npos,
            "Quick load retains running playback");
      Wait([&] { return ReadPicture(window).center != expectedPixel; },
           "Running restore continues to present new fields");
      Key(window, SDL_SCANCODE_P, SDLK_P);
      const auto continued = save(9);
      Check(FrameCount(continued, rom) > FrameCount(first, rom) + 1,
            "Running restore resets pacing and continues the restored machine timeline");
      Quit();
    } catch (...) {
      failure = std::current_exception();
      if (!returned)
        Quit();
    }
  });
  const auto result = RunDesktop({}, {}, request, profile);
  returned = true;
  driver.join();
  if (failure)
    std::rethrow_exception(failure);
  Check(result == 0, "State application exits normally");
  Check(PreferencesFile(profile).Load().stateSlot == 9,
        "Quick-slot selection survives application exit");
}
} // namespace

int main(int argc, char **argv) {
  const auto directory =
      fs::temp_directory_path() /
      ("cupid-state-app-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  if (!fs::create_directory(directory))
    return 2;
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
      SDL_Quit();
    }
  } cleanup{directory};
  try {
    const auto pictures = argc == 2 ? snes::core::FilePath(argv[1]) : fs::path{};
    if (!pictures.empty())
      fs::create_directories(pictures);
    Workflow(directory, pictures);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
