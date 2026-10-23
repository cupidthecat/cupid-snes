// SDL3 texture upload, scaling, and window presentation.

#include "SdlVideoOutput.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace snes::frontend {

namespace {
int ProbeFrameIndex() {
    static int probeFrame = -2;
    if (probeFrame == -2) {
        const char* v = std::getenv("SNES_PROBE_FRAME");
        probeFrame = -1;
        if (v && *v) {
            const std::string_view value(v);
            int parsed = -1;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                parsed >= 0)
                probeFrame = parsed;
        }
    }
    return probeFrame;
}
} // namespace

// Construction / Destruction

SdlVideoOutput::SdlVideoOutput() : SdlVideoOutput(Config{}) {
}

SdlVideoOutput::SdlVideoOutput(const Config& config)
    : deferredPresentation_(config.deferPresentation), aspectCorrection_(config.aspectCorrection),
      integerScaling_(config.integerScaling), linearFilter_(config.linearFilter) {
    if (config.scale < 1 || config.scale > 6)
        throw std::invalid_argument("Window scale must be between 1 and 6");
    scale_ = config.scale;
    baseWidth_ = config.baseW;
    baseHeight_ = config.baseH;
    int windowW =
        int(std::lround(config.baseW * (aspectCorrection_ ? 8.0 / 7 : 1.0) * config.scale));
    int windowH = config.baseH * config.scale;

    const auto flags = SDL_WINDOW_RESIZABLE | (config.hidden ? SDL_WINDOW_HIDDEN : 0);
    if (!SDL_CreateWindowAndRenderer(config.title.c_str(), windowW, windowH, flags, &window_,
                                     &renderer_)) {
        throw std::runtime_error(std::string("SDL_CreateWindowAndRenderer failed: ") +
                                 SDL_GetError());
    }

    SDL_SetRenderVSync(renderer_, config.vsync ? 1 : 0);

    // Clear to black initially
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderPresent(renderer_);
}

SdlVideoOutput::~SdlVideoOutput() {
    if (texture_)
        SDL_DestroyTexture(texture_);
    if (renderer_)
        SDL_DestroyRenderer(renderer_);
    if (window_)
        SDL_DestroyWindow(window_);
}

void SdlVideoOutput::SetPresentation(const Presentation& options) {
    if (options.scale < 1 || options.scale > 6)
        throw std::invalid_argument("Window scale must be between 1 and 6");
    if (texture_ &&
        !SDL_SetTextureScaleMode(texture_, options.linearFilter ? SDL_SCALEMODE_LINEAR
                                                                : SDL_SCALEMODE_NEAREST))
        throw std::runtime_error(SDL_GetError());
    if (options.scale != scale_ || options.aspectCorrection != aspectCorrection_) {
        const unsigned lines = texH_ ? (texH_ > 240 ? texH_ / 2 : texH_) : baseHeight_;
        const int width = int(
            std::lround(baseWidth_ * (options.aspectCorrection ? 8.0 / 7 : 1.0) * options.scale));
        if (!SDL_SetWindowSize(window_, width,
                               int(lines) * options.scale + topInset_ + bottomInset_)) {
            const std::string error = SDL_GetError();
            if (texture_)
                SDL_SetTextureScaleMode(texture_, linearFilter_ ? SDL_SCALEMODE_LINEAR
                                                                : SDL_SCALEMODE_NEAREST);
            throw std::runtime_error(error);
        }
    }
    scale_ = options.scale;
    aspectCorrection_ = options.aspectCorrection;
    integerScaling_ = options.integerScaling;
    linearFilter_ = options.linearFilter;
    Redraw();
}

void SdlVideoOutput::SetOverlay(std::function<void()> draw, int topInset, int bottomInset) {
    if (topInset < 0 || bottomInset < 0)
        throw std::invalid_argument("Desktop insets must not be negative");
    int width = 0, height = 0;
    SDL_GetWindowSize(window_, &width, &height);
    if (!SDL_SetWindowSize(window_, width,
                           std::max(1, height - topInset_ - bottomInset_ + topInset + bottomInset)))
        throw std::runtime_error(SDL_GetError());
    overlay_ = std::move(draw);
    topInset_ = topInset;
    bottomInset_ = bottomInset;
}

// IVideoOutput::Present

void SdlVideoOutput::Present(const snes::core::VideoFrame& frame) {
    if (!renderer_)
        return;
    if (frame.pixels.empty())
        return;

    EnsureTexture(frame.width, frame.height);
    if (!texture_)
        return;

    lastPixels_.assign(frame.pixels.begin(), frame.pixels.end());

    // Upload pixel data.
    // PPU output is ABGR8888 (as a uint32_t: alpha MSB, red LSB).
    // The texture was created with SDL_PIXELFORMAT_ABGR8888 to match.
    int pitch = static_cast<int>(frame.width * sizeof(uint32_t));
    SDL_UpdateTexture(texture_, nullptr, frame.pixels.data(), pitch);

    // Diagnostic captures are opt-in and count submitted emulated frames.
    const int probeFrame = ProbeFrameIndex();
    if (probeFrame >= 0 && submittedFrames_ == static_cast<uint64_t>(probeFrame)) {
        SDL_Surface* surf =
            SDL_CreateSurface(static_cast<int>(frame.width), static_cast<int>(frame.height),
                              SDL_PIXELFORMAT_ABGR8888);
        if (surf) {
            char filename[32];
            std::snprintf(filename, sizeof(filename), "frame%d.bmp", probeFrame);
            for (uint32_t y = 0; y < frame.height; ++y)
                std::memcpy(static_cast<unsigned char*>(surf->pixels) + y * surf->pitch,
                            frame.pixels.data() + y * frame.width, pitch);
            SDL_SaveBMP(surf, filename);
            SDL_DestroySurface(surf);
        }
    }
    ++submittedFrames_;
    pending_ = true;
    if (!deferredPresentation_)
        PresentPending();
}

void SdlVideoOutput::PresentPending() {
    if (!pending_ || !renderer_ || !texture_)
        return;
    pending_ = false;

    // Fit the picture to the window, using integer scaling when it fits.
    int winW = 0, winH = 0;
    SDL_GetWindowSize(window_, &winW, &winH);
    const auto viewport = InputViewport();
    SDL_FRect dst{viewport.x, viewport.y, viewport.width, viewport.height};

    // One-shot: log presentation dimensions
    static bool logged = false;
    if (!logged) {
        fprintf(stderr, "[VIDEO] frame=%ux%u window=%dx%d dst=%.0fx%.0f+%.0f+%.0f\n", texW_, texH_,
                winW, winH, dst.w, dst.h, dst.x, dst.y);
        logged = true;
    }

    // Draw the picture and gun sights over a black letterbox.
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
    for (unsigned gun = 0; gun < std::min(gunCount_, 2u); ++gun) {
        const auto& aim = gunAim_[gun];
        if (aim.offscreen || viewport.visibleLines == 0)
            continue;
        const float x = dst.x + (aim.x + 0.5f) * dst.w / 256;
        const float y = dst.y + (aim.y + 0.5f) * dst.h / viewport.visibleLines;
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        const SDL_FRect horizontal{x - 8, y - 1, 17, 3}, vertical{x - 1, y - 8, 3, 17};
        SDL_RenderFillRect(renderer_, &horizontal);
        SDL_RenderFillRect(renderer_, &vertical);
        SDL_SetRenderDrawColor(renderer_, gun ? 255 : 0, gun ? 0 : 255, gun ? 255 : 0, 255);
        SDL_RenderLine(renderer_, x - 7, y, x + 7, y);
        SDL_RenderLine(renderer_, x, y - 7, x, y + 7);
    }
    if (overlay_)
        overlay_();
    SDL_RenderPresent(renderer_);
}

void SdlVideoOutput::Redraw() {
    if (!texture_) {
        ShowIdle();
        return;
    }
    pending_ = true;
    PresentPending();
}

void SdlVideoOutput::ClearPicture() {
    if (texture_)
        SDL_DestroyTexture(texture_);
    texture_ = nullptr;
    texW_ = texH_ = 0;
    pending_ = false;
    submittedFrames_ = 0;
    lastPixels_.clear();
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    if (overlay_)
        overlay_();
    SDL_RenderPresent(renderer_);
}

void SdlVideoOutput::ShowIdle() {
    if (texture_ || !lastPixels_.empty())
        ClearPicture();
    SDL_SetRenderDrawColor(renderer_, 22, 24, 30, 255);
    SDL_RenderClear(renderer_);
    SDL_SetRenderScale(renderer_, 2, 2);
    SDL_SetRenderDrawColor(renderer_, 235, 237, 245, 255);
    SDL_RenderDebugText(renderer_, 20, 30, "Cupid-SNES");
    SDL_SetRenderDrawColor(renderer_, 170, 178, 195, 255);
    SDL_RenderDebugText(renderer_, 20, 56, "Open a game from the File menu");
    SDL_RenderDebugText(renderer_, 20, 76, "or drop a cartridge file here.");
    SDL_SetRenderScale(renderer_, 1, 1);
    if (overlay_)
        overlay_();
    SDL_RenderPresent(renderer_);
}

void SdlVideoOutput::SaveScreenshot(const std::filesystem::path& path) const {
    if (lastPixels_.empty() || !texW_ || !texH_)
        throw std::runtime_error("No game picture is available yet");
    auto* surface =
        SDL_CreateSurfaceFrom(int(texW_), int(texH_), SDL_PIXELFORMAT_ABGR8888,
                              const_cast<uint32_t*>(lastPixels_.data()), int(texW_ * 4));
    if (!surface)
        throw std::runtime_error(SDL_GetError());
    const auto name = path.u8string();
    const bool saved = SDL_SaveBMP(surface, reinterpret_cast<const char*>(name.c_str()));
    SDL_DestroySurface(surface);
    if (!saved)
        throw std::runtime_error(std::string("Cannot save screenshot: ") + SDL_GetError());
}

// WindowID

uint32_t SdlVideoOutput::WindowID() const noexcept {
    return window_ ? SDL_GetWindowID(window_) : 0;
}

VideoViewport SdlVideoOutput::InputViewport() const {
    int width = 0, height = 0;
    SDL_GetWindowSize(window_, &width, &height);
    auto viewport = MakeVideoViewport(width, std::max(0, height - topInset_ - bottomInset_),
                                      texH_ ? texH_ : 224, aspectCorrection_, integerScaling_);
    viewport.y += topInset_;
    return viewport;
}

// EnsureTexture - (re)create texture when frame dimensions change

void SdlVideoOutput::EnsureTexture(uint32_t w, uint32_t h) {
    if (texture_ && texW_ == w && texH_ == h)
        return;

    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }

    // PPU pixel layout: uint32_t = (0xFF << 24) | (B << 16) | (G << 8) | R
    // This is SDL_PIXELFORMAT_ABGR8888 (named MSB→LSB in the uint32_t).
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                 static_cast<int>(w), static_cast<int>(h));

    if (texture_) {
        texW_ = w;
        texH_ = h;
        // Filtering affects presentation; captured core pixels retain their native values.
        SDL_SetTextureScaleMode(texture_,
                                linearFilter_ ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    } else {
        texW_ = texH_ = 0;
    }
}

} // namespace snes::frontend
