#pragma once

#include "CheatDialog.hpp"
#include "CpuDebuggerDialog.hpp"
#include "DesktopMenu.hpp"
#include "MemoryDialog.hpp"
#include "PreferencesDialog.hpp"
#include "StateManagerDialog.hpp"
#include "snes/core/StateOperation.hpp"
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
  StateManagerDialog &StateManager();
  CheatDialog &CheatsEditor();
  MemoryDialog &MemoryEditor();
  CpuDebuggerDialog &CpuEditor();
  void SetStatus(std::string text);
  void ShowError(std::string text);
  bool ErrorOpen() const;
  void BeginStateOperation(std::string title);
  void UpdateStateOperation(const core::StateProgress &progress);
  void CancelStateOperation();
  bool StateOperationCancelled() const;
  void EndStateOperation();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace snes::frontend
