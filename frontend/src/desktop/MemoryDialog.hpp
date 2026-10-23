#pragma once

#include "debug/MemoryTools.hpp"
#include <optional>

namespace snes::frontend {
class MemoryDialog {
public:
  struct Dump {
    std::string region;
    std::vector<uint8_t> bytes;
  };
  void Open(MemoryTools &tools, bool search);
  bool Active() const noexcept {
    return active_;
  }
  void Draw();
  std::optional<CheatEntry> TakeCheat();
  std::optional<Dump> TakeDump();
  void Rejected(std::string message);

private:
  void Memory();
  void Search();
  void Refresh();
  void Attempt(const std::function<void()> &operation);
  MemoryTools *tools_ = nullptr;
  RamSearch::Config config_;
  size_t offset_ = 0;
  std::vector<uint8_t> page_;
  std::string address_, value_ = "0", adjustment_ = "0", name_ = "Memory value", message_;
  int relation_ = 0, operand_ = 0;
  std::optional<RamSearch::Result> selected_;
  std::optional<CheatEntry> cheat_;
  std::optional<Dump> dump_;
  bool active_ = false, opening_ = false, searchTab_ = false, selectTab_ = false;
  bool focusFilter_ = false;
};
} // namespace snes::frontend
