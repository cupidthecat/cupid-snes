#include "SdlVideoOutput.hpp"

#include <cstdio>
#include <memory>
#include <stdexcept>

using snes::core::VideoFrame;
using snes::frontend::SdlVideoOutput;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

Uint32 ReadPixel(SDL_Renderer* renderer, int x = -1, int y = -1) {
    // These checks deliberately use the dummy/software backend, whose surface
    // persists after presentation. GPU backbuffer retention is not tested here.
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_RenderReadPixels(renderer, nullptr), SDL_DestroySurface);
    Check(surface != nullptr, SDL_GetError());
    Uint8 r = 0, g = 0, b = 0, a = 0;
    Check(SDL_ReadSurfacePixel(surface.get(), x < 0 ? surface->w / 2 : x,
                               y < 0 ? surface->h / 2 : y, &r, &g, &b, &a),
          SDL_GetError());
    return Uint32(r) | (Uint32(g) << 8) | (Uint32(b) << 16);
}

void DeferredPresentation() {
    SdlVideoOutput::Config config;
    config.scale = 1;
    config.vsync = false;
    config.deferPresentation = true;
    SdlVideoOutput video(config);
    VideoFrame frame;
    frame.pixels.assign(frame.width * frame.height, 0xff0000ff);
    const auto initial = ReadPixel(video.Renderer());
    video.Present(frame);
    Check(ReadPixel(video.Renderer()) == initial,
          "Preparing a frame leaves the displayed picture intact");
    video.PresentPending();
    Check(ReadPixel(video.Renderer()) == 0xff,
          "The scheduled presentation displays the prepared pixels");

    frame.width = 512;
    frame.height = 478;
    frame.pixels.assign(frame.width * frame.height, 0xff00ff00);
    video.Present(frame);
    Check(ReadPixel(video.Renderer()) == 0xff,
          "Preparing a new resolution does not display it early");
    video.PresentPending();
    Check(ReadPixel(video.Renderer()) == 0xff00,
          "Deferred presentation handles hires interlaced output");
    video.PresentPending();
    Check(ReadPixel(video.Renderer()) == 0xff00,
          "Presenting without a new picture keeps the last frame");
}

void ImmediatePresentation() {
    SdlVideoOutput::Config config;
    config.scale = 1;
    config.vsync = false;
    SdlVideoOutput video(config);
    VideoFrame frame;
    frame.pixels.assign(frame.width * frame.height, 0xffff0000);
    video.Present(frame);
    Check(ReadPixel(video.Renderer()) == 0xff0000, "Default output still presents synchronously");
}

void ExplicitVsyncSelection() {
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    SdlVideoOutput::Config config;
    config.scale = 1;
    config.vsync = false;
    SdlVideoOutput video(config);
    int interval = -1;
    Check(SDL_GetRenderVSync(video.Renderer(), &interval) && interval == 0,
          "Explicit pacing disables renderer synchronization despite a global hint");
    SDL_ResetHint(SDL_HINT_RENDER_VSYNC);
}

void AspectAndFractionalAim() {
    for (bool aspect : {false, true}) {
        for (bool integer : {false, true}) {
            const auto view = snes::frontend::MakeVideoViewport(1000, 800, 478, aspect, integer);
            const auto center = view.Aim(view.x + view.width / 2, view.y + view.height / 2);
            Check(!center.offscreen && center.x == 128 && center.y == 119,
                  "Host aspect and fractional scaling retain native light-gun coordinates");
            Check(view.width <= 1000 && view.height <= 800 && view.width > 0 && view.height > 0,
                  "Presentation options fit the picture without clipping");
            const auto ratio = view.width / view.height;
            Check(std::abs(ratio - (256.0f / 239) * (aspect ? 8.0f / 7 : 1)) < 0.0001f,
                  "Aspect correction changes presentation geometry without changing visible rows");
        }
    }
}

void FilteringChangesOnlyPresentation() {
    VideoFrame frame;
    frame.pixels.resize(frame.width * frame.height);
    for (unsigned y = 0; y < frame.height; ++y)
        for (unsigned x = 0; x < frame.width; ++x)
            frame.pixels[y * frame.width + x] = x < 128 ? 0xff0000ff : 0xffff0000;
    for (bool linear : {false, true}) {
        SdlVideoOutput::Config config;
        config.scale = 2;
        config.vsync = false;
        config.linearFilter = linear;
        SdlVideoOutput video(config);
        video.Present(frame);
        const auto boundary = ReadPixel(video.Renderer(), 255, 224);
        if (linear)
            Check((boundary & 255) > 0 && ((boundary >> 16) & 255) > 0,
                  "Linear filtering blends neighboring displayed pixels");
        else
            Check(boundary == 255, "Nearest filtering retains a sharp displayed pixel edge");
        Check(frame.pixels[127] == 0xff0000ff && frame.pixels[128] == 0xffff0000,
              "Host filtering leaves the generated frame intact");
    }
}
} // namespace

int main() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "%s\n", SDL_GetError());
        return 1;
    }
    int result = 0;
    try {
        DeferredPresentation();
        ImmediatePresentation();
        ExplicitVsyncSelection();
        AspectAndFractionalAim();
        FilteringChangesOnlyPresentation();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        result = 1;
    }
    SDL_Quit();
    return result;
}
