#include "SdlVideoOutput.hpp"
#include "desktop/DesktopMenu.hpp"
#include "desktop/DesktopUi.hpp"
#include "desktop/FileDialogs.hpp"
#include "snes/core/FilePath.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using snes::frontend::DesktopCommand;
using snes::frontend::DesktopMenu;
using snes::frontend::SdlVideoOutput;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Shortcuts() {
  DesktopMenu menu;
  SDL_KeyboardEvent key{};
  key.type = SDL_EVENT_KEY_DOWN;
  key.scancode = SDL_SCANCODE_P;
  Check(menu.KeyCommand(key) == DesktopCommand::Pause, "P pauses the game");
  key.repeat = true;
  Check(!menu.KeyCommand(key), "Held shortcuts do not repeatedly toggle a command");
  key.repeat = false;
  key.mod = SDL_KMOD_ALT;
  Check(!menu.KeyCommand(key), "Alt+P does not pause the game");
  key.scancode = SDL_SCANCODE_RETURN;
  Check(menu.KeyCommand(key) == DesktopCommand::Fullscreen, "Alt+Enter toggles fullscreen");
  key.mod = SDL_KMOD_CTRL;
  key.scancode = SDL_SCANCODE_O;
  Check(menu.KeyCommand(key) == DesktopCommand::Open, "Ctrl+O opens a game");
  key.scancode = SDL_SCANCODE_N;
  Check(!menu.KeyCommand(key), "Ctrl+N does not execute an unmodified shortcut");
  key.mod = SDL_KMOD_NONE;
  Check(menu.KeyCommand(key) == DesktopCommand::FrameAdvance, "N advances one frame");
  key.type = SDL_EVENT_KEY_UP;
  Check(!menu.KeyCommand(key), "Releasing a key does not execute a command");
}
void CustomShortcuts() {
  using snes::frontend::Hotkeys;
  using snes::frontend::KeyChord;
  Hotkeys hotkeys;
  auto &pause = hotkeys.bindings[size_t(DesktopCommand::Pause)];
  pause = {{{SDL_SCANCODE_F8, KeyChord::Control | KeyChord::Shift},
            {SDL_SCANCODE_F8, KeyChord::Alt},
            {SDL_SCANCODE_F8, KeyChord::Gui},
            {SDL_SCANCODE_F8, 0}}};
  hotkeys.Validate();
  DesktopMenu menu;
  menu.SetHotkeys(hotkeys);
  SDL_KeyboardEvent key{};
  key.type = SDL_EVENT_KEY_DOWN;
  key.scancode = SDL_SCANCODE_F8;
  for (const auto modifiers :
       {SDL_Keymod(SDL_KMOD_LCTRL | SDL_KMOD_RSHIFT | SDL_KMOD_CAPS), SDL_Keymod(SDL_KMOD_RALT),
        SDL_Keymod(SDL_KMOD_LGUI), SDL_Keymod(SDL_KMOD_NONE)}) {
    key.mod = modifiers;
    Check(menu.KeyCommand(key) == DesktopCommand::Pause,
          "All four custom alternatives normalize left/right modifiers and ignore lock state");
  }
  key.mod = SDL_KMOD_CTRL;
  Check(!menu.KeyCommand(key), "A modifier combination must match exactly");
  key.mod = SDL_KMOD_NONE;
  key.scancode = SDL_SCANCODE_P;
  Check(!menu.KeyCommand(key), "Remapping removes the old shortcut");
  key.scancode = SDL_SCANCODE_UNKNOWN;
  Check(!menu.KeyCommand(key), "Unassigned bindings do not consume unknown key events");
  key.scancode = SDL_SCANCODE_F8;
  key.repeat = true;
  Check(!menu.KeyCommand(key), "Custom shortcuts ignore key repeat");
  Check(hotkeys.Label(DesktopCommand::Pause).find("Ctrl+Shift+F8") != std::string::npos,
        "Menu shortcut labels reflect the saved configuration");
  hotkeys.bindings[size_t(DesktopCommand::Quit)][0] = pause[0];
  bool rejected = false;
  try {
    hotkeys.Validate();
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Check(rejected, "A chord cannot trigger two different commands");
}
void ModifierShortcuts() {
  using snes::frontend::Hotkeys;
  using snes::frontend::KeyChord;
  Hotkeys hotkeys;
  hotkeys.bindings[size_t(DesktopCommand::Pause)][0] = {SDL_SCANCODE_LCTRL, 0};
  hotkeys.bindings[size_t(DesktopCommand::PowerCycle)][0] = {SDL_SCANCODE_RALT, KeyChord::Control};
  hotkeys.Validate();
  DesktopMenu menu;
  menu.SetHotkeys(hotkeys);
  const auto send = [&](SDL_Scancode scan, SDL_Keymod mods, SDL_EventType type) {
    SDL_KeyboardEvent key{};
    key.type = type;
    key.scancode = scan;
    key.mod = mods;
    return menu.KeyCommand(key);
  };
  Check(!send(SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, SDL_EVENT_KEY_DOWN),
        "A modifier-only shortcut waits for release");
  Check(send(SDL_SCANCODE_LCTRL, 0, SDL_EVENT_KEY_UP) == DesktopCommand::Pause,
        "Releasing an unused modifier invokes its shortcut once");
  send(SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, SDL_EVENT_KEY_DOWN);
  Check(send(SDL_SCANCODE_O, SDL_KMOD_LCTRL, SDL_EVENT_KEY_DOWN) == DesktopCommand::Open &&
            !send(SDL_SCANCODE_LCTRL, 0, SDL_EVENT_KEY_UP),
        "A modifier used in another chord does not invoke a modifier-only action");
  send(SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, SDL_EVENT_KEY_DOWN);
  SDL_KeyboardEvent repeated{};
  repeated.type = SDL_EVENT_KEY_DOWN;
  repeated.scancode = SDL_SCANCODE_A;
  repeated.mod = SDL_KMOD_LCTRL;
  repeated.repeat = true;
  Check(!menu.KeyCommand(repeated) && !send(SDL_SCANCODE_LCTRL, 0, SDL_EVENT_KEY_UP),
        "Repeated gameplay keys consume a held modifier without repeatedly invoking shortcuts");
  send(SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, SDL_EVENT_KEY_DOWN);
  Check(!send(SDL_SCANCODE_RALT, SDL_KMOD_LCTRL | SDL_KMOD_RALT, SDL_EVENT_KEY_DOWN) &&
            !send(SDL_SCANCODE_LCTRL, SDL_KMOD_RALT, SDL_EVENT_KEY_UP) &&
            send(SDL_SCANCODE_RALT, 0, SDL_EVENT_KEY_UP) == DesktopCommand::PowerCycle,
        "Modifier chords preserve their pressed combination across release order");
  send(SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, SDL_EVENT_KEY_DOWN);
  menu.ResetHotkeys();
  Check(!send(SDL_SCANCODE_LCTRL, 0, SDL_EVENT_KEY_UP),
        "UI capture and focus changes discard pending modifier shortcuts");
}

void SharedUi(SdlVideoOutput &video, const char *capturePath) {
  using snes::frontend::DesktopUi;
  int width = 0, height = 0;
  SDL_GetWindowSize(video.Window(), &width, &height);
  DesktopUi ui(video.Window(), video.Renderer());
  ui.Frame();
  video.SetOverlay([&] { ui.Render(); }, ui.TopInset(), ui.BottomInset());
  const auto frame = [&] {
    ui.Frame();
    video.Redraw();
  };
  const auto capture = [&](const char *suffix) {
    if (!capturePath)
      return;
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_RenderReadPixels(video.Renderer(), nullptr), SDL_DestroySurface);
    Check(surface && SDL_SaveBMP(surface.get(), (std::string(capturePath) + suffix).c_str()),
          SDL_GetError());
  };
  const auto key = [&](SDL_Scancode scan, SDL_Keycode code, SDL_Keymod modifiers = 0,
                       SDL_EventType type = SDL_EVENT_KEY_DOWN) {
    SDL_Event event{};
    event.type = type;
    event.key.windowID = video.WindowID();
    event.key.scancode = scan;
    event.key.key = code;
    event.key.mod = modifiers;
    ui.ProcessEvent(event);
    frame();
  };
  const auto click = [&](float x, float y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = video.WindowID();
    event.motion.x = x;
    event.motion.y = y;
    ui.ProcessEvent(event);
    frame();
    event = {};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = video.WindowID();
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    ui.ProcessEvent(event);
    frame();
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.down = false;
    ui.ProcessEvent(event);
    frame();
  };
  frame();
  int actualWidth = 0, actualHeight = 0;
  const auto resizeDeadline = SDL_GetTicks() + 1000;
  do {
    SDL_PumpEvents();
    SDL_GetWindowSize(video.Window(), &actualWidth, &actualHeight);
    if (actualWidth == width && actualHeight == height + ui.TopInset() + ui.BottomInset())
      break;
    SDL_Delay(1);
  } while (SDL_GetTicks() < resizeDeadline);
  Check(actualWidth == width && actualHeight == height + ui.TopInset() + ui.BottomInset(),
        "Desktop chrome preserves the configured game area");
  const auto viewport = video.InputViewport();
  Check(viewport.y == ui.TopInset() && viewport.height == height,
        "Video starts beneath the shared menu bar");
  Check(viewport.Aim(10, float(ui.TopInset() - 1)).offscreen &&
            viewport.Aim(10, float(actualHeight - ui.BottomInset())).offscreen,
        "Menu and status bars are outside light-gun input");

  click(20, float(ui.TopInset()) / 2);
  click(70, float(ui.TopInset()) + 12);
  Check(ui.Menu().TakeCommand() == DesktopCommand::Open,
        "Shared menu clicks reach the desktop command queue");
  Check(!ui.Menu().TakeCommand(), "A menu selection is consumed exactly once");
  click(80, float(ui.TopInset()) / 2);
  click(90, float(ui.TopInset()) + 12);
  Check(!ui.Menu().TakeCommand(), "Game commands are disabled without a cartridge");
  ui.Menu().Update(true, true);
  frame();
  click(90, float(ui.TopInset()) + 12);
  Check(ui.Menu().TakeCommand() == DesktopCommand::Pause,
        "Loading a cartridge enables the shared game commands");

  auto &editor = ui.PreferencesEditor();
  snes::frontend::Preferences current;
  editor.Open(current, "isolated-preferences.cfg");
  frame();
  frame();
  Check(editor.Active() && ui.WantsKeyboard() && ui.WantsPointer(),
        "The preference editor captures input on every platform");
  if (capturePath) {
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_RenderReadPixels(video.Renderer(), nullptr), SDL_DestroySurface);
    Check(surface && SDL_SaveBMP(surface.get(), capturePath), SDL_GetError());
  }
  const float left = (actualWidth - 620) / 2.0f;
  const float top = (actualHeight - 560) / 2.0f;
  click(left + 20, top + 296);
  click(left + 112, top + 540);
  auto changes = editor.TakeChanges();
  Check(changes && changes->mute && changes->scale == current.scale,
        "Preference widgets submit the edited values through Apply");
  editor.Rejected("The selected output device could not be opened.");
  frame();
  Check(editor.Active(), "A rejected preference change keeps the editor open");
  click(left + 112, top + 540);
  changes = editor.TakeChanges();
  Check(changes && changes->mute, "Rejected changes can be retried without losing edits");
  editor.Applied();
  frame();
  Check(editor.Active(), "Apply keeps the editor open");
  click(left + 20, top + 296);
  SDL_Event escape{};
  escape.type = SDL_EVENT_KEY_DOWN;
  escape.key.windowID = video.WindowID();
  escape.key.scancode = SDL_SCANCODE_ESCAPE;
  escape.key.key = SDLK_ESCAPE;
  ui.ProcessEvent(escape);
  frame();
  Check(!editor.Active() && !editor.TakeChanges(),
        "Escape cancels preference editing without applying changes");
  escape.type = SDL_EVENT_KEY_UP;
  ui.ProcessEvent(escape);
  editor.Open(current, "isolated-preferences.cfg");
  frame();
  frame();
  click(left + 90, top + 44);
  click(left + 300, top + 102);
  SDL_Event binding{};
  binding.type = SDL_EVENT_KEY_DOWN;
  binding.key.windowID = video.WindowID();
  binding.key.scancode = SDL_SCANCODE_C;
  binding.key.key = SDLK_C;
  ui.ProcessEvent(binding);
  frame();
  binding.type = SDL_EVENT_KEY_UP;
  ui.ProcessEvent(binding);
  if (capturePath) {
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_RenderReadPixels(video.Renderer(), nullptr), SDL_DestroySurface);
    Check(surface && SDL_SaveBMP(surface.get(), (std::string(capturePath) + ".input.bmp").c_str()),
          SDL_GetError());
  }
  click(left + 40, top + 540);
  changes = editor.TakeChanges();
  Check(changes && changes->keyboard.up == SDL_SCANCODE_C,
        "The shared Input page captures SDL scancodes without invoking game shortcuts");
  editor.Rejected("This profile could not be saved.");
  frame();
  Check(editor.Active(), "A failed OK operation does not dismiss the editor");
  click(left + 40, top + 540);
  Check(editor.TakeChanges().has_value(), "OK retries the edited profile");
  editor.Applied();
  frame();
  Check(!editor.Active(), "A successful OK operation closes the editor");

  editor.Open(current, "isolated-preferences.cfg");
  frame();
  frame();
  click(left + 280, top + 44);
  click(left + 100, top + 132);
  SDL_Event text{};
  text.type = SDL_EVENT_TEXT_INPUT;
  text.text.windowID = video.WindowID();
  text.text.text = "Pause";
  ui.ProcessEvent(text);
  frame();
  frame();
  capture(".hotkeys.bmp");
  click(left + 300, top + 164);
  key(SDL_SCANCODE_LCTRL, SDLK_LCTRL, SDL_KMOD_LCTRL);
  click(left + 112, top + 540);
  Check(!editor.TakeChanges(), "Apply waits for a complete shortcut assignment");
  key(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
  key(SDL_SCANCODE_F8, SDLK_F8, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
  key(SDL_SCANCODE_F8, SDLK_F8, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT, SDL_EVENT_KEY_UP);
  key(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, SDL_KMOD_LCTRL, SDL_EVENT_KEY_UP);
  key(SDL_SCANCODE_LCTRL, SDLK_LCTRL, 0, SDL_EVENT_KEY_UP);
  click(left + 112, top + 540);
  changes = editor.TakeChanges();
  Check(changes &&
            changes->hotkeys.bindings[size_t(DesktopCommand::Pause)][0] ==
                snes::frontend::KeyChord{SDL_SCANCODE_F8, snes::frontend::KeyChord::Control |
                                                              snes::frontend::KeyChord::Shift},
        "The Hotkeys page captures modifier chords without prematurely assigning a modifier");
  editor.Applied();
  frame();
  click(left + 300, top + 164);
  key(SDL_SCANCODE_LCTRL, SDLK_LCTRL, SDL_KMOD_LCTRL);
  key(SDL_SCANCODE_RALT, SDLK_RALT, SDL_KMOD_LCTRL | SDL_KMOD_RALT);
  key(SDL_SCANCODE_LCTRL, SDLK_LCTRL, SDL_KMOD_RALT, SDL_EVENT_KEY_UP);
  key(SDL_SCANCODE_RALT, SDLK_RALT, 0, SDL_EVENT_KEY_UP);
  click(left + 112, top + 540);
  changes = editor.TakeChanges();
  Check(changes &&
            changes->hotkeys.bindings[size_t(DesktopCommand::Pause)][0] ==
                snes::frontend::KeyChord{SDL_SCANCODE_RALT, snes::frontend::KeyChord::Control},
        "Modifier-only chords retain the held combination when released in either order");
  editor.Applied();
  frame();
  key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
  key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0, SDL_EVENT_KEY_UP);
  Check(!editor.Active(), "Escape closes the editor after a completed assignment");

  editor.Open(current, "isolated-preferences.cfg");
  frame();
  frame();
  click(left + 208, top + 44);
  click(left + 100, top + 124);
  key(SDL_SCANCODE_DOWN, SDLK_DOWN);
  key(SDL_SCANCODE_DOWN, SDLK_DOWN, 0, SDL_EVENT_KEY_UP);
  key(SDL_SCANCODE_DOWN, SDLK_DOWN);
  key(SDL_SCANCODE_DOWN, SDLK_DOWN, 0, SDL_EVENT_KEY_UP);
  key(SDL_SCANCODE_RETURN, SDLK_RETURN);
  key(SDL_SCANCODE_RETURN, SDLK_RETURN, 0, SDL_EVENT_KEY_UP);
  capture(".devices.bmp");
  click(left + 100, top + 292);
  key(SDL_SCANCODE_DOWN, SDLK_DOWN);
  key(SDL_SCANCODE_DOWN, SDLK_DOWN, 0, SDL_EVENT_KEY_UP);
  key(SDL_SCANCODE_RETURN, SDLK_RETURN);
  key(SDL_SCANCODE_RETURN, SDLK_RETURN, 0, SDL_EVENT_KEY_UP);
  click(left + 40, top + 540);
  changes = editor.TakeChanges();
  Check(changes && changes->console.ports[0] == snes::core::ControllerDevice::Multitap &&
            changes->console.expansion == snes::core::Emulator::ExpansionDevice::Satellaview,
        "The Devices page submits controller and expansion choices using keyboard navigation");
  editor.Applied();
  frame();

  snes::core::VideoFrame picture;
  picture.pixels.assign(picture.width * picture.height, 0xff0000ff);
  video.Present(picture);
  video.PresentPending();
  std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
      SDL_RenderReadPixels(video.Renderer(), nullptr), SDL_DestroySurface);
  Uint8 r = 0, g = 0, b = 0, a = 0;
  Check(surface &&
            SDL_ReadSurfacePixel(surface.get(), width / 2, ui.TopInset() + height / 2, &r, &g, &b,
                                 &a) &&
            r == 255 && g == 0 && b == 0,
        "Shared UI rendering preserves the game picture");
  video.SetOverlay({});
}

void Screenshots(SdlVideoOutput &video) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("cupid-desktop-" + std::to_string(SDL_GetTicksNS()) + ".bmp");
  struct RemoveCapture {
    std::filesystem::path path;
    ~RemoveCapture() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup{path};
  snes::core::VideoFrame frame{2, 2, {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff}};
  video.Present(frame);
  frame.pixels.assign(4, 0);
  video.SaveScreenshot(path);
  using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
  const auto utf8 = path.u8string();
  Surface surface(SDL_LoadBMP(reinterpret_cast<const char *>(utf8.c_str())), SDL_DestroySurface);
  Check(surface && surface->w == 2 && surface->h == 2,
        "Screenshots retain native dimensions before deferred presentation");
  constexpr std::array<std::array<Uint8, 3>, 4> colors{
      {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}}};
  for (unsigned index = 0; index < colors.size(); ++index) {
    Uint8 r = 0, g = 0, b = 0, a = 0;
    Check(SDL_ReadSurfacePixel(surface.get(), int(index % 2), int(index / 2), &r, &g, &b, &a),
          SDL_GetError());
    Check(colors[index] == std::array<Uint8, 3>{r, g, b},
          "Screenshots own their submitted pixels with correct color channels");
  }
  video.ClearPicture();
  bool rejected = false;
  try {
    video.SaveScreenshot(path);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected, "Unloading a game removes its last captured picture");
}

void CancelPicker(SdlVideoOutput &video) {
#ifdef _WIN32
  snes::frontend::FileDialogs dialogs(video.Window());
  dialogs.Begin(snes::frontend::LaunchRequest::Kind::Cartridge);
  dialogs.Cancel();
  std::optional<snes::frontend::FileDialogs::Result> result;
  const auto deadline = SDL_GetTicks() + 5000;
  while (!result && SDL_GetTicks() < deadline) {
    SDL_PumpEvents();
    result = dialogs.Poll();
    if (!result)
      SDL_Delay(1);
  }
  Check(result && !result->request && result->error.empty() && !dialogs.Active(),
        "Immediate cancellation waits for the native picker callback before releasing its parent");
#else
  (void)video;
#endif
}
} // namespace

int main(int argc, char **argv) {
#ifndef _WIN32
  if (!SDL_getenv("SNES_TEST_WINDOW"))
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
#endif
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "%s\n", SDL_GetError());
    return 1;
  }
  int result = 0;
  try {
    Shortcuts();
    CustomShortcuts();
    ModifierShortcuts();
    SdlVideoOutput::Config config;
    config.hidden = true;
    config.vsync = false;
    config.deferPresentation = true;
    SdlVideoOutput video(config);
    video.ShowIdle();
    SharedUi(video, argc == 2 ? argv[1] : nullptr);
    CancelPicker(video);
    Screenshots(video);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    result = 1;
  }
  SDL_Quit();
  return result;
}
