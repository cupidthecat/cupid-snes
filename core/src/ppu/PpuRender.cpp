// snes emulator
// core/src/ppu/PpuRender.cpp
// Compose retained background samples, object rows, and color math.

// PpuRender.cpp - SNES PPU scanline rendering pipeline
//
// Implements the scanline rendering pipeline:
//   - IO state snapshot per visible scanline
//   - Retained background samples from the clocked fetch and shift stages
//   - Sprite rendering (128 OAM evaluation, 32-per-line, 4bpp tiles)
//   - Window masking (BG/OBJ layer windows + color math windows)
//   - Priority-based compositing with above/below (main/sub) screens
//   - Color math (add/subtract with saturation, halve, per-layer enable)
//   - Brightness application via lookup table
//   - Output to RGBA8888 framebuffer
//

#include "snes/core/Ppu.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace snes::core {

namespace {
int gRenderFrame = 0;

int ProbeFrame() {
    static int probeFrame = -2;
    if (probeFrame == -2) {
        const char* v = std::getenv("SNES_PROBE_FRAME");
        probeFrame = (v && *v) ? std::atoi(v) : -1;
    }
    return probeFrame;
}

bool OamTraceEnabled() {
    static int enabled = -1;
    if (enabled == -1) {
        const char* v = std::getenv("SNES_OAM_TRACE");
        enabled = (v && *v && *v != '0') ? 1 : 0;
    }
    return enabled == 1;
}
}

// Brightness lookup table

void Ppu::BuildLightTable() {
    // 16 brightness levels × 32768 BGR555 entries
    // Index and output are both plain BGR555 - no channel swapping.
    lightTableData_ = std::make_unique<uint16_t[]>(16 * 32768);
    for (int l = 0; l < 16; l++) {
        lightTable_[l] = lightTableData_.get() + l * 32768;
        double luma = static_cast<double>(l) / 15.0;
        for (uint32_t color = 0; color < 32768; color++) {
            uint32_t r = (color >>  0) & 31;
            uint32_t g = (color >>  5) & 31;
            uint32_t b = (color >> 10) & 31;
            uint32_t ar = static_cast<uint32_t>(luma * r + 0.5);
            uint32_t ag = static_cast<uint32_t>(luma * g + 0.5);
            uint32_t ab = static_cast<uint32_t>(luma * b + 0.5);
            lightTable_[l][color] =
                static_cast<uint16_t>((ab << 10) | (ag << 5) | ar);
        }
    }
}

// OAM parsing - raw bytes → structured Object array

void Ppu::ParseOam() {
    for (int n = 0; n < 128; n++) {
        auto& obj = objects_[n];

        // Low table: 4 bytes per object at offset n*4
        uint16_t base = static_cast<uint16_t>(n) * 4;
        uint8_t b0 = oam_[base + 0];
        uint8_t b1 = oam_[base + 1];
        uint8_t b2 = oam_[base + 2];
        uint8_t b3 = oam_[base + 3];

        // High table: 2 bits per object at offset 512 + n/4
        uint8_t hiByte = oam_[512 + (n >> 2)];
        uint8_t hiBits = (hiByte >> ((n & 3) * 2)) & 3;

        obj.x         = static_cast<uint16_t>(b0) | ((hiBits & 1) << 8); // 9-bit x
        // Convert OAM Y to visible scanline-counter coordinates.
        obj.y         = static_cast<uint8_t>(b1 + 1);
        obj.character = b2;
        obj.nameselect = (b3 >> 0) & 1;
        obj.palette    = (b3 >> 1) & 7;
        obj.priority   = (b3 >> 4) & 3;
        obj.hflip      = (b3 >> 6) & 1;
        obj.vflip      = (b3 >> 7) & 1;
        obj.size       = (hiBits >> 1) & 1;
    }
}

// Frame rendering - renders all cached scanlines at VBlank

void Ppu::RenderFrame() {
    if (lineCount_ == 0) return;

    bool hires = false, interlace = displayInterlace_, overscan = false;
    for (int i = 0; i < lineCount_; ++i) {
        const auto& registers = lines_[lineStart_ + i].io;
        hires |= lines_[lineStart_ + i].backgroundHires;
        hires |= registers.pseudoHires || registers.bgMode == 5 || registers.bgMode == 6;
        overscan |= registers.overscan;
    }
    const uint16_t width = hires ? 512 : 256;
    const uint16_t height = uint16_t((overscan ? 239 : 224) * (interlace ? 2 : 1));
    if (width != frameWidth_ || height != frameHeight_ || interlace != frameInterlace_)
        std::fill_n(output_.get(), OutputWidth * OutputHeight, 0xff000000u);
    frameWidth_ = width;
    frameHeight_ = height;
    frameInterlace_ = interlace;

    // Parse OAM once per frame
    ParseOam();

    if (OamTraceEnabled() && gRenderFrame == ProbeFrame()) {
        std::fprintf(stderr, "[OAM frame=%d] baseSize=%u first=%u\n",
                     gRenderFrame, io_.obj.baseSize, io_.obj.first);
        for (int i = 0; i < 128; i++) {
            const auto& obj = objects_[i];
            uint8_t w = obj.size ? kObjLargeWidth[io_.obj.baseSize]
                                 : kObjSmallWidth[io_.obj.baseSize];
            uint8_t h = obj.size ? kObjLargeHeight[io_.obj.baseSize]
                                 : kObjSmallHeight[io_.obj.baseSize];
            bool onscreenX = !(obj.x > 256 && static_cast<int>(obj.x) + w - 1 < 512);
            bool onscreenY = obj.y <= 224 || static_cast<uint16_t>(obj.y) + h >= 256;
            if (!onscreenX || !onscreenY) continue;
            std::fprintf(stderr,
                         "  idx=%3d x=%3u y=%3u chr=%3u size=%u w=%2u h=%2u pri=%u pal=%u ns=%u hf=%u vf=%u\n",
                         i, obj.x, obj.y, obj.character, obj.size ? 1 : 0, w, h,
                         obj.priority, obj.palette, obj.nameselect ? 1 : 0,
                         obj.hflip ? 1 : 0, obj.vflip ? 1 : 0);
        }
    }

    // Render each cached scanline
    for (int i = 0; i < lineCount_; i++) {
        RenderLine(lines_[lineStart_ + i]);
    }

    lineStart_ = 0;
    lineCount_ = 0;
    vramSnapshotCount_ = 0;
    gRenderFrame++;
}

// RenderLine - the core per-scanline pipeline

void Ppu::RenderLine(Line& line) {
  const auto &events = rasterEvents_[line.y];
  ColorMathLatch latch;

  if (events.empty()) {
    RenderLineSpan(line, line.io, 0, 256, latch);
    return;
  }

    const IO snapshot = line.io;
    IO state = snapshot;
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        ApplyRasterEvent(state, *it, false);
    }

    int x = 0;
    for (const auto& event : events) {
      const int next = std::clamp<int>(event.x, x, 256);
      if (next > x)
        RenderLineSpan(line, state, x, next, latch);
      ApplyRasterEvent(state, event, true);
      x = next;
    }
    if (x < 256)
      RenderLineSpan(line, state, x, 256, latch);
    line.io = snapshot;
}

void Ppu::RenderLineSpan(Line &line, const IO &state, int x0, int x1, ColorMathLatch &latch) {
  line.io = state;
  const uint16_t y = line.y;

  // Export visible rows directly: visible vcounter 1 maps to output row 0.
  uint16_t outputY = static_cast<uint16_t>(y - 1);
  if (frameInterlace_)
    outputY = uint16_t(outputY * 2 + unsigned(line.fieldID));

  auto *outRow = output_.get() + static_cast<size_t>(outputY) * OutputWidth;
  const int scale = frameWidth_ == 512 ? 2 : 1;
  const int outputStart = x0 * scale;
  const int outputEnd = x1 * scale;

  // Display disabled → black
  if (line.io.displayDisable) {
    std::fill(outRow + outputStart, outRow + outputEnd, 0u);
    return;
  }

  // Background samples have already crossed the fetch and shift stages.
  std::copy_n(line.backgroundAbove, 256, line.above);
  std::copy_n(line.backgroundBelow, 256, line.below);
  RenderObjects(line, line.io.obj);

  // Step 3: Compute color windows
  RenderWindowColor(line, line.io.col.window, line.io.col.window.aboveMask, line.windowAbove);
  RenderWindowColor(line, line.io.col.window, line.io.col.window.belowMask, line.windowBelow);

  // Step 4: Final compositing with brightness
  auto *luma = lightTable_[line.io.displayBrightness];

  const bool hires = line.io.pseudoHires || line.io.bgMode == 5 || line.io.bgMode == 6;
  const auto expandColor = [luma](uint16_t color555) {
    uint16_t dimmed = luma[color555];

    // Convert BGR555 → RGBA8888
    // dimmed is BGR555: bbbbb_ggggg_rrrrr
    // Expand each five-bit channel with (c << 3) | (c >> 2).
    uint32_t r5 = (dimmed >> 0) & 31;
    uint32_t g5 = (dimmed >> 5) & 31;
    uint32_t b5 = (dimmed >> 10) & 31;
    uint32_t r8 = (r5 << 3) | (r5 >> 2);
    uint32_t g8 = (g5 << 3) | (g5 >> 2);
    uint32_t b8 = (b5 << 3) | (b5 >> 2);
    return (0xFFu << 24) | (b8 << 16) | (g8 << 8) | r8;
  };
  for (int x = x0; x < x1; ++x) {
    // The sub dot consumes the preceding main dot's decisions before the
    // current main dot replaces them. Blank spans leave those latches alone.
    const uint16_t sub = hires ? CompositeHiresSubPixel(line, x, latch) : 0;
    const uint16_t main = CompositePixel(line, x, latch);
    if (scale == 2) {
      outRow[x * 2] = expandColor(hires ? sub : main);
      outRow[x * 2 + 1] = expandColor(main);
    } else {
      outRow[x] = expandColor(main);
    }
  }
}

// PlotAbove / PlotBelow - priority-based pixel merge

void Ppu::PlotAbove(Line& line, int x, uint8_t source,
                    uint8_t priority, uint16_t color) {
    if (priority > line.above[x].priority) {
        line.above[x] = {source, priority, color};
    }
}

void Ppu::PlotBelow(Line& line, int x, uint8_t source,
                    uint8_t priority, uint16_t color) {
    if (priority > line.below[x].priority) {
        line.below[x] = {source, priority, color};
    }
}

// DirectColor - BG1 direct color mode (modes 3/4)

uint16_t Ppu::DirectColor(uint8_t paletteIndex, uint8_t paletteColor) {
    // paletteIndex = bgr bits from palette number (3 bits)
    // paletteColor = BBGGGRRR (8-bit pixel value)
    return static_cast<uint16_t>(
        ((paletteColor << 2) & 0x001C) + ((paletteIndex <<  1) & 0x0002) +  // R
        ((paletteColor << 4) & 0x0380) + ((paletteIndex <<  5) & 0x0040) +  // G
        ((paletteColor << 7) & 0x6000) + ((paletteIndex << 10) & 0x1000));  // B
}

// RenderObjects - sprite rendering
//

void Ppu::RenderObjects(Line& line, const ObjectIO& obj) {
    if (!obj.aboveEnable && !obj.belowEnable) return;

    // Phase 1: Sprite evaluation - find which sprites are on this scanline
    bool winAbove[256], winBelow[256];
    RenderWindow(line, obj.window, obj.window.aboveEnable, winAbove);
    RenderWindow(line, obj.window, obj.window.belowEnable, winBelow);

    int itemCount = 0;
    int tileCount = 0;
    constexpr int ItemLimit = 32;
    constexpr int TileLimit = 34;
    // Clear items/tiles
    for (int i = 0; i < 128; i++) {
        line.items[i] = {false, 0, 0, 0};
        line.tiles[i] = {};
    }

    uint16_t y = static_cast<uint16_t>(line.y & 0xFF);

    for (int n = 0; n < 128; n++) {
        uint8_t idx = static_cast<uint8_t>((obj.first + n) & 127);
        const auto& object = line.objects[idx];

        // Determine sprite dimensions
        uint8_t w, h;
        if (!object.size) {
            w = kObjSmallWidth[obj.baseSize];
            h = kObjSmallHeight[obj.baseSize];
            if (obj.interlace && obj.baseSize >= 6) h = 16; // hardware quirk
        } else {
            w = kObjLargeWidth[obj.baseSize];
            h = kObjLargeHeight[obj.baseSize];
        }

        // Off-screen X check: if sprite starts beyond 256 and doesn't wrap onto screen
        if (object.x > 256 && static_cast<int>(object.x) + w - 1 < 512) continue;

        // Y range check in 8-bit space (raw OAM Y and wrapped scanline).
        uint32_t effectiveH = h >> static_cast<uint32_t>(obj.interlace);
        uint32_t objY = object.y;
        uint32_t lineY = y;

        // Check Y range (with 8-bit wrapping)
        bool inRange = false;
        if (lineY >= objY && lineY < objY + effectiveH) {
            inRange = true;
        } else if (objY + effectiveH >= 256 && lineY < ((objY + effectiveH) & 0xFF)) {
            inRange = true;
        }

        if (!inRange) continue;

        if (itemCount >= ItemLimit) break;
        line.items[itemCount] = {true, idx, w, h};
        itemCount++;
    }

    // Phase 2: Tile fetching (reverse order - lower index = higher priority)
    for (int n = itemCount - 1; n >= 0; n--) {
        const auto& item = line.items[n];
        if (!item.valid) continue;

        const auto& object = line.objects[item.index];

        uint32_t w = item.width;
        uint32_t h = item.height;
        uint32_t tileWidth = w >> 3; // tiles across

        // Compute Y within sprite
        int spriteY = static_cast<int>((y - object.y) & 0xFF);
        if (obj.interlace) spriteY <<= 1;

        // V-flip: non-square sprites flip within each width-sized block
        if (object.vflip) {
            if (w == h) {
                spriteY = static_cast<int>(h - 1) - spriteY;
            } else if (spriteY < static_cast<int>(w)) {
                spriteY = static_cast<int>(w - 1) - spriteY;
            } else {
                spriteY = static_cast<int>(w) + static_cast<int>(w - 1)
                        - (spriteY - static_cast<int>(w));
            }
        }

        // Interlace field offset
        if (obj.interlace) {
            spriteY = !object.vflip ? spriteY + static_cast<int>(line.fieldID)
                                    : spriteY - static_cast<int>(line.fieldID);
        }

        uint32_t sx = static_cast<uint32_t>(object.x) & 0x1FF;
        spriteY &= 0xFF;

        // Tile data base address
        uint16_t tiledataAddr = obj.tiledataAddress;
        if (object.nameselect) {
            // OBSEL nameselect selects an alternate character page at
            // ((1 + nameselect) << 12) words from the base tile data address.
            tiledataAddr += static_cast<uint16_t>((1u + obj.nameselect) << 12);
        }

        uint16_t characterX = object.character & 0x0F;
        uint16_t characterY = static_cast<uint16_t>(
            (((object.character >> 4) + (static_cast<uint32_t>(spriteY) >> 3)) & 0x0F) << 4);

        for (uint32_t tileX = 0; tileX < tileWidth; tileX++) {
            uint32_t objectX = (sx + (tileX << 3)) & 0x1FF;
            // Skip tiles entirely off-screen (x=256 sprites are not skipped)
            if (sx != 256 && objectX >= 256 && objectX + 7 < 512) continue;

            if (tileCount >= TileLimit) break;

            uint32_t mirrorX = object.hflip ? (tileWidth - 1 - tileX) : tileX;
            uint16_t address = tiledataAddr +
                static_cast<uint16_t>(
                    ((characterY + ((characterX + mirrorX) & 15)) << 4));
            address = (address & 0x7FF0) + static_cast<uint16_t>(spriteY & 7);

            ObjectTile tile;
            tile.valid    = true;
            tile.x        = static_cast<uint16_t>(objectX);
            tile.priority = object.priority;
            tile.palette  = static_cast<uint8_t>(128 + (object.palette << 4));
            tile.hflip    = object.hflip;

            // 4bpp tile data (2 VRAM words = 32 bits)
            uint32_t d = 0;
            d  = static_cast<uint32_t>(line.vram[(address + 0) & 0x7FFF]) <<  0;
            d |= static_cast<uint32_t>(line.vram[(address + 8) & 0x7FFF]) << 16;
            tile.data = d;

            line.tiles[tileCount] = tile;
            tileCount++;
        }
    }

    // Phase 3: Pixel decode into temporary buffers
    uint8_t paletteArr[256] = {};
    uint8_t priorityArr[256] = {};

    for (int n = 0; n < tileCount && n < TileLimit; n++) {
        const auto& tile = line.tiles[n];
        if (!tile.valid) continue;

        uint32_t tileX = tile.x;
        for (int px = 0; px < 8; px++) {
            tileX &= 511;
            if (tileX < 256) {
                uint32_t shift = tile.hflip ? static_cast<uint32_t>(px)
                                            : (7 - static_cast<uint32_t>(px));
                uint32_t color = 0;
                color  = (tile.data >> (shift +  0)) & 1;
                color += (tile.data >> (shift +  7)) & 2;
                color += (tile.data >> (shift + 14)) & 4;
                color += (tile.data >> (shift + 21)) & 8;

                if (color != 0) {
                    paletteArr[tileX] = tile.palette + static_cast<uint8_t>(color);
                    priorityArr[tileX] = obj.priority[tile.priority];
                }
            }
            tileX++;
        }
    }

    // Phase 4: Plot to above/below
    for (int x = 0; x < 256; x++) {
        if (priorityArr[x] == 0) continue;

        // OBJ1 = palettes 0-3 (indices < 192), OBJ2 = palettes 4-7 (indices >= 192)
        uint8_t src = (paletteArr[x] < 192) ? Source::OBJ1 : Source::OBJ2;
        uint16_t color = line.cgram[paletteArr[x]];

        if (obj.aboveEnable && !winAbove[x])
            PlotAbove(line, x, src, priorityArr[x], color);
        if (obj.belowEnable && !winBelow[x])
            PlotBelow(line, x, src, priorityArr[x], color);
    }
}

// RenderWindow - compute window mask for a BG/OBJ layer
//
// output[x] = true means pixel is INSIDE the window → will be clipped

void Ppu::RenderWindow(const Line& line, const WindowLayer& wl, bool enable,
                       bool output[256]) {
    if (!enable || (!wl.oneEnable && !wl.twoEnable)) {
        std::memset(output, 0, 256 * sizeof(bool));
        return;
    }

    if (wl.oneEnable && !wl.twoEnable) {
        // Only window 1
        for (int x = 0; x < 256; x++) {
            bool inside = (x >= line.io.window.oneLeft && x <= line.io.window.oneRight);
            output[x] = inside ^ wl.oneInvert;
        }
        return;
    }

    if (!wl.oneEnable && wl.twoEnable) {
        // Only window 2
        for (int x = 0; x < 256; x++) {
            bool inside = (x >= line.io.window.twoLeft && x <= line.io.window.twoRight);
            output[x] = inside ^ wl.twoInvert;
        }
        return;
    }

    // Both windows enabled - combine with mask logic
    for (int x = 0; x < 256; x++) {
        bool one = (x >= line.io.window.oneLeft && x <= line.io.window.oneRight) ^ wl.oneInvert;
        bool two = (x >= line.io.window.twoLeft && x <= line.io.window.twoRight) ^ wl.twoInvert;
        switch (wl.mask) {
        case 0: output[x] = one | two;   break; // OR
        case 1: output[x] = one & two;   break; // AND
        case 2: output[x] = one ^ two;   break; // XOR
        case 3: output[x] = !(one ^ two); break; // XNOR
        }
    }
}

// RenderWindowColor - compute window mask for color math
//
// mask: 0=always on, 1=inside window only, 2=outside window only, 3=never
// output[x] = true means color math/display is ENABLED at this pixel

void Ppu::RenderWindowColor(const Line& line, const WindowColor& wc,
                            uint8_t mask, bool output[256]) {
    switch (mask) {
    case 0: // Always
        for (int x = 0; x < 256; x++) output[x] = true;
        return;
    case 3: // Never
        for (int x = 0; x < 256; x++) output[x] = false;
        return;
    default:
        break;
    }

    // Compute window region first
    bool windowRegion[256];

    if (!wc.oneEnable && !wc.twoEnable) {
        std::memset(windowRegion, 0, 256 * sizeof(bool));
    } else if (wc.oneEnable && !wc.twoEnable) {
        for (int x = 0; x < 256; x++) {
            bool inside = (x >= line.io.window.oneLeft && x <= line.io.window.oneRight);
            windowRegion[x] = inside ^ wc.oneInvert;
        }
    } else if (!wc.oneEnable && wc.twoEnable) {
        for (int x = 0; x < 256; x++) {
            bool inside = (x >= line.io.window.twoLeft && x <= line.io.window.twoRight);
            windowRegion[x] = inside ^ wc.twoInvert;
        }
    } else {
        for (int x = 0; x < 256; x++) {
            bool one = (x >= line.io.window.oneLeft && x <= line.io.window.oneRight) ^ wc.oneInvert;
            bool two = (x >= line.io.window.twoLeft && x <= line.io.window.twoRight) ^ wc.twoInvert;
            switch (wc.mask) {
            case 0: windowRegion[x] = one | two;   break;
            case 1: windowRegion[x] = one & two;   break;
            case 2: windowRegion[x] = one ^ two;   break;
            case 3: windowRegion[x] = !(one ^ two); break;
            }
        }
    }

    // mask=1: inside window → enabled
    // mask=2: outside window → enabled
    for (int x = 0; x < 256; x++) {
        output[x] = (mask == 1) ? windowRegion[x] : !windowRegion[x];
    }
}

// CompositePixel - color math compositing per pixel
//

uint16_t Ppu::CompositePixel(const Line &line, int x, ColorMathLatch &latch) const {
  const Pixel &main = line.above[x];
  const Pixel &sub = line.below[x];
  latch.mainColor = main.color;
  latch.mainVisible = line.windowAbove[x];
  latch.mathEnabled = line.windowBelow[x] && line.io.col.enable[main.source];
  const uint16_t visible = latch.mainVisible ? main.color : 0;
  if (!latch.mathEnabled)
    return visible;

  // A transparent sub screen selects fixed color and suppresses halving.
  const bool transparent = sub.source == Source::COL;
  latch.useSubscreen = line.io.col.blendMode && !transparent;
  latch.halve = line.io.col.halve && latch.mainVisible && !(line.io.col.blendMode && transparent);
  const uint16_t operand = latch.useSubscreen ? sub.color : line.io.col.fixedColor;
  return Blend(line, visible, operand, latch.halve);
}

uint16_t Ppu::CompositeHiresSubPixel(const Line &line, int x, const ColorMathLatch &latch) const {
  const uint16_t visible = latch.mainVisible ? line.below[x].color : 0;
  if (!latch.mathEnabled)
    return visible;
  const uint16_t operand = latch.useSubscreen ? latch.mainColor : line.io.col.fixedColor;
  return Blend(line, visible, operand, latch.halve);
}

// Blend packed BGR555 channels with saturation.

uint16_t Ppu::Blend(const Line& line, uint16_t x, uint16_t y,
                    bool halve) const {
    if (!line.io.col.mathMode) {
        // ADD
        if (!halve) {
            uint32_t sum   = static_cast<uint32_t>(x) + static_cast<uint32_t>(y);
            uint32_t carry = (sum - ((x ^ y) & 0x0421)) & 0x8420;
            return static_cast<uint16_t>((sum - carry) | (carry - (carry >> 5)));
        } else {
            return static_cast<uint16_t>(
                (static_cast<uint32_t>(x) + static_cast<uint32_t>(y)
                 - ((x ^ y) & 0x0421)) >> 1);
        }
    } else {
        // SUBTRACT
        uint32_t diff   = static_cast<uint32_t>(x) - static_cast<uint32_t>(y) + 0x8420;
        uint32_t borrow = (diff - ((x ^ y) & 0x8420)) & 0x8420;
        if (!halve) {
            return static_cast<uint16_t>((diff - borrow) & (borrow - (borrow >> 5)));
        } else {
            return static_cast<uint16_t>(
                (((diff - borrow) & (borrow - (borrow >> 5))) & 0x7BDE) >> 1);
        }
    }
}

} // namespace snes::core
