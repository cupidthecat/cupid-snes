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
uint16_t Color(const Ppu &ppu, unsigned x, unsigned y) {
  const auto pixel = ppu.OutputData()[y * Ppu::OutputWidth + x];
  return uint16_t(((pixel >> 3) & 31) | ((pixel >> 6) & 0x3e0) | ((pixel >> 9) & 0x7c00));
}
std::unique_ptr<Ppu> Fixture(unsigned count, uint8_t y, Ppu::Config config = {}) {
  auto ppu = std::make_unique<Ppu>(config);
  ppu->WriteIO(0x2100, 15);
  ppu->WriteIO(0x2101, 1);
  ppu->WriteIO(0x212c, 16);
  for (unsigned i = 0; i < 128; ++i) {
    ppu->OamData()[i * 4] = 16;
    ppu->OamData()[i * 4 + 1] = i < count ? y : 240;
  }
  ppu->FrameBegin();
  return ppu;
}
void ToLine(Ppu &ppu, unsigned last) {
  for (unsigned line = 1; line <= last; ++line) {
    ppu.SetCurrentLine(uint16_t(line));
    ppu.ScanlineBegin(uint16_t(line));
  }
}
void OverflowClock() {
  for (unsigned count : {32u, 33u}) {
    auto ppu = Fixture(count, 9);
    ToLine(*ppu, 9);
    ppu->SetCurrentHClock(1080);
    Check(!(ppu->ReadIO(0x213e, 0) & 0x40), "Object overflow waits for pattern fetch completion");
    ppu->SetCurrentHClock(1352);
    Check((ppu->ReadIO(0x213e, 0) & 0xc0) == (count == 33 ? 0x40 : 0),
          "The 33rd object sets overflow before the following scanline begins");
    ppu->FrameBegin();
    Check(!(ppu->ReadIO(0x213e, 0) & 0xc0), "The next field clears both object overflow flags");
  }
}
void SeparatePatternWords() {
  auto ppu = Fixture(1, 0);
  ppu->CgramData()[133] = 31;
  ppu->CgramData()[134] = 31 << 5;
  for (unsigned y = 0; y < 8; ++y)
    ppu->VramData()[0x2000 + y] = 0x00ff;
  ppu->SetCurrentHClock(1084);
  ppu->ReadIO(0x213e, 0);
  for (unsigned y = 0; y < 8; ++y) {
    ppu->VramData()[0x2000 + y] = 0xff00;
    ppu->VramData()[0x2008 + y] = 0x00ff;
  }
  ToLine(*ppu, 2);
  ppu->VBlankBegin();
  for (unsigned x = 16; x < 24; ++x) {
    Check(Color(*ppu, x, 0) == 31,
          "Object words fetched four clocks apart can retain mixed planes");
    Check(Color(*ppu, x, 1) == (31 << 5), "The next fetch uses both updated object words");
  }
}
void MembershipAndAttributes() {
  auto ppu = Fixture(1, 9);
  ppu->CgramData()[129] = 31;
  ppu->VramData()[0x2301] = 0x00ff;
  ToLine(*ppu, 8);
  ppu->SetCurrentLine(9);
  ppu->SetCurrentHClock(8);
  ppu->ReadIO(0x213e, 0);
  ppu->OamData()[1] = 240;
  ppu->ScanlineBegin(9);
  ppu->SetCurrentLine(10);
  ppu->ScanlineBegin(10);
  ppu->VBlankBegin();
  for (unsigned x = 16; x < 24; ++x)
    Check(Color(*ppu, x, 9) == 31,
          "Evaluation retains membership while pattern fetching consumes the later attributes");
}
void FirstObjectLatch() {
  auto ppu = Fixture(33, 9);
  ppu->CgramData()[129] = 31;
  ppu->CgramData()[145] = 31 << 5;
  ppu->VramData()[0x2000] = 0x00ff;
  ppu->OamData()[32 * 4 + 3] = 2;
  ToLine(*ppu, 8);
  ppu->SetCurrentLine(9);
  ppu->SetCurrentHClock(4);
  ppu->WriteIO(0x2102, 64);
  ppu->WriteIO(0x2103, 0x80);
  ppu->ScanlineBegin(9);
  ppu->SetCurrentLine(10);
  ppu->ScanlineBegin(10);
  ppu->VBlankBegin();
  for (unsigned x = 16; x < 24; ++x)
    Check(Color(*ppu, x, 9) == 31,
          "Priority rotation written during evaluation waits for the following evaluation line");
}
void OamBusOwnership() {
  auto ppu = Fixture(1, 9);
  ToLine(*ppu, 8);
  ppu->SetCurrentLine(9);
  ppu->SetCurrentHClock(8);
  Check(ppu->ReadIO(0x2138, 0) == 16 && ppu->GetLatch().oamAddress == 0,
        "Evaluation supplies the OAM bus address for CPU reads");
  ppu->SetCurrentHClock(1082);
  Check(ppu->ReadIO(0x2138, 0) == 0 && ppu->GetLatch().oamAddress == 512,
        "Pattern fetching switches the OAM bus to the object's high-table byte");
  ppu->WriteIO(0x2102, 0);
  ppu->WriteIO(0x2104, 0xaa);
  ppu->WriteIO(0x2104, 0xbb);
  Check(ppu->OamData()[512] == 0xbb && ppu->OamData()[0] == 16,
        "Active-display OAM writes follow the current pipeline latch");
  ppu->WriteIO(0x2100, 0x8f);
  ppu->WriteIO(0x2102, 0);
  ppu->WriteIO(0x2104, 0x11);
  ppu->WriteIO(0x2104, 0x22);
  Check(ppu->OamData()[0] == 0x11 && ppu->OamData()[1] == 0x22,
        "Forced blank returns OAM addressing to the CPU port");
}
void BlankedPatternWord() {
  auto ppu = Fixture(1, 0);
  ppu->CgramData()[129] = 31;
  ppu->CgramData()[133] = 31 << 5;
  ppu->VramData()[0x2000] = 0x00ff;
  ppu->SetCurrentHClock(1084);
  ppu->WriteIO(0x2100, 0x8f);
  ppu->WriteIO(0x2116, 8);
  ppu->WriteIO(0x2117, 0x20);
  ppu->WriteIO(0x2118, 0xff);
  ppu->WriteIO(0x2119, 0);
  ppu->SetCurrentHClock(1086);
  ppu->WriteIO(0x2100, 15);
  ToLine(*ppu, 1);
  ppu->VBlankBegin();
  for (unsigned x = 16; x < 24; ++x)
    Check(Color(*ppu, x, 0) == 31,
          "Blanking at the second word preserves the first word and suppresses the new planes");
}
void VblankAddressReset() {
  auto ppu = Fixture(0, 0);
  ppu->WriteIO(0x2102, 0x20);
  ppu->SetCurrentLine(225);
  Check(ppu->GetIO().oamAddress == 0x40, "Vertical blank resets OAM at the scanline boundary");
  ppu->WriteIO(0x2104, 0x55);
  ppu->SetCurrentHClock(2);
  ppu->VBlankBegin();
  Check(ppu->GetIO().oamAddress == 0x41,
        "The later VBlank notification preserves an OAM port access made after the boundary");
}
void LivePalette() {
  auto ppu = Fixture(1, 0);
  ppu->OamData()[0] = 0;
  ppu->CgramData()[129] = 31;
  ppu->VramData()[0x2000] = 0x00ff;
  ppu->SetCurrentLine(1);
  ppu->SetCurrentHClock(62);
  ppu->WriteIO(0x2121, 129);
  ppu->SetCurrentHClock(70);
  ppu->WriteIO(0x2122, 0xe0);
  ppu->SetCurrentHClock(78);
  ppu->WriteIO(0x2122, 3);
  ppu->ScanlineBegin(1);
  ppu->VBlankBegin();
  for (unsigned x = 0; x < 8; ++x)
    Check(Color(*ppu, x, 0) == (x < 5 ? 31 : 31 << 5),
          "Object palette writes change later pixel samples while retaining earlier colors");
}
void ExtendedVramFetches() {
  for (uint32_t bytes : {65536u, 131072u}) {
    auto ppu = Fixture(1, 0, Ppu::Config{3, bytes});
    ppu->WriteIO(0x2101, 4);
    ppu->CgramData()[129] = 31;
    ppu->CgramData()[130] = 31 << 5;
    for (unsigned row = 0; row < 8; ++row) {
      ppu->VramData()[row] = 0x00ff;
      ppu->VramData()[0x8000 + row] = 0xff00;
    }
    ppu->FrameBegin();
    ppu->SetCurrentLine(1);
    ppu->ScanlineBegin(1);
    ppu->VBlankBegin();
    Check(Color(*ppu, 16, 0) == (bytes == 131072 ? 31 << 5 : 31),
          "Object pattern fetches use the selected VRAM bank without aliasing at 128 KiB");
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
  run("OverflowClock", OverflowClock);
  run("SeparatePatternWords", SeparatePatternWords);
  run("MembershipAndAttributes", MembershipAndAttributes);
  run("FirstObjectLatch", FirstObjectLatch);
  run("OamBusOwnership", OamBusOwnership);
  run("BlankedPatternWord", BlankedPatternWord);
  run("VblankAddressReset", VblankAddressReset);
  run("LivePalette", LivePalette);
  run("ExtendedVramFetches", ExtendedVramFetches);
  if (!failures)
    std::puts("Object fetch tests passed");
  return failures ? 1 : 0;
}
