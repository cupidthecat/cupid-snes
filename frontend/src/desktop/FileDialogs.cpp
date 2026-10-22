#include "FileDialogs.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace snes::frontend {
namespace {
constexpr SDL_DialogFileFilter consoleFilters[] = {{"Console cartridges", "sfc;smc;bin"},
                                                   {"All files", "*"}};
constexpr SDL_DialogFileFilter handheldFilters[] = {{"Handheld cartridges", "gb;gbc;sgb"},
                                                    {"All files", "*"}};
constexpr SDL_DialogFileFilter bootFilters[] = {{"Boot firmware", "bin;rom"}, {"All files", "*"}};
constexpr SDL_DialogFileFilter slotFilters[] = {{"Slot cartridges", "st;bs;sfc;smc;bin"},
                                                {"All files", "*"}};
int Choice(SDL_Window *window, const char *message, const char *first, const char *second) {
  const SDL_MessageBoxButtonData buttons[] = {
      {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, first},
      {0, 2, second},
      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"}};
  const SDL_MessageBoxData box{
      SDL_MESSAGEBOX_INFORMATION, window, "Open cartridge", message, 3, buttons, nullptr};
  int choice = 0;
  if (!SDL_ShowMessageBox(&box, &choice))
    throw std::runtime_error(SDL_GetError());
  return choice;
}
} // namespace

struct FileDialogs::Impl {
  struct Selection {
    std::mutex mutex;
    bool ready = false;
    std::string path, error;
  };
  SDL_Window *window;
  LaunchRequest request;
  unsigned index = 0, count = 0;
  bool active = false, closing = false;
  std::shared_ptr<Selection> selection;
  std::optional<Result> immediate;

  void CloseNative() {
#ifdef _WIN32
    const auto parent = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    const auto popup = GetLastActivePopup(parent);
    if (popup && popup != parent && GetWindow(popup, GW_OWNER) == parent)
      PostMessageW(popup, WM_CLOSE, 0, 0);
#endif
  }

  static void SDLCALL Selected(void *userdata, const char *const *files, int) {
    std::unique_ptr<std::shared_ptr<Selection>> owner(
        static_cast<std::shared_ptr<Selection> *>(userdata));
    auto &value = **owner;
    std::lock_guard lock(value.mutex);
    if (!files)
      value.error = SDL_GetError();
    else if (files[0])
      value.path = files[0];
    value.ready = true;
  }
  void Show() {
    using Kind = LaunchRequest::Kind;
    const char *title = "Open console game";
    const SDL_DialogFileFilter *filters = consoleFilters;
    if (request.kind == Kind::Handheld) {
      title = index == 0   ? "Select Super Game Boy console BIOS"
              : index == 1 ? "Select handheld game"
                           : "Select matching handheld boot firmware";
      filters = index == 0 ? consoleFilters : index == 1 ? handheldFilters : bootFilters;
    } else if (request.kind == Kind::SufamiTurbo) {
      title = index == 0   ? "Select Sufami Turbo BIOS"
              : index == 1 ? "Select slot A game"
                           : "Select slot B game";
      filters = index == 0 ? consoleFilters : slotFilters;
    } else if (request.kind == Kind::Broadcast) {
      title = index == 0 ? "Select broadcast cartridge or BIOS" : "Select memory pack";
      filters = index == 0 ? consoleFilters : slotFilters;
    } else if (request.kind == Kind::Campus92 || request.kind == Kind::PowerFest94) {
      constexpr const char *titles[] = {"Select competition boot cartridge", "Select first game",
                                        "Select second game", "Select third game"};
      title = titles[index];
    }
    const auto properties = SDL_CreateProperties();
    if (!properties)
      throw std::runtime_error(SDL_GetError());
    SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
                           const_cast<SDL_DialogFileFilter *>(filters));
    SDL_SetNumberProperty(properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, title);
    SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Open");
    selection = std::make_shared<Selection>();
    // The callback owns its shared state even if the application closes first.
    auto *userdata = new std::shared_ptr<Selection>(selection);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, Selected, userdata, properties);
    SDL_DestroyProperties(properties);
  }
};

FileDialogs::FileDialogs(SDL_Window *window) : impl_(std::make_unique<Impl>()) {
  impl_->window = window;
}
FileDialogs::~FileDialogs() = default;
bool FileDialogs::Active() const noexcept {
  return impl_->active;
}
void FileDialogs::Begin(LaunchRequest::Kind kind) {
  auto &self = *impl_;
  if (self.active)
    return;
  self.request = {};
  self.closing = false;
  self.request.kind = kind;
  self.index = 0;
  using Kind = LaunchRequest::Kind;
  self.count = kind == Kind::Cartridge ? 1 : kind == Kind::Handheld ? 3 : 4;
  if (kind == Kind::SufamiTurbo) {
    const int choice =
        Choice(self.window, "How many slot games would you like to load?", "One game", "Two games");
    if (!choice) {
      self.immediate = Result{};
      return;
    }
    self.count = choice == 1 ? 2 : 3;
  } else if (kind == Kind::Broadcast) {
    const int choice = Choice(self.window, "Would you like to attach a memory pack?",
                              "Browse for pack", "Empty slot");
    if (!choice) {
      self.immediate = Result{};
      return;
    }
    self.count = choice == 1 ? 2 : 1;
    self.request.paths[1] = "-";
  }
  self.active = true;
  try {
    self.Show();
  } catch (...) {
    self.active = false;
    throw;
  }
}
void FileDialogs::Cancel() {
  auto &self = *impl_;
  if (self.active) {
    self.closing = true;
    self.CloseNative();
  }
}
std::optional<FileDialogs::Result> FileDialogs::Poll() {
  auto &self = *impl_;
  if (self.immediate)
    return std::exchange(self.immediate, {});
  if (!self.active || !self.selection)
    return {};
  if (self.closing)
    self.CloseNative();
  std::string path, error;
  {
    std::lock_guard lock(self.selection->mutex);
    if (!self.selection->ready)
      return {};
    path = self.selection->path;
    error = self.selection->error;
  }
  self.selection.reset();
  if (self.closing) {
    self.active = false;
    return Result{};
  }
  if (path.empty() || !error.empty()) {
    self.active = false;
    return Result{{}, std::move(error)};
  }
  self.request.paths[self.index++] = std::move(path);
  if (self.index == self.count) {
    self.active = false;
    return Result{self.request, {}};
  }
  try {
    self.Show();
  } catch (...) {
    self.active = false;
    throw;
  }
  return {};
}
} // namespace snes::frontend
