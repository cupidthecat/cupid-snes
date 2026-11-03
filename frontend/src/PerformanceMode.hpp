#pragma once

#include <SDL3/SDL.h>

namespace snes::frontend {

// GameMode keeps intermittent emulation work from being throttled between fields.
// Load it optionally so desktop builds do not require an additional dependency.
class PerformanceMode {
  public:
    PerformanceMode() {
#ifdef __linux__
        const auto* setting = SDL_getenv("SNES_GAMEMODE");
        if (setting && SDL_strcmp(setting, "0") == 0)
            return;
        // Application tests use dummy devices and should not change host policy.
        const auto* driver = SDL_GetCurrentVideoDriver();
        if (driver && SDL_strcmp(driver, "dummy") == 0)
            return;
        library_ = SDL_LoadObject("libgamemode.so.0");
        if (library_) {
            start_ = reinterpret_cast<Request>(
                SDL_LoadFunction(library_, "real_gamemode_request_start"));
            end_ = reinterpret_cast<Request>(
                SDL_LoadFunction(library_, "real_gamemode_request_end"));
        }
#endif
    }

    ~PerformanceMode() {
        SetActive(false);
        if (library_)
            SDL_UnloadObject(library_);
    }

    PerformanceMode(const PerformanceMode&) = delete;
    PerformanceMode& operator=(const PerformanceMode&) = delete;

    void SetActive(bool wanted) {
        if (wanted == wanted_)
            return;
        wanted_ = wanted;
        if (!start_ || !end_)
            return;
        if (wanted)
            active_ = start_() == 0;
        else if (active_) {
            end_();
            active_ = false;
        }
    }

  private:
    using Request = int (*)();
    SDL_SharedObject* library_ = nullptr;
    Request start_ = nullptr, end_ = nullptr;
    bool wanted_ = false, active_ = false;
};

} // namespace snes::frontend
