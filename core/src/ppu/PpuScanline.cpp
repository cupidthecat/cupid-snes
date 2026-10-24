#include "snes/core/PpuScanline.hpp"

#include "snes/core/PpuBackground.hpp"
#include "snes/core/PpuObject.hpp"
#include "snes/core/PpuPixelMixer.hpp"

namespace snes::core {

PpuScanlineStatus SamplePpuScanline(Ppu::Line& line, std::span<const uint16_t> vram,
                                    std::span<const uint8_t, Ppu::OamSize> oam) {
    auto state = line.io;
    PpuBackground background;
    PpuObject objects;
    Ppu::Latch latch;
    if (state.obj.aboveEnable || state.obj.belowEnable) {
        // Object evaluation prepares the row consumed by the following scanline.
        objects.BeginLine(uint16_t(line.y - 1), state.obj);
        for (unsigned clock = 0; clock <= 1016; clock += 8)
            objects.Clock(uint16_t(clock), line.fieldID, state, latch, oam, vram);
        objects.Clock(65535, line.fieldID, state, latch, oam, vram);
        objects.BeginLine(line.y, state.obj);
    }
    if (line.y) {
        const auto sampled = background.SampleLine(line.y, line.fieldID, state, vram);
        for (unsigned x = 0; x < 256; ++x) {
            const auto pixel = MixPpuPixel(state, line.palette, sampled[x], objects, x);
            line.sampledAbove[x] = pixel.above;
            line.sampledBelow[x] = pixel.below;
        }
    }
    return {state.obj.rangeOver, state.obj.timeOver};
}

} // namespace snes::core
