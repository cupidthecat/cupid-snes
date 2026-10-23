#pragma once

#include "cheats/CheatBook.hpp"
#include <optional>

namespace snes::frontend {
class CheatDialog {
public:
  struct Change {
    CheatBook book;
    bool replaceUnreadable = false;
  };
  void Open(const CheatBook &book, core::CheatSystem system, const std::filesystem::path &path,
            std::string error = {});
  bool Active() const noexcept {
    return active_;
  }
  void Draw();
  std::optional<Change> TakeChanges();
  bool TakeReload();
  void Applied(const CheatBook &book);
  void Rejected(std::string error);
  void Append(CheatEntry entry);

private:
  CheatBook draft_;
  core::CheatSystem system_ = core::CheatSystem::Console;
  std::string path_, error_, fileError_;
  std::optional<Change> change_;
  int selected_ = -1;
  bool active_ = false, opening_ = false, reload_ = false, replaceUnreadable_ = false;
};
} // namespace snes::frontend
