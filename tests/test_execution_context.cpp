#include "snes/core/detail/ExecutionContext.hpp"

#include <cfenv>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using snes::core::detail::ExecutionContext;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct Lifetime {
  unsigned &destroyed;
  ~Lifetime() {
    ++destroyed;
  }
};

void StackAndCancellation() {
  unsigned destroyed = 0;
  std::vector<int> observed;
  ExecutionContext *active = nullptr;
  auto context = std::make_unique<ExecutionContext>([&]() {
    Lifetime lifetime{destroyed};
    int value = 17;
    for (;;) {
      observed.push_back(value++);
      active->Suspend();
    }
  });
  active = context.get();
  for (unsigned call = 0; call < 3; ++call) {
    context->Resume();
    Check(!context->Running() && destroyed == 0, "Suspension retains live automatic objects");
  }
  Check(observed == std::vector<int>{17, 18, 19},
        "Resumption retains local state on the same stack");
  context.reset();
  Check(destroyed == 1 && observed.size() == 3,
        "Cancellation unwinds locals without replaying work");
  {
    ExecutionContext unused([&]() { ++destroyed; });
  }
  Check(destroyed == 1, "Destroying an unused operation never starts it");
}

void ExceptionsAndCallers() {
  unsigned destroyed = 0;
  ExecutionContext *active = nullptr;
  ExecutionContext context([&]() {
    Lifetime lifetime{destroyed};
    active->Suspend();
    throw std::runtime_error("operation failure");
  });
  active = &context;
  bool wrongThreadRejected = false;
  std::thread other([&]() {
    try {
      context.Resume();
    } catch (const std::logic_error &) {
      wrongThreadRejected = true;
    }
  });
  other.join();
  Check(wrongThreadRejected, "A context rejects callers on a different thread");
  context.Resume();
  bool propagated = false;
  try {
    context.Resume();
  } catch (const std::runtime_error &error) {
    propagated = std::string(error.what()) == "operation failure";
  }
  Check(propagated && destroyed == 1 && !context.Running(),
        "Failures unwind on the worker stack and propagate to its caller");
}

void NestedStacksAndFloatingPoint() {
  const int original = std::fegetround();
  std::fesetround(FE_TONEAREST);
  ExecutionContext *active = nullptr;
  bool finished = false;
  ExecutionContext outer([&]() {
    std::fesetround(FE_DOWNWARD);
    ExecutionContext inner([&]() {
      Check(std::fegetround() == FE_DOWNWARD || std::fegetround() == FE_TONEAREST,
            "A new stack has a valid floating point environment");
      std::fesetround(FE_UPWARD);
    });
    inner.Resume();
    Check(std::fegetround() == FE_DOWNWARD, "A nested operation preserves its caller's rounding");
    active->Suspend();
    Check(std::fegetround() == FE_DOWNWARD, "Resumption restores the operation's rounding");
    finished = true;
  });
  active = &outer;
  outer.Resume();
  Check(std::fegetround() == FE_TONEAREST, "Suspension restores the host's rounding");
  std::fesetround(FE_UPWARD);
  outer.Resume();
  outer.Resume();
  Check(finished && std::fegetround() == FE_UPWARD,
        "Completed operations do not restart or change their caller's floating point state");
  std::fesetround(original);
}
} // namespace

int main() {
  try {
    StackAndCancellation();
    ExceptionsAndCallers();
    NestedStacksAndFloatingPoint();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("Execution context lifetime, exception and floating point checks passed");
}
