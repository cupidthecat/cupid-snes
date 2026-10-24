#pragma once

#include <SDL3/SDL.h>

namespace snes::frontend::detail {
void CloseOwnedDialogs(SDL_Window* window);
}
