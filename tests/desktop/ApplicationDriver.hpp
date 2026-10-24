#pragma once

#include "imgui_internal.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <stdexcept>
#include <thread>

namespace snes::tests::desktop {
inline void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class Predicate> void Wait(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

inline void OnMain(const std::function<void()>& operation) {
    struct Request {
        const std::function<void()>& operation;
        std::exception_ptr error;
    } request{operation, {}};

    Check(SDL_RunOnMainThread(
              [](void* data) {
                  auto& request = *static_cast<Request*>(data);
                  try {
                      request.operation();
                  } catch (...) {
                      request.error = std::current_exception();
                  }
              },
              &request, true),
          "Application observer runs on the owning thread");
    if (request.error)
        std::rethrow_exception(request.error);
}

inline void Push(SDL_Event event) {
    Check(SDL_PushEvent(&event), "Queue application input");
}

inline void Frames() {
    // Observe complete frames so queued input remains reliable on busy hosts.
    const auto current = [](bool* pending = nullptr) {
        int frame = 0;
        OnMain([&] {
            if (const auto* context = ImGui::GetCurrentContext()) {
                frame = context->FrameCount;
                if (pending)
                    *pending = !context->InputEventsQueue.empty();
            }
        });
        return frame;
    };
    const auto previous = current();
    Wait(
        [&] {
            bool pending = true;
            return current(&pending) >= previous + 2 && !pending;
        },
        "Application consumes queued UI input");
}

inline void Ready() {
    Wait(
        [] {
            bool progress = false;
            OnMain([&] {
                if (ImGui::GetCurrentContext())
                    if (const auto* window = ImGui::FindWindowByName("State operation"))
                        progress = window->Active;
            });
            return !progress;
        },
        "State progress closes before the next interaction");
    Frames();
}

inline void Key(SDL_WindowID window, SDL_Scancode scan, SDL_Keymod modifiers = 0,
                SDL_Keycode key = 0, bool waitForFrames = true) {
    Ready();
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = window;
    event.key.scancode = scan;
    event.key.key = key ? key : SDL_GetKeyFromScancode(scan, modifiers, false);
    event.key.mod = modifiers;
    Push(event);
    event.type = SDL_EVENT_KEY_UP;
    event.key.mod = 0;
    Push(event);
    if (waitForFrames) {
        Frames();
        Ready();
    }
}

inline void Text(SDL_WindowID window, const char* value) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = window;
    event.text.text = value;
    Push(event);
    Frames();
}

inline void Click(SDL_WindowID window, float x, float y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = window;
    event.motion.x = x;
    event.motion.y = y;
    Push(event);
    Frames();
    event = {};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = window;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    Push(event);
    Frames();
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.down = false;
    Push(event);
    Frames();
}

inline void Quit() {
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    Push(event);
}

inline bool SDLCALL Created(void* data, SDL_Event* event) {
    if (event->type == SDL_EVENT_WINDOW_SHOWN)
        static_cast<std::atomic<SDL_WindowID>*>(data)->store(event->window.windowID);
    return true;
}
} // namespace snes::tests::desktop
