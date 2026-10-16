#pragma once

#include <functional>
#include <memory>

namespace snes::core::detail {

// A resumable stack on the calling thread. Destruction unwinds a suspended
// operation; Resume, Suspend and destruction must use the creating thread.
class ExecutionContext {
public:
  explicit ExecutionContext(std::function<void()> entry);
  ~ExecutionContext();
  ExecutionContext(const ExecutionContext &) = delete;
  ExecutionContext &operator=(const ExecutionContext &) = delete;

  void Resume();
  void Suspend();
  bool Running() const noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace snes::core::detail
