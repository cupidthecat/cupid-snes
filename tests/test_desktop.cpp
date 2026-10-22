#include "SdlVideoOutput.hpp"
#include "desktop/DesktopMenu.hpp"
#include "desktop/FileDialogs.hpp"
#include "snes/core/FilePath.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>

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
  SDL_KeyboardEvent key{};
  key.type = SDL_EVENT_KEY_DOWN;
  key.scancode = SDL_SCANCODE_P;
  Check(DesktopMenu::KeyCommand(key) == DesktopCommand::Pause, "P pauses the game");
  key.repeat = true;
  Check(!DesktopMenu::KeyCommand(key), "Held shortcuts do not repeatedly toggle a command");
  key.repeat = false;
  key.mod = SDL_KMOD_ALT;
  Check(!DesktopMenu::KeyCommand(key), "Alt+P does not pause the game");
  key.scancode = SDL_SCANCODE_RETURN;
  Check(DesktopMenu::KeyCommand(key) == DesktopCommand::Fullscreen, "Alt+Enter toggles fullscreen");
  key.mod = SDL_KMOD_CTRL;
  key.scancode = SDL_SCANCODE_O;
  Check(DesktopMenu::KeyCommand(key) == DesktopCommand::Open, "Ctrl+O opens a game");
  key.scancode = SDL_SCANCODE_N;
  Check(!DesktopMenu::KeyCommand(key), "Ctrl+N does not execute an unmodified shortcut");
  key.mod = SDL_KMOD_NONE;
  Check(DesktopMenu::KeyCommand(key) == DesktopCommand::FrameAdvance, "N advances one frame");
  key.type = SDL_EVENT_KEY_UP;
  Check(!DesktopMenu::KeyCommand(key), "Releasing a key does not execute a command");
}

void NativeMenu(SdlVideoOutput &video) {
  int width = 0, height = 0;
  SDL_GetWindowSize(video.Window(), &width, &height);
  DesktopMenu menu(video.Window());
#ifdef _WIN32
  const auto window = static_cast<HWND>(SDL_GetPointerProperty(
      SDL_GetWindowProperties(video.Window()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  const auto root = GetMenu(window);
  RECT client{};
  Check(GetClientRect(window, &client) && client.right == width && client.bottom == height,
        "Native menus preserve the configured game area");
  Check(root && GetMenuItemCount(root) == 3, "The window exposes File, Game and View menus");
  constexpr unsigned first = 4100;
  const auto file = GetSubMenu(root, 0), game = GetSubMenu(root, 1);
  Check(GetMenuState(file, first + unsigned(DesktopCommand::Unload), MF_BYCOMMAND) & MF_GRAYED,
        "Unload is unavailable without a cartridge");
  menu.Update(true, true);
  Check(!(GetMenuState(file, first + unsigned(DesktopCommand::Unload), MF_BYCOMMAND) & MF_GRAYED),
        "Loading a cartridge enables unload");
  Check(GetMenuState(game, first + unsigned(DesktopCommand::Pause), MF_BYCOMMAND) & MF_CHECKED,
        "The native pause check reflects the running session");
  Check(PostMessageW(window, WM_COMMAND, first + unsigned(DesktopCommand::Pause), 0),
        "Posting a native menu command succeeds");
  std::optional<DesktopCommand> command;
  const auto deadline = SDL_GetTicks() + 1000;
  while (!command && SDL_GetTicks() < deadline) {
    SDL_PumpEvents();
    command = menu.TakeCommand();
    if (!command)
      SDL_Delay(1);
  }
  Check(command == DesktopCommand::Pause, "Native menu selections reach the desktop command queue");
  Check(!menu.TakeCommand(), "Native menu selections are consumed exactly once");
  std::array<snes::frontend::LaunchRequest, 3> recent{};
  recent[0].paths[0] =
      snes::core::PathText(std::filesystem::path(u8"\u65e5\u672c\u8a9e & game.sfc"));
  recent[1].kind = snes::frontend::LaunchRequest::Kind::Handheld;
  recent[1].paths = {"adapter.sfc", "guest.gb", "boot.bin", ""};
  recent[2].kind = snes::frontend::LaunchRequest::Kind::SufamiTurbo;
  recent[2].paths = {"bios.bin", "slot-a.st", "slot-b.st", ""};
  menu.SetRecent(recent);
  const auto history = GetSubMenu(file, 6);
  Check(history && GetMenuItemCount(history) == 5,
        "Recent games include entries and a clear action");
  wchar_t label[512]{};
  Check(GetMenuStringW(history, first + unsigned(DesktopCommand::Recent0), label, 512,
                       MF_BYCOMMAND) &&
            std::wstring(label).find(L"\u65e5\u672c\u8a9e && game.sfc") != std::wstring::npos,
        "Recent game menus preserve Unicode names and literal ampersands");
  Check(GetMenuStringW(history, first + unsigned(DesktopCommand::Recent2), label, 512,
                       MF_BYCOMMAND) &&
            std::wstring(label).find(L"slot-a.st + slot-b.st") != std::wstring::npos,
        "Slot history identifies both selected games");
  command.reset();
  Check(PostMessageW(window, WM_COMMAND, first + unsigned(DesktopCommand::Recent1), 0),
        "A recent game selection can be posted");
  const auto recentDeadline = SDL_GetTicks() + 1000;
  while (!command && SDL_GetTicks() < recentDeadline) {
    SDL_PumpEvents();
    command = menu.TakeCommand();
    if (!command)
      SDL_Delay(1);
  }
  Check(command == DesktopCommand::Recent1, "Native recent selections retain their history index");
  menu.SetRecent({});
  Check(GetMenuItemCount(history) == 1,
        "Clearing recent games replaces old entries with an empty marker");
#else
  menu.Update(true, true);
  Check(!menu.TakeCommand(), "An untouched menu has no pending commands");
#endif
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
    SdlVideoOutput::Config config;
    config.hidden = true;
    config.vsync = false;
    config.deferPresentation = true;
    SdlVideoOutput video(config);
    video.ShowIdle();
    if (argc == 2) {
      std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
          SDL_RenderReadPixels(video.Renderer(), nullptr), SDL_DestroySurface);
      Check(surface && SDL_SaveBMP(surface.get(), argv[1]), SDL_GetError());
    }
    NativeMenu(video);
    CancelPicker(video);
    Screenshots(video);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    result = 1;
  }
  SDL_Quit();
  return result;
}
