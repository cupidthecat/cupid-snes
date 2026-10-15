#include "snes/core/detail/ExecutionContext.hpp"

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <ucontext.h>
#endif

namespace snes::core::detail {

struct ExecutionContext::State {
  struct Cancelled {};
  std::function<void()> entry;
  const std::thread::id owner = std::this_thread::get_id();
  std::exception_ptr failure;
  bool started = false, running = false, finished = false, cancelled = false;
#ifdef _WIN32
  void *worker = nullptr, *caller = nullptr;

  static void WINAPI Enter(void *data) {
    static_cast<State *>(data)->Run();
  }
#else
  ucontext_t worker{}, caller{};
  std::unique_ptr<char[]> stack;

  static void Enter(unsigned low, unsigned high) {
    auto address = uintptr_t(low);
#if UINTPTR_MAX > UINT32_MAX
    address |= uintptr_t(high) << 32;
#else
    (void)high;
#endif
    reinterpret_cast<State *>(address)->Run();
  }
#endif

  explicit State(std::function<void()> function) : entry(std::move(function)) {
#ifdef _WIN32
    worker = CreateFiberEx(64 * 1024, 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, &Enter, this);
    if (!worker)
      throw std::system_error(int(GetLastError()), std::system_category(), "CreateFiberEx");
#else
    constexpr size_t stackSize = 1024 * 1024;
    stack = std::make_unique<char[]>(stackSize);
    if (getcontext(&worker) != 0)
      throw std::system_error(errno, std::generic_category(), "getcontext");
    worker.uc_stack.ss_sp = stack.get();
    worker.uc_stack.ss_size = stackSize;
    worker.uc_link = nullptr;
    const auto address = reinterpret_cast<uintptr_t>(this);
    unsigned high = 0;
#if UINTPTR_MAX > UINT32_MAX
    high = unsigned(address >> 32);
#endif
    makecontext(&worker, reinterpret_cast<void (*)()>(&Enter), 2, unsigned(address), high);
#endif
  }

  ~State() {
    if (std::this_thread::get_id() != owner || running)
      std::terminate();
    if (started && !finished) {
      cancelled = true;
      try {
        Resume();
      } catch (...) {
        std::terminate();
      }
      if (!finished)
        std::terminate();
    }
#ifdef _WIN32
    DeleteFiber(worker);
#endif
  }

  void Run() noexcept {
    try {
      entry();
    } catch (const Cancelled &) {
    } catch (...) {
      failure = std::current_exception();
    }
    finished = true;
    running = false;
    SwitchToCaller();
    // A Windows fiber entry must never return: that would terminate its thread.
    std::terminate();
  }

  void SwitchToCaller() noexcept {
#ifdef _WIN32
    SwitchToFiber(caller);
#else
    if (swapcontext(&worker, &caller) != 0)
      std::terminate();
#endif
  }

  void Resume() {
    if (std::this_thread::get_id() != owner || running)
      throw std::logic_error("Execution context resumed from an invalid caller");
    if (!finished) {
#ifdef _WIN32
      const bool converted = !IsThreadAFiber();
      caller =
          converted ? ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH) : GetCurrentFiber();
      if (!caller)
        throw std::system_error(int(GetLastError()), std::system_category(),
                                "ConvertThreadToFiberEx");
      started = running = true;
      SwitchToFiber(worker);
      if (converted && !ConvertFiberToThread())
        throw std::system_error(int(GetLastError()), std::system_category(),
                                "ConvertFiberToThread");
#else
      started = running = true;
      if (swapcontext(&caller, &worker) != 0) {
        running = false;
        throw std::system_error(errno, std::generic_category(), "swapcontext");
      }
#endif
    }
    if (failure)
      std::rethrow_exception(failure);
  }

  void Suspend() {
    if (!running || std::this_thread::get_id() != owner)
      throw std::logic_error("Execution context suspended outside its operation");
    running = false;
    SwitchToCaller();
    running = true;
    if (cancelled)
      throw Cancelled{};
  }
};

ExecutionContext::ExecutionContext(std::function<void()> entry)
    : state_(std::make_unique<State>(std::move(entry))) {}

ExecutionContext::~ExecutionContext() = default;

void ExecutionContext::Resume() {
  state_->Resume();
}

void ExecutionContext::Suspend() {
  state_->Suspend();
}

bool ExecutionContext::Running() const noexcept {
  return state_->running;
}

} // namespace snes::core::detail
