#pragma once

#include "settings/Preferences.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <optional>

namespace snes::frontend {
class PreferencesDialog {
public:
  PreferencesDialog();
  ~PreferencesDialog();
  void Open(const Preferences &current, const std::filesystem::path &file);
  void ProcessEvent(const SDL_Event &event);
  void Draw();
  bool Active() const noexcept;
  std::optional<Preferences> TakeChanges();
  void Applied();
  void Rejected(std::string error);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace snes::frontend
