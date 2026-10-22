#include "PpuScanlineProgram.hpp"
#include "ppu/OffsetTileProgram.hpp"
#include "snes/core/Emulator.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace snes::core;

namespace {
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

std::vector<uint8_t> BrightnessProgram(bool dynamic) {
  std::vector<uint8_t> code{0x78};
  const auto port = [&](uint16_t address, uint8_t value) {
    code.insert(code.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  port(0x2100, 0x80);
  port(0x2121, 0);
  port(0x2122, 0x1f);
  port(0x2122, 0);
  port(0x2100, 15);
  if (dynamic) {
    const auto loop = code.size();
    port(0x2100, 15);
    code.insert(code.end(), {0xa2, 11, 0xca, 0xd0, 0xfd});
    port(0x2100, 4);
    code.insert(code.end(), {0xa2, 9, 0xca, 0xd0, 0xfd});
    const auto offset = int(loop) - int(code.size()) - 2;
    code.insert(code.end(), {0x80, uint8_t(offset)});
  } else {
    code.insert(code.end(), {0x80, 0xfe});
  }
  std::vector<uint8_t> rom(32768, 0xea);
  std::copy(code.begin(), code.end(), rom.begin());
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  const uint8_t header[]{0x20, 0, 5, 0, 1, 0, 0, 255, 255, 0, 0};
  std::copy(std::begin(header), std::end(header), rom.begin() + 0x7fd5);
  rom[0x7ffc] = 0;
  rom[0x7ffd] = 0x80;
  return rom;
}

struct Output final : IVideoOutput {
  VideoFrame frame;
  void Present(const VideoFrame &value) override {
    frame = value;
  }
  uint64_t Digest() const {
    uint64_t value = 14695981039346656037ull;
    for (const auto pixel : frame.pixels) {
      const uint16_t color =
          uint16_t((((pixel >> 0) & 255) >> 3) << 10 | (((pixel >> 8) & 255) >> 3) << 5 |
                   (((pixel >> 16) & 255) >> 3));
      for (unsigned shift : {0u, 8u})
        value = (value ^ ((color >> shift) & 255)) * 1099511628211ull;
    }
    return value;
  }
};

void ScanlineBrightness() {
  for (bool dynamic : {false, true}) {
    Emulator::Config config;
    config.entropyMode = EntropyMode::None;
    config.entropySeed = 0;
    auto machine = std::make_unique<Emulator>(config);
    Check(machine->LoadCartridge(BrightnessProgram(dynamic)), "The scanline video fixture loads");
    Output output;
    machine->AttachVideoOutput(&output);
    for (unsigned frame = 0; frame < 3; ++frame)
      machine->StepFrame({false, false, false});
    Check(output.frame.width == 256 && output.frame.height == 224 &&
              output.frame.pixels.size() == 256 * 224,
          "Scanline rendering retains the complete visible frame");
    Check(output.Digest() == (dynamic ? 0xf5030600487dab25ull : 0x3d25fafee3e02325ull),
          "Brightness is sampled once for each complete scanline");
  }
}

void RetainedMemoryAndActiveOam() {
  auto ppu = std::make_unique<Ppu>(Ppu::Config{.model = Ppu::RenderModel::Scanline});
  for (unsigned object = 0; object < 128; ++object)
    Check(ppu->OamData()[object * 4 + 1] == 0xff,
          "Cold scanline sprite positions precede the first visible row");
  ppu->WriteIO(0x2115, 0x80);
  ppu->WriteIO(0x2116, 0);
  ppu->WriteIO(0x2117, 0);
  ppu->WriteIO(0x2118, 0x34);
  ppu->WriteIO(0x2119, 0x12);
  ppu->WriteIO(0x2121, 3);
  ppu->WriteIO(0x2122, 0x67);
  ppu->WriteIO(0x2122, 0x45);
  ppu->WriteIO(0x2102, 0);
  ppu->WriteIO(0x2103, 0);
  ppu->WriteIO(0x2104, 0x5a);
  ppu->WriteIO(0x2104, 0xa5);
  ppu->Reset(true);
  Check(ppu->VramData()[0] == 0x1234 && ppu->CgramData()[3] == 0x4567 &&
            ppu->OamData()[0] == 0x5a && ppu->OamData()[1] == 0xa5,
        "Scanline warm reset retains video, palette and sprite memory");
  Check(ppu->DisplayDisable() && ppu->Brightness() == 0 && ppu->GetLatch().ppu1.mdr == 0 &&
            ppu->GetLatch().ppu2.mdr == 0 && !ppu->GetIO().vramIncrementMode &&
            ppu->GetIO().vramIncrementSize == 0,
        "Scanline reset restores register and latch defaults independently of memory");
  ppu->WriteIO(0x2100, 15);
  ppu->WriteIO(0x2104, 0x66);
  ppu->WriteIO(0x2104, 0x77);
  Check(ppu->OamData()[0] == 0x5a && ppu->OamData()[1] == 0xa5 && ppu->OamData()[0x218] == 0x77,
        "Active scanline sprite writes use the fixed high-table destination");
  ppu->Reset();
  Check(ppu->VramData()[0] == 0 && ppu->CgramData()[3] == 0 && ppu->OamData()[0] == 0 &&
            ppu->OamData()[0x218] == 0,
        "Cold scanline power clears retained graphics memory");
}

void HiresBackdropMath() {
  for (bool blend : {false, true}) {
    for (bool subtract : {false, true}) {
      for (bool halve : {false, true}) {
        auto ppu = std::make_unique<Ppu>(Ppu::Config{.model = Ppu::RenderModel::Scanline});
        ppu->WriteIO(0x2121, 0);
        ppu->WriteIO(0x2122, 0x21);
        ppu->WriteIO(0x2122, 0x04);
        for (uint8_t color : {0x25, 0x45, 0x85})
          ppu->WriteIO(0x2132, color);
        ppu->WriteIO(0x2130, blend ? 2 : 0);
        ppu->WriteIO(0x2131, uint8_t(0x20 | (subtract ? 0x80 : 0) | (halve ? 0x40 : 0)));
        ppu->WriteIO(0x2133, 8);
        ppu->WriteIO(0x2100, 15);
        ppu->FrameBegin();
        ppu->ScanlineBegin(1);
        ppu->RenderFrame();
        const uint32_t channel = subtract ? 0 : blend ? 2 : halve ? 3 : 6;
        const uint32_t expanded = (channel << 3) | (channel >> 2);
        const uint32_t expected = 0xff000000u | expanded | (expanded << 8) | (expanded << 16);
        Check(ppu->FrameWidth() == 512, "Pseudo hires retains both dots");
        for (unsigned x = 0; x < 512; ++x)
          Check(ppu->OutputData()[x] == expected,
                "Hires blending uses the backdrop operand and suppresses transparent halving");
      }
    }
  }
}

void CachedPaletteAndGraphicsLifetime() {
  auto ppu = std::make_unique<Ppu>(Ppu::Config{.model = Ppu::RenderModel::Scanline});
  ppu->WriteIO(0x2115, 0x80);
  ppu->WriteIO(0x2116, 0);
  ppu->WriteIO(0x2117, 0x10);
  for (unsigned row = 0; row < 8; ++row) {
    ppu->WriteIO(0x2118, 0xff);
    ppu->WriteIO(0x2119, 0);
  }
  ppu->WriteIO(0x210b, 1);
  ppu->WriteIO(0x212c, 1);
  const auto palette = [&](uint16_t color) {
    ppu->WriteIO(0x2121, 1);
    ppu->WriteIO(0x2122, uint8_t(color));
    ppu->WriteIO(0x2122, uint8_t(color >> 8));
  };
  palette(0x001f);
  ppu->WriteIO(0x2100, 15);
  ppu->FrameBegin();
  ppu->SetCurrentLine(1);
  ppu->SetCurrentHClock(512);
  ppu->ScanlineBegin(1);
  ppu->WriteIO(0x2100, 5);
  ppu->SetCurrentHClock(1100);
  palette(0x7c00);
  ppu->WriteIO(0x2100, 15);
  ppu->ScanlineBegin(2);
  ppu->SetCurrentHClock(514);
  ppu->WriteIO(0x2100, 0x80);
  ppu->WriteIO(0x2116, 0);
  ppu->WriteIO(0x2117, 0x10);
  for (unsigned row = 0; row < 8; ++row) {
    ppu->WriteIO(0x2118, 0);
    ppu->WriteIO(0x2119, 0);
  }
  ppu->WriteIO(0x2100, 15);
  ppu->ScanlineBegin(3);
  ppu->RenderFrame();
  for (unsigned x = 0; x < 256; ++x) {
    Check(ppu->OutputData()[x] == 0xff000052,
          "Display writes at the sampling edge belong to the retained line");
    Check(ppu->OutputData()[Ppu::OutputWidth + x] == 0xffff0000,
          "Later palette writes affect the next line without changing the cached palette");
    Check(ppu->OutputData()[2 * Ppu::OutputWidth + x] == 0xff000000,
          "Graphics writes flush earlier lines before changing their shared tile data");
  }
}

void ScanlineVramCapacity() {
  auto ppu =
      std::make_unique<Ppu>(Ppu::Config{.vramBytes = 131072, .model = Ppu::RenderModel::Scanline});
  ppu->WriteIO(0x2115, 0x80);
  ppu->WriteIO(0x2116, 0);
  ppu->WriteIO(0x2117, 0x80);
  ppu->WriteIO(0x2118, 0xcd);
  ppu->WriteIO(0x2119, 0xab);
  Check(ppu->VramData()[0] == 0xabcd && ppu->VramData()[0x8000] == 0,
        "Scanline video retains its physical VRAM mirror with expanded memory selected");
}

void ModelStatusRegisters() {
  for (auto model : {Ppu::RenderModel::Progressive, Ppu::RenderModel::Scanline}) {
    for (uint8_t revision : {1, 2, 3}) {
      auto ppu = std::make_unique<Ppu>(Ppu::Config{.revision2 = revision, .model = model});
      ppu->WriteIO(0x211b, 0x10);
      ppu->WriteIO(0x211b, 0);
      ppu->WriteIO(0x211c, 0);
      ppu->WriteIO(0x211c, 1);
      Check(ppu->ReadIO(0x2134, 0) == 0x10, "The multiplier drives PPU1 bus bit four");
      const bool scanline = model == Ppu::RenderModel::Scanline;
      Check(ppu->ReadIO(0x213e, 0) == (scanline ? 1 : 0x11),
            "Scanline status clears reserved bits while progressive status retains bus bit four");
      Check((ppu->ReadIO(0x213f, 0) & 15) == (scanline ? 3 : revision),
            "Scanline status reports its fixed revision independently of the hardware selection");
    }
  }
}

void GraphicsPrograms() {
  constexpr uint64_t staticDigest[]{
      0x84d8252d0873d5b8ull, 0x3486ddb8e5b60f21ull, 0x3389c3e398f5ee12ull, 0x2422d8fc9427e879ull,
      0x6996ae415a89e31dull, 0xec7979ab01bfdb45ull, 0xa3dc5a4bb7737f6full, 0xfacd29f73124ff25ull};
  constexpr uint64_t dynamicDigest[2][8]{
      {0xbc5c88dc075c7088ull, 0x9cd105c5f7154bbdull, 0xbfb603399d7d9923ull, 0xabeb7193574faa8dull,
       0xba2486e4d294e986ull, 0x0ff3e22409176394ull, 0xe04febeadf63f0aaull, 0xca85cd92de102c01ull},
      {0x06b2f0cbd996a2b8ull, 0x95460179c1060591ull, 0xfbb1e165d2d863c2ull, 0x1177cfc4abf66b19ull,
       0x8cc62990cf7c40d3ull, 0xb7889a9f837489b9ull, 0xafc2f5920d9ac453ull, 0x010833441a791725ull}};
  for (unsigned model = 0; model < 2; ++model) {
    for (unsigned scene = 0; scene < 18; ++scene) {
      const bool objects = scene == 8, extended = scene == 9, dynamic = scene >= 10;
      const unsigned mode = scene < 8 ? scene : objects ? 0 : extended ? 7 : scene - 10;
      Emulator::Config config;
      config.entropyMode = EntropyMode::None;
      config.entropySeed = 0;
      config.ppu.model = model ? Ppu::RenderModel::Scanline : Ppu::RenderModel::Progressive;
      auto machine = std::make_unique<Emulator>(config);
      Check(machine->LoadCartridge(PpuGraphicsProgram(mode, objects, dynamic, extended)),
            "The graphics program loads its own DMA data");
      Output output;
      machine->AttachVideoOutput(&output);
      for (unsigned frame = 0; frame < 4; ++frame)
        machine->StepFrame({false, false, false});
      const uint64_t expected = objects   ? 0x4608963220c0102cull
                                : dynamic ? dynamicDigest[model][mode]
                                          : staticDigest[mode];
      Check(output.frame.width == (mode == 5 || mode == 6 ? 512 : 256) &&
                output.frame.height == 224 && output.Digest() == expected,
            "Normal CPU execution retains each background mode, object row and scroll transition");
    }
  }
}

void IndependentMode7Mosaic() {
  constexpr uint64_t digests[3][4]{
      {0xfacd29f73124ff25ull, 0xbdbb459b32e92325ull, 0xfacd29f73124ff25ull, 0xbdbb459b32e92325ull},
      {0xfacd29f73124ff25ull, 0xfacd29f73124ff25ull, 0xbdbb459b32e92325ull, 0xbdbb459b32e92325ull},
      {0xfacd29f73124ff25ull, 0xce5070d3bf203929ull, 0xb1215d6a5d373a81ull, 0xbdbb459b32e92325ull}};
  constexpr uint64_t progressiveDigests[2][2]{{0xc6acc36ee9690f25ull, 0xe6697bcd06edcb25ull},
                                              {0x9d1a87d5292f60a5ull, 0x9844ea08d3e84749ull}};
  for (unsigned layers = 1; layers <= 3; ++layers) {
    for (unsigned mosaic = 0; mosaic < 4; ++mosaic) {
      for (bool pal : {false, true}) {
        auto rom = PpuGraphicsProgram(7, false, false, true, 0x30 | mosaic, layers);
        rom[0x7fd9] = pal ? 2 : 1;
        for (auto model : {Ppu::RenderModel::Progressive, Ppu::RenderModel::Scanline}) {
          Emulator::Config config;
          config.entropyMode = EntropyMode::None;
          config.entropySeed = 0;
          config.ppu.model = model;
          auto machine = std::make_unique<Emulator>(config);
          Check(machine->LoadCartridge(rom), "The independent mosaic guest loads");
          Output output;
          machine->AttachVideoOutput(&output);
          auto expected = digests[layers - 1][mosaic];
          if (model == Ppu::RenderModel::Progressive && layers > 1 && (mosaic == 1 || mosaic == 2))
            expected = progressiveDigests[layers - 2][mosaic - 1];
          for (bool warm : {false, true}) {
            if (warm)
              Check(machine->Reset(), "The independent mosaic guest resets");
            for (unsigned field = 0; field < 4; ++field)
              machine->StepFrame({false, false, false});
            Check(output.frame.width == 256 && output.frame.height == 224 &&
                      output.Digest() == expected,
                  "Mode 7 mosaic enables retain the selected renderer's coordinate sampling");
          }
        }
      }
    }
  }
}

void OffsetTileColumns() {
  for (unsigned mode : {2u, 4u}) {
    for (unsigned layer : {0u, 1u}) {
      for (bool large : {false, true}) {
        for (auto offset :
             {PpuOffsetKind::None, PpuOffsetKind::Horizontal, PpuOffsetKind::Vertical}) {
          for (bool pal : {false, true}) {
            const auto rom = PpuOffsetTileProgram(mode, layer, large, offset, pal);
            for (auto model : {Ppu::RenderModel::Progressive, Ppu::RenderModel::Scanline}) {
              Emulator::Config config;
              config.entropyMode = EntropyMode::None;
              config.entropySeed = 0;
              config.ppu.model = model;
              auto machine = std::make_unique<Emulator>(config);
              Check(machine->LoadCartridge(rom), "The offset-tile column guest loads");
              Output output;
              machine->AttachVideoOutput(&output);
              const auto expected = !large                                ? 0x3d25fafee3e02325ull
                                    : offset == PpuOffsetKind::None       ? 0x17e42fff5975e325ull
                                    : model == Ppu::RenderModel::Scanline ? 0xfd74dc3bf015e325ull
                                                                          : 0x4a7616d88fd5e325ull;
              for (bool warm : {false, true}) {
                if (warm)
                  Check(machine->Reset(), "The offset-tile column guest resets");
                for (unsigned field = 0; field < 4; ++field)
                  machine->StepFrame({false, false, false});
                Check(output.frame.width == 256 && output.frame.height == 224 &&
                          output.Digest() == expected,
                      "Offset tiles preserve the first column and the selected renderer's table "
                      "phase");
              }
            }
          }
        }
      }
    }
  }
}
} // namespace

int main() {
  try {
    ScanlineBrightness();
    RetainedMemoryAndActiveOam();
    HiresBackdropMath();
    CachedPaletteAndGraphicsLifetime();
    ScanlineVramCapacity();
    ModelStatusRegisters();
    GraphicsPrograms();
    IndependentMode7Mosaic();
    OffsetTileColumns();
    std::puts("Scanline video checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
