#include "snes/core/Ppu.hpp"

#include <cstdio>
#include <memory>
#include <stdexcept>

using snes::core::Ppu;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
unsigned PaletteWord(Ppu &ppu, unsigned address) {
  ppu.WriteIO(0x2121, uint8_t(address));
  unsigned value = ppu.ReadIO(0x213b, 0);
  return (value | (unsigned(ppu.ReadIO(0x213b, 0)) << 8)) & 0x7fff;
}
void VramAddress(Ppu &ppu, unsigned address) {
  ppu.WriteIO(0x2116, uint8_t(address));
  ppu.WriteIO(0x2117, uint8_t(address >> 8));
}
unsigned VramWord(Ppu &ppu) {
  unsigned value = ppu.ReadIO(0x2139, 0);
  return value | (unsigned(ppu.ReadIO(0x213a, 0)) << 8);
}
std::unique_ptr<Ppu> Backgrounds(unsigned mode = 0, unsigned above = 1, unsigned below = 1) {
  auto ppu = std::make_unique<Ppu>();
  ppu->WriteIO(0x2105, uint8_t(mode));
  ppu->WriteIO(0x2108, 4);
  ppu->WriteIO(0x210b, 0x21);
  ppu->WriteIO(0x212c, uint8_t(above));
  ppu->WriteIO(0x212d, uint8_t(below));
  ppu->CgramData()[0] = 0x0555;
  ppu->CgramData()[1] = 0x1111;
  ppu->CgramData()[5] = 0x4567;
  ppu->CgramData()[15] = 0x1234;
  ppu->CgramData()[33] = 0x2345;
  for (unsigned x = 0; x < 1024; ++x) {
    ppu->VramData()[x] = mode == 0 ? 0x0c00 : 0;
    ppu->VramData()[0x400 + x] = mode == 0 ? 0 : 0x0800;
  }
  for (unsigned y = 0; y < 8; ++y) {
    ppu->VramData()[0x1000 + y] = mode == 0 ? 0xffff : 0x00ff;
    ppu->VramData()[0x2000 + y] = 0x00ff;
  }
  ppu->WriteIO(0x2100, 15);
  ppu->FrameBegin();
  ppu->SetCurrentLine(1);
  return ppu;
}
void PaletteOwnership() {
  auto ppu = Backgrounds();
  ppu->SetCurrentHClock(100);
  Check(PaletteWord(*ppu, 5) == 0x1234 && ppu->GetLatch().cgramAddress == 15,
        "Rendering owns palette accesses during the active clock interval");
  ppu->WriteIO(0x2121, 5);
  ppu->WriteIO(0x2122, 0x55);
  ppu->WriteIO(0x2122, 0x2a);
  Check(ppu->CgramData()[15] == 0x2a55 && ppu->CgramData()[0] == 0x0555 &&
            ppu->CgramData()[5] == 0x4567,
        "Active palette writes follow the rendering address rather than the CPU address");
}
void ScreenReadOrder() {
  auto ppu = Backgrounds(0, 1, 2);
  ppu->SetCurrentHClock(100);
  Check(PaletteWord(*ppu, 5) == 0x1234, "The main-screen palette read follows the sub-screen read");
  ppu = Backgrounds(3, 1, 2);
  ppu->WriteIO(0x2130, 1);
  ppu->SetCurrentHClock(100);
  Check(PaletteWord(*ppu, 5) == 0x2345,
        "Direct color on the main screen leaves the sub-screen palette address on the bus");
}
void DirectColorAndBlanking() {
  auto ppu = Backgrounds(3);
  ppu->SetCurrentHClock(100);
  ppu->WriteIO(0x2130, 1);
  ppu->SetCurrentHClock(110);
  Check(PaletteWord(*ppu, 5) == 0x1111,
        "Direct color on both screens retains the preceding palette address");
  ppu->SetCurrentLine(2);
  ppu->SetCurrentHClock(110);
  Check(PaletteWord(*ppu, 5) == 0x0555,
        "Line start reads the backdrop even when later pixels use direct color");
  ppu = Backgrounds();
  ppu->SetCurrentHClock(100);
  ppu->WriteIO(0x2100, 0x8f);
  ppu->SetCurrentHClock(110);
  Check(PaletteWord(*ppu, 5) == 0x4567 && ppu->GetLatch().cgramAddress == 15,
        "Blanking releases the CPU palette port while retaining the rendering address");
}
void PaletteWindowEdges() {
  for (unsigned h : {86u, 88u, 1094u, 1096u, 1360u}) {
    auto ppu = Backgrounds();
    ppu->SetCurrentHClock(uint16_t(h));
    Check(PaletteWord(*ppu, 5) == (h >= 88 && h < 1096 ? 0x1234u : 0x4567u),
          "Palette bus ownership changes at its active-clock boundaries");
  }
  for (bool overscan : {false, true}) {
    for (unsigned line : {0u, 224u, 225u, 239u, 240u}) {
      auto ppu = Backgrounds();
      ppu->SetCurrentHClock(0);
      ppu->WriteIO(0x2133, overscan ? 4 : 0);
      ppu->SetCurrentLine(uint16_t(line));
      ppu->SetCurrentHClock(100);
      const bool owned = line > 0 && line < (overscan ? 240u : 225u);
      Check(PaletteWord(*ppu, 5) == (owned ? 0x1234u : 0x4567u),
            "Palette access uses the visible-line and overscan boundaries");
    }
  }
}
void SharedPalettePhase() {
  auto ppu = std::make_unique<Ppu>();
  ppu->GetLatch().ppu2.mdr = 0;
  ppu->CgramData()[0] = 0x1234;
  ppu->CgramData()[1] = 0x5566;
  ppu->WriteIO(0x2121, 0);
  ppu->WriteIO(0x2122, 0x56);
  Check(ppu->ReadIO(0x213b, 0) == 0x12 && ppu->GetIO().cgramAddress == 1,
        "A read after a low write consumes the shared high-byte phase");
  ppu->WriteIO(0x2122, 7);
  Check(ppu->CgramData()[0] == 0x1234,
        "The next write starts a new color instead of completing the old one");
  ppu->WriteIO(0x2121, 0);
  Check(ppu->ReadIO(0x213b, 0) == 0x34, "CGADD resets the shared phase");
  ppu->WriteIO(0x2122, 0xaa);
  Check(ppu->CgramData()[0] == 0x2a07 && ppu->ReadIO(0x213b, 0) == 0x66,
        "A high write after a low read uses the retained write latch and advances the address");
}
void WindowPaletteReads() {
  auto ppu = Backgrounds(0, 1, 2);
  ppu->WriteIO(0x2123, 2);
  ppu->WriteIO(0x2126, 0);
  ppu->WriteIO(0x2127, 255);
  ppu->WriteIO(0x212e, 1);
  ppu->SetCurrentHClock(100);
  Check(PaletteWord(*ppu, 5) == 0x0555,
        "A masked main layer reads the backdrop after the unmasked sub layer");
  ppu = Backgrounds(3, 1, 2);
  ppu->WriteIO(0x2130, 1);
  ppu->WriteIO(0x2123, 0x20);
  ppu->WriteIO(0x2126, 0);
  ppu->WriteIO(0x2127, 255);
  ppu->WriteIO(0x212f, 2);
  ppu->SetCurrentHClock(100);
  Check(PaletteWord(*ppu, 5) == 0x0555,
        "A masked sub layer leaves the backdrop address when the main layer uses direct color");
  ppu = Backgrounds();
  ppu->WriteIO(0x2130, 0xc0);
  ppu->SetCurrentHClock(100);
  Check(PaletteWord(*ppu, 5) == 0x1234,
        "Color-window black clipping still consumes the selected palette word");
}
void ObjectPaletteReads() {
  auto ppu = std::make_unique<Ppu>();
  ppu->WriteIO(0x2101, 1);
  ppu->WriteIO(0x212c, 16);
  ppu->CgramData()[129] = 0x3456;
  ppu->CgramData()[5] = 0x4567;
  for (unsigned i = 0; i < 128; ++i) {
    ppu->OamData()[i * 4] = 16;
    ppu->OamData()[i * 4 + 1] = i == 0 ? 0 : 240;
  }
  for (unsigned y = 0; y < 8; ++y)
    ppu->VramData()[0x2000 + y] = 0x00ff;
  ppu->WriteIO(0x2100, 15);
  ppu->FrameBegin();
  ppu->SetCurrentLine(1);
  ppu->SetCurrentHClock(126);
  Check(PaletteWord(*ppu, 5) == 0x3456,
        "An object supplies its palette address after evaluation and pattern fetching");
  ppu->WriteIO(0x212c, 0);
  ppu->SetCurrentHClock(130);
  Check(PaletteWord(*ppu, 5) == 0,
        "Disabling the object screen returns the palette lookup to the backdrop");
}
void VramGateAndPrefetch() {
  auto ppu = std::make_unique<Ppu>();
  ppu->VramData()[0x1234] = 0xbeef;
  ppu->WriteIO(0x2115, 0x80);
  VramAddress(*ppu, 0x1234);
  ppu->WriteIO(0x2100, 15);
  Check(VramWord(*ppu) == 0xbeef && VramWord(*ppu) == 0 && ppu->GetIO().vramAddress == 0x1236,
        "Display enable retains the prefetched word while subsequent blocked fetches return zero");
  VramAddress(*ppu, 0x3333);
  ppu->WriteIO(0x2118, 0xaa);
  ppu->WriteIO(0x2119, 0xbb);
  Check(ppu->VramData()[0x3333] == 0 && ppu->GetIO().vramAddress == 0x3334,
        "Scanline-zero VRAM writes are blocked while address increments still occur");
  ppu->WriteIO(0x2100, 0x8f);
  VramAddress(*ppu, 0x9234);
  Check(VramWord(*ppu) == 0xbeef, "Forced blank permits VRAM reads through the physical mirror");
}
void HardwareProfiles() {
  constexpr std::array<unsigned, 4> mapped{0x9234, 0x92a1, 0x93a0, 0x91a4};
  for (uint8_t revision : {1, 2, 3})
    for (uint32_t bytes : {65536u, 131072u}) {
      auto ppu = std::make_unique<Ppu>(Ppu::Config{revision, bytes});
      Check((ppu->ReadIO(0x213f, 0) & 15) == revision,
            "The selected PPU2 revision appears in its status register");
      for (unsigned mapping = 0; mapping < mapped.size(); ++mapping) {
        ppu->Reset();
        ppu->WriteIO(0x2115, uint8_t(0x80 | (mapping << 2)));
        VramAddress(*ppu, 0x9234);
        ppu->WriteIO(0x2118, 0xa5);
        ppu->WriteIO(0x2119, 0x5a);
        const unsigned physical = mapped[mapping] & (bytes / 2 - 1);
        Check(ppu->VramData()[physical] == 0x5aa5,
              "All VRAM translation modes preserve the configured upper address bit");
        VramAddress(*ppu, 0x9234);
        Check(VramWord(*ppu) == 0x5aa5, "VRAM prefetch reads the same translated physical bank");
      }
      ppu->Reset();
      ppu->WriteIO(0x2115, 0x80);
      VramAddress(*ppu, 0);
      ppu->WriteIO(0x2118, 0x34);
      ppu->WriteIO(0x2119, 0x12);
      VramAddress(*ppu, 0x8000);
      ppu->WriteIO(0x2118, 0x78);
      ppu->WriteIO(0x2119, 0x56);
      Check(ppu->VramData()[0] == (bytes == 65536 ? 0x5678 : 0x1234) &&
                ppu->VramData()[0x8000] == (bytes == 65536 ? 0 : 0x5678),
            "The two VRAM banks alias at 64 KiB and remain separate at 128 KiB");
      ppu->Reset(true);
      Check((ppu->ReadIO(0x213f, 0) & 15) == revision &&
                ppu->VramData()[0] == (bytes == 65536 ? 0x5678 : 0x1234),
            "Warm reset retains the video hardware profile and memory mapping");
    }
}
} // namespace
int main() {
  unsigned failures = 0;
  const auto run = [&](const char *name, auto test) {
    try {
      test();
    } catch (const std::exception &error) {
      ++failures;
      std::fprintf(stderr, "%s: %s\n", name, error.what());
    }
  };
  run("PaletteOwnership", PaletteOwnership);
  run("ScreenReadOrder", ScreenReadOrder);
  run("DirectColorAndBlanking", DirectColorAndBlanking);
  run("PaletteWindowEdges", PaletteWindowEdges);
  run("SharedPalettePhase", SharedPalettePhase);
  run("WindowPaletteReads", WindowPaletteReads);
  run("ObjectPaletteReads", ObjectPaletteReads);
  run("VramGateAndPrefetch", VramGateAndPrefetch);
  run("HardwareProfiles", HardwareProfiles);
  if (!failures)
    std::puts("PPU memory port tests passed");
  return failures ? 1 : 0;
}
