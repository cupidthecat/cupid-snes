#include "snes/core/PowerOnState.hpp"
#include "snes/core/Ppu.hpp"
#include "snes/core/PpuBackground.hpp"
#include "snes/core/PpuObject.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace snes::core;
using snes::core::detail::StateArchive;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class Operation> void Reject(Operation operation, const char *message) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected, message);
}
template <class Device> std::vector<uint8_t> Capture(Device &device) {
  StateArchive archive;
  device.SerializeState(archive);
  return archive.Bytes();
}
template <class Device> void Restore(Device &device, std::span<const uint8_t> bytes) {
  StateArchive archive(bytes);
  device.SerializeState(archive);
  archive.Commit();
}

Ppu::IO Mode(unsigned mode) {
  static const auto modes = [] {
    auto ppu = std::make_unique<Ppu>();
    std::array<Ppu::IO, 8> values;
    for (unsigned mode = 0; mode < values.size(); ++mode) {
      ppu->WriteIO(0x2105, uint8_t(mode));
      ppu->WriteIO(0x2133, 0x40);
      auto &io = values[mode] = ppu->GetIO();
      io.displayDisable = false;
      io.displayBrightness = 15;
      io.mosaic.size = 5;
      io.mosaic.counter = 2;
      io.mode7.a = 0x109;
      io.mode7.b = 0x17;
      io.mode7.c = 0xffe7;
      io.mode7.d = 0x103;
      io.mode7.x = 123;
      io.mode7.y = 71;
      io.mode7.hoffset = 129;
      io.mode7.voffset = 73;
      unsigned layer = 0;
      for (auto *bg : {&io.bg1, &io.bg2, &io.bg3, &io.bg4}) {
        bg->aboveEnable = bg->belowEnable = true;
        bg->tiledataAddress = uint16_t(0x1000 + layer * 0x2000);
        bg->screenAddress = uint16_t(layer * 0x400);
        bg->hoffset = uint16_t(7 + layer * 9);
        bg->voffset = uint16_t(3 + layer * 13);
        bg->mosaicEnable = (layer++ & 1) == 0;
      }
      io.obj.aboveEnable = io.obj.belowEnable = true;
      io.obj.first = 5;
      io.obj.tiledataAddress = 0x4000;
    }
    return values;
  }();
  return modes[mode];
}
void Memory(std::span<uint16_t> words, unsigned salt = 0) {
  for (size_t index = 0; index < words.size(); ++index)
    words[index] = uint16_t(index * 47 + (index >> 7) * 19 + 0x3917 + salt * 31);
}
void Oam(std::span<uint8_t> bytes) {
  std::fill(bytes.begin(), bytes.end(), uint8_t{0});
  for (unsigned index = 0; index < 128; ++index)
    bytes[index * 4 + 1] = 200;
  for (unsigned index = 0; index < 33; ++index) {
    bytes[index * 4] = uint8_t(index * 8);
    bytes[index * 4 + 1] = 9;
    bytes[index * 4 + 2] = uint8_t(index * 17);
    bytes[index * 4 + 3] = uint8_t(((index & 7) << 1) | ((index & 3) << 4) |
                                   (index & 1 ? 0x40 : 0) | (index & 2 ? 0x80 : 0));
  }
  std::fill(bytes.begin() + 512, bytes.end(), uint8_t{0xaa});
}
void SameSamples(const PpuBackground &left, const PpuBackground &right) {
  for (unsigned index = 0; index < 4; ++index)
    for (bool sub : {false, true}) {
      const auto &a = (sub ? left.Samples().below : left.Samples().above)[index];
      const auto &b = (sub ? right.Samples().below : right.Samples().above)[index];
      Check(a.palette == b.palette && a.group == b.group && a.priority == b.priority,
            "Background continuation retains each fetched palette, group and priority");
    }
}

void BackgroundContinuation() {
  for (auto model : {Ppu::RenderModel::Progressive, Ppu::RenderModel::Scanline})
    for (unsigned mode = 0; mode < 8; ++mode)
      for (bool field : {false, true})
        for (unsigned cut : {0u, 2u, 14u, 26u, 56u, 58u, 60u, 70u, 1022u, 1054u, 1078u}) {
          auto io = Mode(mode), otherIo = io;
          std::array<uint16_t, Ppu::VramWords> vram;
          Memory(vram);
          PpuBackground original, restored;
          original.BeginLine(10, field, io, model);
          for (unsigned clock = 0; clock <= cut; clock += 2)
            original.Clock(uint16_t(clock), io, vram);
          const auto saved = Capture(original);
          restored.BeginLine(73, !field, otherIo);
          Restore(restored, saved);
          for (unsigned clock = cut + 2; clock <= 1078; clock += 2) {
            if (clock == 66)
              for (auto &word : vram)
                word ^= 0x53a9;
            if (clock == 138)
              io.bg1.hoffset = otherIo.bg1.hoffset = 0x129;
            Check(original.Clock(uint16_t(clock), io, vram) ==
                      restored.Clock(uint16_t(clock), otherIo, vram),
                  "Background output edges retain their clock after restore");
            SameSamples(original, restored);
          }
          Check(Capture(original) == Capture(restored),
                "Background checkpoints retain tile columns, offsets, mosaic and Mode 7 state");
        }
}

void ObjectContinuation() {
  for (unsigned size = 0; size < 8; ++size)
    for (bool field : {false, true})
      for (unsigned cut : {0u, 8u, 1008u, 1016u, 1080u, 1082u, 1084u, 1086u, 1088u, 1120u, 1200u}) {
        auto io = Mode(0);
        io.obj.baseSize = uint8_t(size);
        io.obj.interlace = field;
        Ppu::Latch latch;
        std::array<uint8_t, Ppu::OamSize> oam;
        std::array<uint16_t, Ppu::VramWords> vram;
        Oam(oam);
        Memory(vram);
        PpuObject original, restored;
        original.BeginLine(10, io.obj);
        for (unsigned clock = 0; clock <= cut; clock += 2)
          original.Clock(uint16_t(clock), field, io, latch, oam, vram);
        auto otherIo = io;
        auto otherLatch = latch;
        Restore(restored, Capture(original));
        for (unsigned clock = cut + 2; clock < 1368; clock += 2) {
          if (clock == 1090)
            for (auto &word : vram)
              word ^= 0x83b7;
          original.Clock(uint16_t(clock), field, io, latch, oam, vram);
          restored.Clock(uint16_t(clock), field, otherIo, otherLatch, oam, vram);
        }
        original.Clock(65535, field, io, latch, oam, vram);
        restored.Clock(65535, field, otherIo, otherLatch, oam, vram);
        Check(
            Capture(original) == Capture(restored) && latch.oamAddress == otherLatch.oamAddress &&
                io.obj.rangeOver == otherIo.obj.rangeOver &&
                io.obj.timeOver == otherIo.obj.timeOver,
            "Sprite state retains selected objects, partial tile words, overflow and OAM feedback");
        original.BeginLine(11, io.obj);
        restored.BeginLine(11, otherIo.obj);
        unsigned visible = 0;
        for (unsigned x = 0; x < 256; ++x) {
          const auto a = original.Pixel(x), b = restored.Pixel(x);
          Check(a.palette == b.palette && a.priority == b.priority,
                "Restored sprite banks decode the same next-line pixels");
          visible += a.palette != 0;
        }
        Check(visible > 0, "The sprite checkpoint fixture decodes visible object pixels");
      }
}

struct Scene {
  std::unique_ptr<Ppu> ppu;
  unsigned mode;
  bool pal;
  unsigned counterCallbacks = 0, displayCallbacks = 0;
  Scene(Ppu::RenderModel model, unsigned mode, unsigned salt = 0)
      : mode(mode), pal((mode & 4) != 0) {
    Ppu::Config config{.revision2 = uint8_t(1 + mode % 3),
                       .vramBytes = (mode & 4) ? 131072u : 65536u,
                       .model = model,
                       .deinterlace = (mode & 2) == 0};
    ppu = std::make_unique<Ppu>(config);
    ppu->SetPal(pal);
    Memory({ppu->VramData(), Ppu::VramStorageWords}, salt);
    Oam({ppu->OamData(), Ppu::OamSize});
    for (unsigned index = 0; index < Ppu::CgramColors; ++index)
      ppu->CgramData()[index] = uint16_t((index * 93 + 0x11 + salt * 131) & 0x7fff);
    ppu->GetIO() = Mode(mode);
    ppu->WriteIO(0x2133, uint8_t(0x40 | ((mode & 1) ? 4 : 0) | ((mode & 2) ? 3 : 0)));
    ppu->SetCounterLatchCallback([this] { ++counterCallbacks; });
    ppu->SetVDispCallback([this](uint16_t) { ++displayCallbacks; });
    ppu->FrameBegin();
  }
  void Clock(unsigned line, unsigned clock) {
    auto &video = *ppu;
    video.SetCurrentHClock(uint16_t(clock));
    if (line > 0 && line < video.VDisp()) {
      if (clock == 26)
        video.WriteIO(0x2105, uint8_t(mode));
      if (clock == 146) {
        video.WriteIO(0x2100, uint8_t(3 + line % 13));
        video.WriteIO(0x2123, uint8_t(line * 3));
      }
      if (clock == 266) {
        video.WriteIO(0x2126, uint8_t(line));
        video.WriteIO(0x2130, uint8_t((line & 3) * 0x51));
        video.WriteIO(0x2132, uint8_t(0x20 | (line & 31)));
      }
      if (clock == 510) {
        video.WriteIO(0x2121, uint8_t(line));
        video.WriteIO(0x2122, uint8_t(line * 37));
        video.WriteIO(0x210d, uint8_t(line * 5));
        video.WriteIO(0x211b, uint8_t(0x13 + line));
        video.WriteIO(0x2104, uint8_t(line * 11));
      }
      if (clock == 512) {
        video.ScanlineBegin(uint16_t(line));
        video.WriteIO(0x2100, uint8_t(15 - line % 13));
      }
      if (clock == 514) {
        video.WriteIO(0x2122, uint8_t(line & 127));
        video.WriteIO(0x211b, 1);
        video.WriteIO(0x210d, uint8_t(0x34 + line));
        video.WriteIO(0x2104, uint8_t(line * 23));
      }
      if (clock == 770)
        video.WriteIO(0x2133, uint8_t(0x40 | ((mode & 1) ? 4 : 0) | ((mode & 2) ? 3 : 0) |
                                      ((line & 1) ? 0x08 : 0)));
      if (clock == 1024)
        video.WriteIO(0x2105, uint8_t((mode + 1) & 7));
    }
    video.ReadIO(0x213e, 0x5a);
  }
  void Line(unsigned line, unsigned after = 0) {
    constexpr unsigned clocks[]{12,  26,   58,   146,  266,  510,  512, 514,
                                770, 1024, 1080, 1084, 1088, 1240, 1368};
    for (auto clock : clocks)
      if (clock > after)
        Clock(line, clock);
  }
  void Field(unsigned from = 1, unsigned after = 0) {
    for (unsigned line = from; line < (pal ? 312u : 262u); ++line) {
      if (line != from || after == 0)
        ppu->SetCurrentLine(uint16_t(line));
      if (line == ppu->VDisp())
        ppu->VBlankBegin();
      Line(line, line == from ? after : 0);
    }
  }
  void StartNext() {
    ppu->SetCurrentLine(0);
    ppu->FrameBegin();
  }
  void Prefix(unsigned cut) {
    Field();
    StartNext();
    for (unsigned line = 1; line <= 4; ++line) {
      ppu->SetCurrentLine(uint16_t(line));
      if (line < 4)
        Line(line);
      else {
        constexpr unsigned clocks[]{12,  26,   58,   146,  266,  510,  512, 514,
                                    770, 1024, 1080, 1084, 1088, 1240, 1368};
        for (auto clock : clocks)
          if (clock <= cut)
            Clock(line, clock);
      }
    }
  }
};

void MachineContinuation() {
  for (auto model : {Ppu::RenderModel::Progressive, Ppu::RenderModel::Scanline})
    for (unsigned mode = 0; mode < 8; ++mode)
      for (unsigned cut : {26u, 512u, 1084u}) {
        Scene original(model, mode), restored(model, mode, 3);
        if (cut == 1084) {
          original.Field();
          original.StartNext();
        }
        original.Prefix(cut);
        Check(original.ppu->FieldID() == (cut == 1084),
              "Whole-PPU checkpoints exercise both interlace field identities");
        const auto saved = Capture(*original.ppu), before = Capture(*restored.ppu);
        auto *vram = restored.ppu->VramData();
        auto *oam = restored.ppu->OamData();
        auto *palette = restored.ppu->CgramData();
        auto *output = restored.ppu->OutputData();
        StateArchive archive(saved);
        restored.ppu->SerializeState(archive);
        Check(Capture(*restored.ppu) == before && restored.counterCallbacks == 0 &&
                  restored.displayCallbacks == 0,
              "PPU validation leaves live memory, output and platform callbacks untouched");
        archive.Commit();
        Check(restored.ppu->VramData() == vram && restored.ppu->OamData() == oam &&
                  restored.ppu->CgramData() == palette && restored.ppu->OutputData() == output &&
                  Capture(*restored.ppu) == saved,
              "Graphics restore retains every mapped allocation and all private rendering state");
        Check(std::equal(original.ppu->OutputData(),
                         original.ppu->OutputData() + Ppu::OutputWidth * Ppu::OutputHeight,
                         restored.ppu->OutputData()) &&
                  original.ppu->VramData()[50000] == restored.ppu->VramData()[50000],
              "A checkpoint retains the prior picture and physical VRAM beyond the visible bank");
        original.ppu->ReadIO(0x2137, 0);
        restored.ppu->ReadIO(0x2137, 0);
        Check(restored.counterCallbacks == 1, "Restored counter reads retain their own callback");
        auto resume = cut;
        if (cut == 512) {
          original.Clock(4, 514);
          restored.Clock(4, 514);
          Check(original.ppu->GetIO().mode7.a == restored.ppu->GetIO().mode7.a &&
                    original.ppu->GetIO().bg1.hoffset == restored.ppu->GetIO().bg1.hoffset &&
                    original.ppu->GetIO().oamAddress == restored.ppu->GetIO().oamAddress &&
                    original.ppu->GetLatch().cgram == restored.ppu->GetLatch().cgram,
                "Register writes resume with the saved scroll, affine, OAM and palette latches");
          resume = 514;
        }
        original.Field(4, resume);
        restored.Field(4, resume);
        Check(Capture(*original.ppu) == Capture(*restored.ppu) && restored.displayCallbacks > 0,
              "Both render models resume cached lines, raster events, interlace and pending "
              "sampling");
        Check(std::any_of(original.ppu->OutputData(),
                          original.ppu->OutputData() + Ppu::OutputWidth * Ppu::OutputHeight,
                          [](uint32_t value) { return value != 0; }),
              "Restored rendering produces a visible frame");
      }
}

void DisplayFeedback() {
  auto source = std::make_unique<Ppu>();
  auto restored = std::make_unique<Ppu>();
  source->CgramData()[0] = 0x11;
  source->WriteIO(0x2100, 15);
  source->WriteIO(0x2133, 4);
  source->FrameBegin();
  const auto field = [](Ppu &ppu, bool feedback) {
    for (uint16_t line = 1; line <= (feedback ? 240 : 239); ++line) {
      ppu.SetCurrentLine(line);
      if (line == 240)
        ppu.VBlankBegin();
      ppu.SetCurrentHClock(512);
      if (line < 240)
        ppu.ScanlineBegin(line);
      ppu.ReadIO(0x213e, 0);
      ppu.SetCurrentHClock(1368);
      ppu.ReadIO(0x213e, 0);
    }
  };
  field(*source, true);
  source->SetCurrentLine(241);
  source->WriteIO(0x2100, 0);
  source->SetCurrentLine(0);
  source->FrameBegin();
  Restore(*restored, Capture(*source));
  for (auto *ppu : {source.get(), restored.get()}) {
    field(*ppu, false);
    ppu->SetCurrentLine(240);
    ppu->VBlankBegin();
  }
  const auto value = restored->OutputData()[0];
  const auto color =
      uint16_t(((value >> 3) & 31) | ((value >> 6) & 0x3e0) | ((value >> 9) & 0x7c00));
  Check(color == 0x11 && Capture(*source) == Capture(*restored),
        "Zero-brightness rendering retains the lower-border feedback words after restore");
}

void ColdAndRejectedStates() {
  for (auto model : {Ppu::RenderModel::Progressive, Ppu::RenderModel::Scanline})
    for (auto entropy : {EntropyMode::None, EntropyMode::Low, EntropyMode::High}) {
      auto source = std::make_unique<Ppu>(Ppu::Config{.model = model});
      auto restored = std::make_unique<Ppu>(Ppu::Config{.model = model});
      PowerOnState state(entropy, 0x12345678);
      source->Power(state);
      Restore(*restored, Capture(*source));
      Check(Capture(*source) == Capture(*restored),
            "Cold graphics states retain entropy and the scanline model's zero VRAM increment");
    }
  Scene source(Ppu::RenderModel::Progressive, 3), target(Ppu::RenderModel::Progressive, 3, 7);
  source.Prefix(512);
  const auto valid = Capture(*source.ppu), before = Capture(*target.ppu);
  auto invalid = valid;
  invalid.back() = 2;
  StateArchive archive(invalid);
  Reject([&] { target.ppu->SerializeState(archive); },
         "A malformed final graphics flag is rejected");
  Reject([&] { archive.Commit(); }, "Late graphics errors cannot publish earlier memory or pixels");
  Check(Capture(*target.ppu) == before,
        "A rejected graphics restore preserves the complete old PPU");
  Scene mismatch(Ppu::RenderModel::Scanline, 3);
  Reject([&] { Restore(*mismatch.ppu, valid); },
         "Graphics checkpoints require matching render hardware");
}
} // namespace

int main() {
  try {
    BackgroundContinuation();
    ObjectContinuation();
    MachineContinuation();
    DisplayFeedback();
    ColdAndRejectedStates();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  std::puts(
      "Graphics state preserves fetched backgrounds, sprite banks, memory and both render models");
  return 0;
}
