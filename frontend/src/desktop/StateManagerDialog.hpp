#pragma once

#include "storage/StateStorage.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <optional>

namespace snes::frontend {
class StateManagerDialog {
public:
  struct Action {
    enum class Kind { Save, Load, Rename, Remove, Undo, Redo } kind;
    StateStorage::Key key;
    std::string name;
  };
  explicit StateManagerDialog(SDL_Renderer *renderer);
  ~StateManagerDialog();
  void Open(const StateStorage &storage, bool undo, bool redo);
  bool Active() const noexcept;
  void Draw();
  std::optional<Action> TakeAction();
  void Completed(bool undo, bool redo);
  void Rejected(std::string error);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace snes::frontend
