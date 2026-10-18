#include "snes/core/gameboy/Video.hpp"

#include <cstdio>
#include <stdexcept>
#include <vector>

using snes::core::gameboy::Video;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Screen final : Video::Output {
  struct Pixel {
    uint64_t clock;
    uint8_t shade;
  };
  uint64_t clock = 0;
  std::vector<Pixel> pixels;
  std::vector<uint64_t> lines, frames;
  void BeginFrame() override {
    frames.push_back(clock);
  }
  void BeginLine() override {
    lines.push_back(clock);
  }
  void Pixel(uint8_t shade) override {
    pixels.push_back({clock, shade});
  }
};
struct System {
  Screen screen;
  Video video{screen};
  System() {
    video.Reset();
    for (unsigned row = 0; row < 8; ++row) {
      video.Write(uint16_t(0x8010 + row * 2), 0xff);
      video.Write(uint16_t(0x8021 + row * 2), 0xff);
      video.Write(uint16_t(0x8030 + row * 2), 0xff);
      video.Write(uint16_t(0x8031 + row * 2), 0xff);
    }
    for (unsigned i = 0; i < 1024; ++i) {
      video.Write(uint16_t(0x9800 + i), 1);
      video.Write(uint16_t(0x9c00 + i), 3);
    }
    video.Write(0xff47, 0xe4);
    video.Write(0xff48, 0xe4);
    video.Write(0xff49, 0x1b);
    video.Write(0xff4b, 7);
  }
  uint8_t Step(unsigned clocks) {
    uint8_t interrupts = 0;
    while (clocks--) {
      ++screen.clock;
      interrupts |= video.Advance(1);
    }
    return interrupts;
  }
};

void StartupAndAccess() {
  System system;
  system.video.Write(0xff41, 0x78);
  system.video.Write(0xff40, 0x91);
  Check(system.video.Read(0xff41) == 0xf8 && system.video.Read(0xff44) == 0,
        "LCD startup begins in mode zero before the first coincidence comparison");
  Check(system.Step(2) == 2 && system.video.Read(0xff41) == 0xfc,
        "The first coincidence comparison raises its enabled interrupt");
  system.Step(76);
  Check(system.video.Blocked() == 2 && system.video.Read(0xfe00) == 0xff,
        "Startup blocks OAM near the end of its shortened search period");
  system.Step(2);
  Check(system.video.Read(0xff41) == 0xff && system.video.Blocked() == 15 &&
            system.video.Read(0x8000) == 0xff && system.video.Read(0x8000, true) == 0,
        "Pixel transfer blocks CPU video access while DMA can read physical memory");
  system.video.Write(0x8000, 0x35);
  system.Step(13);
  Check(system.screen.pixels.size() == 1 && system.screen.pixels[0].clock == 93 &&
            system.screen.pixels[0].shade == 1,
        "The initial tile pipeline emits the first pixel at clock ninety-three");
  system.Step(159);
  Check(system.screen.pixels.size() == 160 && system.video.Blocked() == 0 &&
            (system.video.Read(0xff41) & 3) == 0 && system.video.Read(0x8000) == 0,
        "The final pixel releases video memory and blocked VRAM writes leave RAM intact");
  system.Step(198);
  Check(system.screen.lines.size() == 1 && system.screen.lines[0] == 450,
        "The first LCD line has its shorter startup period");
  system.Step(3);
  Check(system.video.Read(0xff44) == 1 && (system.video.Read(0xff41) & 3) == 0 &&
            system.video.Blocked() == 1,
        "The line counter and OAM access change before the visible mode bits");
  system.Step(1);
  Check((system.video.Read(0xff41) & 3) == 2 && system.video.Blocked() == 3,
        "The next clock exposes mode two");
}

void InterruptEdgesAndFrame() {
  System system;
  system.video.Write(0xff41, 8);
  system.video.Write(0xff40, 0x91);
  Check(system.Step(252) == 0, "Mode-zero interrupts remain low through the last pixel clock");
  Check(system.Step(1) == 2, "The mode-zero interrupt follows the visible mode change");
  system.Step(70218 - 253);
  Check(system.screen.frames.size() == 1 && system.screen.frames[0] == 70218 &&
            system.screen.pixels.size() == 23040 && system.screen.lines.size() == 144,
        "LCD startup produces one complete frame before its first vertical reset");
  system.Step(70224);
  Check(system.screen.frames.size() == 2 && system.screen.frames[1] == 140442 &&
            system.screen.pixels.size() == 46080,
        "Later frames retain the full 154-line period");

  System last;
  last.video.Write(0xff41, 0x40);
  last.video.Write(0xff40, 0x91);
  last.Step(69768);
  Check(last.video.Read(0xff44) == 0 && !(last.video.Read(0xff41) & 4),
        "The final vertical-blank line resets LY before coincidence changes");
  Check(last.Step(6) == 2 && (last.video.Read(0xff41) & 4),
        "The delayed final-line coincidence can request an interrupt");
}

void CoincidencePhases() {
  System incoming;
  incoming.video.Write(0xff45, 1);
  incoming.video.Write(0xff41, 0x40);
  incoming.video.Write(0xff40, 0x91);
  Check(incoming.Step(453) == 0 && incoming.video.Read(0xff44) == 1 &&
            !(incoming.video.Read(0xff41) & 4),
        "Visible LY changes one clock before the incoming coincidence comparison");
  Check(incoming.Step(1) == 2 && (incoming.video.Read(0xff41) & 4),
        "Incoming visible-line coincidence changes with mode two");

  System blank;
  blank.video.Write(0xff45, 144);
  blank.video.Write(0xff41, 0x40);
  blank.video.Write(0xff40, 0x91);
  blank.Step(65660);
  Check(blank.video.Read(0xff44) == 144 && !(blank.video.Read(0xff41) & 4),
        "Vertical blank exposes LY before its delayed comparison");
  Check(blank.Step(2) == 2 && (blank.video.Read(0xff41) & 4),
        "Vertical-blank coincidence precedes its mode-one interrupt");

  System final;
  final.video.Write(0xff45, 153);
  final.video.Write(0xff41, 0x40);
  final.video.Write(0xff40, 0x91);
  final.Step(69767);
  Check(final.Step(1) == 2 && final.video.Read(0xff44) == 0 && (final.video.Read(0xff41) & 4),
        "Final-line coincidence can compare 153 after LY has already reset");
  final.Step(2);
  Check(!(final.video.Read(0xff41) & 4), "The final-line 153 comparison lasts only two clocks");

  System blankOverlap;
  blankOverlap.video.Write(0xff45, 143);
  blankOverlap.video.Write(0xff41, 0x60);
  blankOverlap.video.Write(0xff40, 0x91);
  blankOverlap.Step(65658);
  Check(!(blankOverlap.video.Read(0xff41) & 4) && blankOverlap.Step(2) == 0,
        "The outgoing coincidence source overlaps the short vertical-blank OAM pulse");

  System overlap;
  overlap.video.Write(0xff41, 0x30);
  overlap.video.Write(0xff40, 0x91);
  overlap.Step(70218);
  Check(overlap.Step(5) == 0,
        "The mode-one and mode-two sources overlap across the frame boundary");
}

void WindowEdges() {
  System left;
  left.video.Write(0xff43, 3);
  left.video.Write(0xff4b, 0);
  left.video.Write(0xff40, 0xf1);
  left.Step(110);
  Check(left.screen.pixels[0].clock == 103 && left.screen.pixels[0].shade == 3,
        "A left-edge window with fine scrolling retains its extra startup clock");

  System right;
  right.video.Write(0xff4b, 166);
  right.video.Write(0xff4a, 4);
  right.video.Write(0xff40, 0xf1);
  right.Step(140448);
  Check(right.screen.pixels[0].shade == 1 && right.screen.pixels[800].shade == 3 &&
            right.screen.pixels[23040].shade == 3,
        "The far-right window latch carries into later lines and the next frame");
}

void ObjectBackgroundAndWindowTiming() {
  System hidden;
  hidden.video.Write(0xfe00, 16);
  hidden.video.Write(0xfe01, 0);
  hidden.video.Write(0xfe02, 2);
  hidden.video.Write(0xff43, 3);
  hidden.video.Write(0xff40, 0x93);
  hidden.Step(562);
  Check(hidden.screen.pixels[160].clock == 561 && hidden.screen.pixels[160].shade == 1,
        "An object hidden at X zero retains its full fetch delay with fine scrolling");

  System background;
  background.video.Write(0x8012, 0);
  background.video.Write(0x8013, 0);
  background.video.Write(0x8014, 0xff);
  background.video.Write(0x8015, 0xff);
  background.video.Write(0xfe00, 16);
  background.video.Write(0xfe01, 8);
  background.video.Write(0xfe02, 2);
  background.video.Write(0xff43, 3);
  background.video.Write(0xff40, 0x93);
  background.Step(565);
  background.video.Write(0xff42, 1);
  background.Step(6);
  Check(background.screen.pixels[173].shade == 2,
        "Background bitplanes resume on their own clocks after an object fetch");

  System window;
  window.video.Write(0xfe00, 16);
  window.video.Write(0xfe01, 8);
  window.video.Write(0xfe02, 2);
  window.video.Write(0xff43, 3);
  window.video.Write(0xff4b, 80);
  window.video.Write(0xff40, 0xf3);
  window.Step(557);
  window.video.WindowWrite(true);
  window.video.Write(0xff4b, 7);
  window.Step(1);
  window.video.WindowWrite(false);
  Check(window.screen.pixels.size() == 161 && window.screen.pixels[160].clock == 558,
        "A CPU window-coordinate write spanning its trigger clock suppresses that trigger");
  window.Step(10);
  Check(window.screen.pixels.back().shade == 1,
        "A suppressed window trigger remains missed once the output position advances");
}

void ObjectPlanePhases() {
  for (unsigned change : {554u, 555u, 557u}) {
    System system;
    for (unsigned row = 0; row < 8; ++row)
      system.video.Write(uint16_t(0x8031 + row * 2), 0);
    system.video.Write(0xfe00, 16);
    system.video.Write(0xfe01, 8);
    system.video.Write(0xfe02, 3);
    system.video.Write(0xff43, 3);
    system.video.Write(0xff40, 0x93);
    system.Step(change);
    system.video.Write(0xff40, 0x97);
    system.Step(558 - change);
    const auto expected = change == 554 ? 2 : change == 555 ? 3 : 1;
    Check(system.screen.pixels[160].clock == 558 && system.screen.pixels[160].shade == expected,
          "Object bitplanes sample the selected height at separate fetch clocks");
  }
}

void ObjectQueueTiming() {
  for (bool secondModel : {false, true}) {
    uint64_t digest = 14695981039346656037ull;
    const auto measure = [&](unsigned scroll, const std::vector<unsigned> &positions) {
      System system;
      system.video.Reset(secondModel);
      for (unsigned i = 0; i < positions.size(); ++i) {
        system.video.Write(uint16_t(0xfe00 + i * 4), 17);
        system.video.Write(uint16_t(0xfe01 + i * 4), uint8_t(positions[i]));
      }
      system.video.Write(0xff43, uint8_t(scroll));
      system.video.Write(0xff40, 0x93);
      system.Step(534);
      unsigned clocks = 0;
      while ((system.video.Read(0xff41) & 3) == 3 && clocks < 400) {
        system.Step(1);
        ++clocks;
      }
      Check(clocks < 400, "Object fetches finish within the visible line");
      for (unsigned shift : {0u, 8u})
        digest = (digest ^ uint8_t(clocks >> shift)) * 1099511628211ull;
    };
    for (unsigned scroll = 0; scroll < 8; ++scroll)
      for (unsigned x = 0; x <= 16; ++x)
        measure(scroll, {x});
    for (unsigned scroll = 0; scroll < 8; ++scroll)
      for (unsigned x = 1; x <= 24; ++x)
        for (unsigned gap = 0; gap < 10; ++gap)
          measure(scroll, {x, x + gap});
    for (unsigned scroll = 0; scroll < 8; ++scroll)
      for (unsigned x = 40; x < 56; ++x)
        measure(scroll, {x, x + 1, x + 1});
    Check(digest == 0xd876c89001c053e5ull,
          "Hidden and overlapping objects preserve every background queue timing fixture");
  }
}

void LateEnableAndShutdown() {
  System window;
  window.video.Write(0xff4b, 80);
  window.video.Write(0xff40, 0xf1);
  window.Step(100);
  const auto before = window.screen.pixels.size();
  window.video.Write(0xff4b, 7);
  window.Step(8);
  Check(window.screen.pixels.size() == before + 8 && window.screen.pixels.back().shade == 1,
        "A window coordinate moved behind the output position misses that line's trigger");

  System object;
  object.video.Write(0xfe00, 16);
  object.video.Write(0xfe01, 8);
  object.video.Write(0xfe02, 2);
  object.video.Write(0xff40, 0x91);
  object.Step(548);
  object.video.Write(0xff40, 0x93);
  const auto count = object.screen.pixels.size();
  object.Step(1);
  Check(object.screen.pixels.size() == count + 1 && object.screen.pixels.back().shade == 1,
        "Enabling objects after their horizontal trigger does not fetch them late");

  System coincident;
  coincident.video.Write(0xff41, 0x40);
  coincident.video.Write(0xff40, 0x91);
  coincident.Step(2);
  coincident.video.Write(0xff40, 0);
  Check(coincident.video.Read(0xff41) & 4,
        "LCD shutdown also retains a coincidence flag that was already set");
  coincident.Step(4);
  Check(coincident.video.Write(0xff40, 0x91) == 0 && (coincident.video.Read(0xff41) & 4) &&
            coincident.Step(2) == 0,
        "A retained coincidence source does not retrigger when the LCD restarts");

  object.video.Write(0xff40, 0);
  Check(object.video.Read(0xff44) == 0 && !(object.video.Read(0xff41) & 4),
        "LCD shutdown resets LY while retaining the previous coincidence flag");
}

void LiveScrollFetches() {
  System vertical;
  vertical.video.Write(0x8012, 0);
  vertical.video.Write(0x8013, 0xff);
  vertical.video.Write(0xff40, 0x91);
  vertical.Step(85);
  vertical.video.Write(0xff42, 1);
  vertical.Step(8);
  Check(vertical.screen.pixels[0].shade == 2,
        "The tile bitplanes sample vertical scrolling during their later fetch phases");

  for (const unsigned change : {92u, 93u}) {
    System window;
    window.video.Write(0xff43, 3);
    window.video.Write(0xff4b, 0);
    window.video.Write(0xff40, 0xf1);
    window.Step(change);
    window.video.Write(0xff43, 1);
    window.Step(110 - change);
    Check(window.screen.pixels[0].clock == (change == 92 ? 101 : 109),
          "A left-edge window continues checking fine scrolling during its own startup");
  }

  System horizontal;
  horizontal.video.Write(0xff43, 3);
  horizontal.video.Write(0xff40, 0x91);
  horizontal.Step(80);
  horizontal.video.Write(0xff43, 1);
  horizontal.Step(14);
  Check(horizontal.screen.pixels.size() == 1 && horizontal.screen.pixels[0].clock == 94,
        "Fine scrolling remains live after the transfer mode becomes visible");
}

void ClippedWindowDataAndFetchDelay() {
  System fineWindow;
  for (unsigned row = 0; row < 8; ++row) {
    fineWindow.video.Write(uint16_t(0x8030 + row * 2), 0x55);
    fineWindow.video.Write(uint16_t(0x8031 + row * 2), 0x33);
  }
  fineWindow.video.Write(0xff43, 7);
  fineWindow.video.Write(0xff4b, 0);
  fineWindow.video.Write(0xff40, 0xf1);
  fineWindow.Step(114);
  Check(fineWindow.screen.pixels.size() == 8 && fineWindow.screen.pixels[0].clock == 107,
        "A left-edge window with maximum fine scroll retains its startup timing");
  for (unsigned i = 0; i < 8; ++i)
    Check(fineWindow.screen.pixels[i].shade == ((i + 2) & 3),
          "The window trigger inserts its hidden pixel without consuming tile data");

  for (unsigned wx : {0u, 1u, 166u}) {
    System system;
    for (unsigned row = 0; row < 8; ++row) {
      system.video.Write(uint16_t(0x8030 + row * 2), 0x55);
      system.video.Write(uint16_t(0x8031 + row * 2), 0x33);
    }
    system.video.Write(0x9c01, 2);
    system.video.Write(0xff43, wx == 0 ? 3 : 0);
    system.video.Write(0xff4b, uint8_t(wx));
    system.video.Write(0xff40, 0xf1);
    system.Step(560);
    const unsigned first = wx == 166 ? 160 : 0;
    Check(system.screen.pixels[first].shade == 2,
          "Clipped and carried windows start at the retained horizontal tile offset");
  }

  System finalPixel;
  finalPixel.video.Write(0xff43, 1);
  finalPixel.video.Write(0xff4b, 166);
  finalPixel.video.Write(0xff40, 0xf1);
  finalPixel.Step(710);
  Check(finalPixel.screen.pixels[318].shade == 3 && finalPixel.screen.pixels[319].shade == 0,
        "The carried far-right window with one fine-scroll clock inserts color zero at the edge");

  System object;
  object.video.Write(0xfe00, 16);
  object.video.Write(0xfe01, 8);
  object.video.Write(0xfe02, 2);
  object.video.Write(0xff43, 3);
  object.video.Write(0xff40, 0xf3);
  object.Step(570);
  Check(object.screen.pixels[160].clock == 567,
        "Object alignment after a window starts follows the window fetch position");
}

void ObjectsAndPalettes() {
  System system;
  system.video.Write(0xfe00, 16);
  system.video.Write(0xfe01, 8);
  system.video.Write(0xfe02, 2);
  system.video.Write(0xff40, 0x93);
  system.Step(720);
  Check(system.screen.pixels[0].shade == 1 && system.screen.pixels[160].shade == 2 &&
            system.screen.pixels[160].clock == 558,
        "Object search starts after the warmup line and aligned fetches retain their delay");
  system.video.Write(0xff40, 0);
  system.video.Write(0xff47, 0xff);
  system.video.Write(0xff40, 0x90);
  const auto before = system.screen.pixels.size();
  system.Step(94);
  Check(system.screen.pixels[before].shade == 3,
        "Disabling the background still sends color zero through its palette");
  system.video.Write(0xff47, 0);
  system.Step(1);
  Check(system.screen.pixels.back().shade == 0,
        "Palette writes affect pixels that have not yet left the output pipeline");
}
} // namespace

int main() {
  try {
    StartupAndAccess();
    InterruptEdgesAndFrame();
    CoincidencePhases();
    WindowEdges();
    ObjectBackgroundAndWindowTiming();
    ObjectPlanePhases();
    ObjectQueueTiming();
    LateEnableAndShutdown();
    LiveScrollFetches();
    ClippedWindowDataAndFetchDelay();
    ObjectsAndPalettes();
    std::puts("Handheld LCD timing, tiles, windows, objects and interrupt checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
