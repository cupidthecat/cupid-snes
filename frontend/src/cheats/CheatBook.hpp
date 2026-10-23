#pragma once

#include "snes/core/debug/Cheat.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace snes::frontend {
struct CheatEntry {
  std::string name, codes;
  bool enabled = false;
  bool operator==(const CheatEntry &) const = default;
};
struct CheatBook {
  bool enabled = true;
  std::vector<CheatEntry> entries;
  bool operator==(const CheatBook &) const = default;
  std::vector<core::CheatCode> Compile(core::CheatSystem system) const;
};
class CheatStorage {
public:
  static CheatBook Read(const std::filesystem::path &path, core::CheatSystem system);
  static void Write(const std::filesystem::path &path, const CheatBook &book,
                    core::CheatSystem system);
};
} // namespace snes::frontend
