#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace snes::core {
enum class CheatSystem { Console, Handheld };

struct CheatCode {
  uint32_t address = 0;
  uint8_t value = 0;
  std::optional<uint8_t> compare;
  bool operator==(const CheatCode &) const = default;
};

// A group accepts one code per line, or codes separated by '+'.
std::vector<CheatCode> DecodeCheats(std::string_view text, CheatSystem system);
std::string FormatCheat(const CheatCode &code, CheatSystem system);
uint8_t SubstituteCheat(std::span<const CheatCode> codes, uint32_t address, uint8_t value) noexcept;

class CheatEngine {
public:
  using Reader = std::function<uint8_t(uint32_t)>;
  using Writer = std::function<void(uint32_t, uint8_t)>;
  // Call between emulation steps. Removing codes restores their saved bytes.
  void Set(std::span<const CheatCode> codes, const Reader &read, const Writer &write);
  void Refresh(const Writer &write) const;
  // The old hardware must no longer be used after forgetting its saved bytes.
  void Forget() noexcept;
  bool Empty() const noexcept {
    return entries_.empty();
  }

private:
  struct Entry {
    CheatCode code;
    uint8_t original = 0;
    bool active = false;
  };
  std::vector<Entry> entries_;
};
} // namespace snes::core
