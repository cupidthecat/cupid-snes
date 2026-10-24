#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace snes::core {
enum class StatePhase { Synchronize, Capture, Validate, Media, ReadFile, WriteFile, Commit };
// Byte counts describe the current extent; zero total means no fixed extent.
// The item text is borrowed for the observer call.
struct StateProgress {
  StatePhase phase;
  uint64_t completed = 0, total = 0;
  std::string_view item;
};
class StateCancelled final : public std::exception {
public:
  const char *what() const noexcept override {
    return "State operation cancelled";
  }
};

// Reports run on the operation's owning thread. Observers must not mutate or
// reenter the machine. Copies retain the observer through a prepared commit.
class StateOperation {
public:
  using Observer = std::function<bool(const StateProgress &)>;
  StateOperation() = default;
  explicit StateOperation(Observer observer) {
    if (observer)
      observer_ = std::make_shared<const Observer>(std::move(observer));
  }
  explicit operator bool() const noexcept {
    return bool(observer_);
  }
  void Report(StatePhase phase, uint64_t completed = 0, uint64_t total = 0,
              std::string_view item = {}) const {
    if (observer_ && !(*observer_)({phase, completed, total, item}))
      throw StateCancelled();
  }

private:
  std::shared_ptr<const Observer> observer_;
};
} // namespace snes::core
