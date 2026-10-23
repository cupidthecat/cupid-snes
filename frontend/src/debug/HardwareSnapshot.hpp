#pragma once

#include "snes/core/Emulator.hpp"
#include <array>
#include <string>
#include <vector>

namespace snes::frontend {
struct HardwareSnapshot {
    enum class TileFormat { Planar2, Planar4, Planar8, Mode7 };

    struct TileOptions {
        TileFormat format = TileFormat::Planar4;
        uint32_t baseByte = 0, firstTile = 0;
        unsigned palette = 0;
        bool transparentZero = true;
    };

    struct Object {
        unsigned index, rawX, rawY, y, tile, palette, priority, width, height;
        int x;
        bool large, flipX, flipY;
    };

    uint64_t frame = 0, clock = 0;
    core::Ppu::IO ppu{};
    std::vector<uint16_t> vram;
    std::array<uint16_t, 256> cgram{};
    std::array<uint8_t, 544> oam{};
    std::array<uint8_t, 128> dsp{};
    std::array<core::Dsp::Voice, 8> voices{};
    std::array<uint8_t, 65536> audioRam{};
    unsigned dspPhase = 0;
    int noise = 0, counter = 0;

    static HardwareSnapshot Capture(const core::Emulator& machine);
    static uint32_t Color(uint16_t rgb555) noexcept;
    static unsigned Depth(TileFormat format);
    unsigned TileCount(TileFormat format) const;
    core::VideoFrame Tiles(const TileOptions& options) const;
    std::array<Object, 128> Objects() const;
    core::VideoFrame ObjectImage(unsigned index, bool applyFlips = true,
                                 bool transparentZero = true) const;
    uint16_t DirectoryAddress(unsigned voice, bool loop) const;
    std::array<float, 12> DecodedSamples(unsigned voice) const;
    std::string AudioText() const;

  private:
    void ValidateVram() const;
    uint8_t VramByte(size_t address) const;
    uint8_t Pixel(TileFormat format, size_t baseByte, unsigned x, unsigned y) const;
    uint32_t PixelColor(unsigned index, unsigned x, unsigned y, bool transparentZero) const;
};
} // namespace snes::frontend
