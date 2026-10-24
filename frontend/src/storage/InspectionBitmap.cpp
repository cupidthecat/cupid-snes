#include "InspectionBitmap.hpp"
#include <SDL3/SDL.h>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace snes::frontend {
std::vector<uint8_t> InspectionBitmap(const core::VideoFrame& picture) {
    if (!picture.width || !picture.height || picture.width > 512 || picture.height > 512 ||
        picture.pixels.size() != size_t(picture.width) * picture.height)
        throw std::invalid_argument("Inspection image has invalid dimensions");
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_CreateSurface(int(picture.width), int(picture.height), SDL_PIXELFORMAT_ABGR8888),
        SDL_DestroySurface);
    if (!surface)
        throw std::runtime_error(SDL_GetError());
    for (unsigned y = 0; y < picture.height; ++y)
        std::memcpy(static_cast<uint8_t*>(surface->pixels) + size_t(y) * surface->pitch,
                    picture.pixels.data() + size_t(y) * picture.width,
                    size_t(picture.width) * sizeof(uint32_t));
    const std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)> stream(SDL_IOFromDynamicMem(),
                                                                       SDL_CloseIO);
    if (!stream || !SDL_SaveBMP_IO(surface.get(), stream.get(), false))
        throw std::runtime_error(SDL_GetError());
    const auto size = SDL_GetIOSize(stream.get());
    if (size < 0 || size > 2 * 1024 * 1024 || SDL_SeekIO(stream.get(), 0, SDL_IO_SEEK_SET) != 0)
        throw std::runtime_error("Cannot read the inspection bitmap");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (SDL_ReadIO(stream.get(), bytes.data(), bytes.size()) != bytes.size())
        throw std::runtime_error(SDL_GetError());
    return bytes;
}
} // namespace snes::frontend
