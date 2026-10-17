// Compose retained layer samples and screen color math.

// Implements the scanline rendering pipeline:
//   - IO state snapshot per visible scanline
//   - Retained background and object samples from their fetch stages
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
    const char *v = std::getenv("SNES_PROBE_FRAME");
    probeFrame = (v && *v) ? std::atoi(v) : -1;
  }
  return probeFrame;
}

bool OamTraceEnabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char *v = std::getenv("SNES_OAM_TRACE");
    enabled = (v && *v && *v != '0') ? 1 : 0;
  }
  return enabled == 1;
}
} // namespace

// Brightness lookup table

void Ppu::BuildLightTable() {
  // 16 brightness levels × 32768 BGR555 entries
  // Index and output are both plain BGR555 - no channel swapping.
  lightTableData_ = std::make_unique<uint16_t[]>(16 * 32768);
  for (int l = 0; l < 16; l++) {
    lightTable_[l] = lightTableData_.get() + l * 32768;
    double luma = static_cast<double>(l) / 15.0;
    for (uint32_t color = 0; color < 32768; color++) {
      uint32_t r = (color >> 0) & 31;
      uint32_t g = (color >> 5) & 31;
      uint32_t b = (color >> 10) & 31;
      uint32_t ar = static_cast<uint32_t>(luma * r + 0.5);
      uint32_t ag = static_cast<uint32_t>(luma * g + 0.5);
      uint32_t ab = static_cast<uint32_t>(luma * b + 0.5);
      lightTable_[l][color] = static_cast<uint16_t>((ab << 10) | (ag << 5) | ar);
    }
  }
}

// OAM parsing - raw bytes → structured Object array

void Ppu::ParseOam() {
  for (int n = 0; n < 128; n++) {
    auto &obj = objects_[n];

    // Low table: 4 bytes per object at offset n*4
    uint16_t base = static_cast<uint16_t>(n) * 4;
    uint8_t b0 = oam_[base + 0];
    uint8_t b1 = oam_[base + 1];
    uint8_t b2 = oam_[base + 2];
    uint8_t b3 = oam_[base + 3];

    // High table: 2 bits per object at offset 512 + n/4
    uint8_t hiByte = oam_[512 + (n >> 2)];
    uint8_t hiBits = (hiByte >> ((n & 3) * 2)) & 3;

    obj.x = static_cast<uint16_t>(b0) | ((hiBits & 1) << 8); // 9-bit x
    // Convert OAM Y to visible scanline-counter coordinates.
    obj.y = static_cast<uint8_t>(b1 + 1);
    obj.character = b2;
    obj.nameselect = (b3 >> 0) & 1;
    obj.palette = (b3 >> 1) & 7;
    obj.priority = (b3 >> 4) & 3;
    obj.hflip = (b3 >> 6) & 1;
    obj.vflip = (b3 >> 7) & 1;
    obj.size = (hiBits >> 1) & 1;
  }
}

// Frame rendering - renders all cached scanlines at VBlank

void Ppu::RenderFrame() {
  if (lineCount_ == 0)
    return;

  bool hires = false, interlace = displayInterlace_, overscan = false;
  for (int i = 0; i < lineCount_; ++i) {
    const auto &registers = lines_[lineStart_ + i].io;
    hires |= lines_[lineStart_ + i].sampledHires;
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
    std::fprintf(stderr, "[OAM frame=%d] baseSize=%u first=%u\n", gRenderFrame, io_.obj.baseSize,
                 io_.obj.first);
    for (int i = 0; i < 128; i++) {
      const auto &obj = objects_[i];
      uint8_t w = obj.size ? kObjLargeWidth[io_.obj.baseSize] : kObjSmallWidth[io_.obj.baseSize];
      uint8_t h = obj.size ? kObjLargeHeight[io_.obj.baseSize] : kObjSmallHeight[io_.obj.baseSize];
      bool onscreenX = !(obj.x > 256 && static_cast<int>(obj.x) + w - 1 < 512);
      bool onscreenY = obj.y <= 224 || static_cast<uint16_t>(obj.y) + h >= 256;
      if (!onscreenX || !onscreenY)
        continue;
      std::fprintf(
          stderr,
          "  idx=%3d x=%3u y=%3u chr=%3u size=%u w=%2u h=%2u pri=%u pal=%u ns=%u hf=%u vf=%u\n", i,
          obj.x, obj.y, obj.character, obj.size ? 1 : 0, w, h, obj.priority, obj.palette,
          obj.nameselect ? 1 : 0, obj.hflip ? 1 : 0, obj.vflip ? 1 : 0);
    }
  }

  // Render each cached scanline
  for (int i = 0; i < lineCount_; i++) {
    RenderLine(lines_[lineStart_ + i]);
  }

  lineStart_ = 0;
  lineCount_ = 0;
  gRenderFrame++;
}

// RenderLine - the core per-scanline pipeline

void Ppu::RenderLine(Line &line) {
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
  for (const auto &event : events) {
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

  // Layer samples have already crossed their evaluation and fetch stages.
  std::copy_n(line.sampledAbove, 256, line.above);
  std::copy_n(line.sampledBelow, 256, line.below);

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

// DirectColor - BG1 direct color mode (modes 3/4)

uint16_t Ppu::DirectColor(uint8_t paletteIndex, uint8_t paletteColor) {
  // paletteIndex = bgr bits from palette number (3 bits)
  // paletteColor = BBGGGRRR (8-bit pixel value)
  return static_cast<uint16_t>(
      ((paletteColor << 2) & 0x001C) + ((paletteIndex << 1) & 0x0002) +  // R
      ((paletteColor << 4) & 0x0380) + ((paletteIndex << 5) & 0x0040) +  // G
      ((paletteColor << 7) & 0x6000) + ((paletteIndex << 10) & 0x1000)); // B
}

// RenderWindowColor - compute window mask for color math
//
// mask: 0=always on, 1=inside window only, 2=outside window only, 3=never
// output[x] = true means color math/display is ENABLED at this pixel

void Ppu::RenderWindowColor(const Line &line, const WindowColor &wc, uint8_t mask,
                            bool output[256]) {
  switch (mask) {
  case 0: // Always
    for (int x = 0; x < 256; x++)
      output[x] = true;
    return;
  case 3: // Never
    for (int x = 0; x < 256; x++)
      output[x] = false;
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
      case 0:
        windowRegion[x] = one | two;
        break;
      case 1:
        windowRegion[x] = one & two;
        break;
      case 2:
        windowRegion[x] = one ^ two;
        break;
      case 3:
        windowRegion[x] = !(one ^ two);
        break;
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

uint16_t Ppu::Blend(const Line &line, uint16_t x, uint16_t y, bool halve) const {
  if (!line.io.col.mathMode) {
    // ADD
    if (!halve) {
      uint32_t sum = static_cast<uint32_t>(x) + static_cast<uint32_t>(y);
      uint32_t carry = (sum - ((x ^ y) & 0x0421)) & 0x8420;
      return static_cast<uint16_t>((sum - carry) | (carry - (carry >> 5)));
    } else {
      return static_cast<uint16_t>(
          (static_cast<uint32_t>(x) + static_cast<uint32_t>(y) - ((x ^ y) & 0x0421)) >> 1);
    }
  } else {
    // SUBTRACT
    uint32_t diff = static_cast<uint32_t>(x) - static_cast<uint32_t>(y) + 0x8420;
    uint32_t borrow = (diff - ((x ^ y) & 0x8420)) & 0x8420;
    if (!halve) {
      return static_cast<uint16_t>((diff - borrow) & (borrow - (borrow >> 5)));
    } else {
      return static_cast<uint16_t>((((diff - borrow) & (borrow - (borrow >> 5))) & 0x7BDE) >> 1);
    }
  }
}

} // namespace snes::core
