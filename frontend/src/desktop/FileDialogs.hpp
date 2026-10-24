#pragma once

#include "session/LaunchRequest.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <optional>

namespace snes::frontend {
class FileDialogs {
  public:
    struct Result {
        std::optional<LaunchRequest> request;
        std::string error;
    };

    explicit FileDialogs(SDL_Window* window);
    ~FileDialogs();
    void Begin(LaunchRequest::Kind kind);
    // Keep the parent window alive until Poll returns the cancelled result.
    void Cancel();
    bool Active() const noexcept;
    std::optional<Result> Poll();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace snes::frontend
