#pragma once

#include "DesktopMenu.hpp"
#include "PreferencesDialog.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <string>

namespace snes::frontend {
class DesktopUi {
public:
  DesktopUi(SDL_Window *window, SDL_Renderer *renderer);
  ~DesktopUi();
  void ProcessEvent(const SDL_Event &event);
  void Frame();
  void Render();
  int TopInset() const;
  int BottomInset() const;
  bool WantsKeyboard() const;
  bool WantsPointer() const;
  DesktopMenu &Menu();
  PreferencesDialog &PreferencesEditor();
  void SetStatus(std::string text);
  void ShowError(std::string text);
  bool ErrorOpen() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace snes::frontend
