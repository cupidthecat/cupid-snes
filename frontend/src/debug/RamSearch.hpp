#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace snes::frontend {
class RamSearch {
public:
  struct Config {
    unsigned bytes = 1;
    bool signedValues = false, bigEndian = false, aligned = false;
    bool operator==(const Config &) const = default;
  };
  enum class Relation { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
  enum class Operand { Constant, Previous, Initial };
  struct Filter {
    Relation relation = Relation::Equal;
    Operand operand = Operand::Constant;
    int64_t value = 0, adjustment = 0;
  };
  struct Result {
    uint32_t offset = 0;
    int64_t current = 0, previous = 0, initial = 0;
  };
  using Progress = std::function<void(size_t, size_t)>;

  // Callbacks may throw to cancel. A failed operation retains the old search.
  void Start(std::span<const uint8_t> bytes, Config config, const Progress &progress = {});
  void Refine(std::span<const uint8_t> bytes, Filter filter, const Progress &progress = {});
  void Undo();
  void Clear() noexcept;
  bool Active() const noexcept {
    return active_;
  }
  bool CanUndo() const noexcept {
    return !history_.empty();
  }
  size_t Count() const noexcept {
    return count_;
  }
  size_t Size() const noexcept {
    return current_.size();
  }
  Config Configuration() const noexcept {
    return config_;
  }
  std::vector<Result> Results(size_t first, size_t count) const;
  static int64_t ParseValue(std::string_view text, Config config);

private:
  struct History {
    std::vector<uint8_t> current, previous;
    std::vector<uint64_t> candidates;
    size_t count = 0;
  };
  static void Validate(Config config);
  int64_t Value(std::span<const uint8_t> bytes, size_t offset) const noexcept;
  Config config_;
  bool active_ = false;
  size_t count_ = 0;
  std::vector<uint8_t> initial_, previous_, current_;
  std::vector<uint64_t> candidates_;
  std::vector<History> history_;
};
} // namespace snes::frontend
