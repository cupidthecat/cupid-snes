#include "DesktopMenu.hpp"

#include <deque>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace snes::frontend {
struct DesktopMenu::Impl {
  std::deque<DesktopCommand> commands;
#ifdef _WIN32
  HWND window = nullptr;
  HMENU menu = nullptr;
  std::vector<std::pair<HMENU, DesktopCommand>> items;
  static constexpr unsigned firstId = 4100;
  static bool SDLCALL Message(void *userdata, MSG *message) {
    auto &self = *static_cast<Impl *>(userdata);
    if (message->hwnd == self.window && message->message == WM_COMMAND &&
        HIWORD(message->wParam) == 0 && message->lParam == 0) {
      const unsigned id = LOWORD(message->wParam);
      if (id >= firstId && id <= firstId + unsigned(DesktopCommand::Screenshot)) {
        self.commands.push_back(DesktopCommand(id - firstId));
        return false;
      }
    }
    return true;
  }
  void Add(HMENU parent, const wchar_t *text, DesktopCommand command) {
    if (!AppendMenuW(parent, MF_STRING, firstId + unsigned(command), text))
      throw std::runtime_error("Cannot create desktop menu item");
    items.emplace_back(parent, command);
  }
  HMENU Submenu(const wchar_t *text) {
    const auto child = CreatePopupMenu();
    if (!child || !AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(child), text)) {
      if (child)
        DestroyMenu(child);
      throw std::runtime_error("Cannot create desktop menu");
    }
    return child;
  }
#endif
  ~Impl() {
#ifdef _WIN32
    SDL_SetWindowsMessageHook(nullptr, nullptr);
    if (window)
      SetMenu(window, nullptr);
    if (menu)
      DestroyMenu(menu);
#endif
  }
};

DesktopMenu::DesktopMenu(SDL_Window *window) : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
  auto &self = *impl_;
  self.window = static_cast<HWND>(SDL_GetPointerProperty(
      SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (!self.window)
    throw std::runtime_error("Cannot find the desktop window");
  int width = 0, height = 0;
  SDL_GetWindowSize(window, &width, &height);
  self.menu = CreateMenu();
  if (!self.menu)
    throw std::runtime_error("Cannot create desktop menus");
  const auto file = self.Submenu(L"&File"), game = self.Submenu(L"&Game"),
             view = self.Submenu(L"&View");
  self.Add(file, L"&Open Game...\tCtrl+O", DesktopCommand::Open);
  self.Add(file, L"Open Super Game &Boy...", DesktopCommand::OpenHandheld);
  self.Add(file, L"Open Sufami &Turbo...", DesktopCommand::OpenSufami);
  self.Add(file, L"Open &Broadcast Cartridge...", DesktopCommand::OpenBroadcast);
  self.Add(file, L"Open Campus Challenge &92...", DesktopCommand::OpenCampus);
  self.Add(file, L"Open PowerFest &94...", DesktopCommand::OpenPowerFest);
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  self.Add(file, L"&Unload Game\tCtrl+W", DesktopCommand::Unload);
  self.Add(file, L"&Quit\tCtrl+Q", DesktopCommand::Quit);
  self.Add(game, L"&Pause\tP", DesktopCommand::Pause);
  self.Add(game, L"&Frame Advance\tN", DesktopCommand::FrameAdvance);
  self.Add(game, L"&Reset\tF5", DesktopCommand::Reset);
  self.Add(game, L"Power &Cycle", DesktopCommand::PowerCycle);
  self.Add(view, L"&Fullscreen\tF11", DesktopCommand::Fullscreen);
  self.Add(view, L"&Screenshot\tF12", DesktopCommand::Screenshot);
  if (!SetMenu(self.window, self.menu))
    throw std::runtime_error("Cannot attach desktop menus");
  DrawMenuBar(self.window);
  // Attaching a native menu must not consume part of the game picture.
  if (!SDL_SetWindowSize(window, width, height))
    throw std::runtime_error(SDL_GetError());
  SDL_SetWindowsMessageHook(Impl::Message, &self);
#else
  (void)window;
#endif
  Update(false, false);
}
DesktopMenu::~DesktopMenu() = default;

void DesktopMenu::Update(bool loaded, bool paused) {
#ifdef _WIN32
  for (const auto &[menu, command] : impl_->items) {
    const bool needsGame =
        command == DesktopCommand::Unload || command == DesktopCommand::Pause ||
        command == DesktopCommand::FrameAdvance || command == DesktopCommand::Reset ||
        command == DesktopCommand::PowerCycle || command == DesktopCommand::Screenshot;
    EnableMenuItem(menu, Impl::firstId + unsigned(command),
                   MF_BYCOMMAND | ((!needsGame || loaded) ? MF_ENABLED : MF_GRAYED));
    if (command == DesktopCommand::Pause)
      CheckMenuItem(menu, Impl::firstId + unsigned(command),
                    MF_BYCOMMAND | (paused ? MF_CHECKED : MF_UNCHECKED));
  }
#else
  (void)loaded;
  (void)paused;
#endif
}
std::optional<DesktopCommand> DesktopMenu::TakeCommand() {
  if (impl_->commands.empty())
    return {};
  const auto command = impl_->commands.front();
  impl_->commands.pop_front();
  return command;
}
std::optional<DesktopCommand> DesktopMenu::KeyCommand(const SDL_KeyboardEvent &key) {
  if (key.repeat || key.type != SDL_EVENT_KEY_DOWN)
    return {};
  if (key.mod & SDL_KMOD_CTRL) {
    if (key.scancode == SDL_SCANCODE_O)
      return DesktopCommand::Open;
    if (key.scancode == SDL_SCANCODE_W)
      return DesktopCommand::Unload;
    if (key.scancode == SDL_SCANCODE_Q)
      return DesktopCommand::Quit;
    return {};
  }
  if ((key.mod & SDL_KMOD_ALT) && key.scancode == SDL_SCANCODE_RETURN)
    return DesktopCommand::Fullscreen;
  if (key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))
    return {};
  switch (key.scancode) {
  case SDL_SCANCODE_ESCAPE:
    return DesktopCommand::Quit;
  case SDL_SCANCODE_P:
    return DesktopCommand::Pause;
  case SDL_SCANCODE_N:
    return DesktopCommand::FrameAdvance;
  case SDL_SCANCODE_F5:
    return DesktopCommand::Reset;
  case SDL_SCANCODE_F11:
    return DesktopCommand::Fullscreen;
  case SDL_SCANCODE_F12:
    return DesktopCommand::Screenshot;
  default:
    return {};
  }
}
} // namespace snes::frontend
