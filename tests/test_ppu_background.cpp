#include "snes/core/Ppu.hpp"
#include "snes/core/PpuBackground.hpp"
#include <array>
#include <cstdint>
#include <functional>

#include <cstdio>
#include <memory>
#include <stdexcept>

using snes::core::Ppu;
using snes::core::PpuBackground;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

uint16_t Color(const Ppu& ppu, int x, int y) {
    const auto pixel = ppu.OutputData()[y * Ppu::OutputWidth + x];
    return uint16_t(((pixel >> 3) & 31) | ((pixel >> 6) & 0x3e0) | ((pixel >> 9) & 0x7c00));
}

struct FetchFixture {
    Ppu::IO io;
    PpuBackground pipeline;
    std::array<uint16_t, Ppu::VramWords> vram{};

    FetchFixture(bool fourBits = false) {
        io.bgMode = fourBits ? 1 : 0;
        io.bg1.tileMode = fourBits ? Ppu::TileMode::BPP4 : Ppu::TileMode::BPP2;
        io.bg1.priority[0] = 8;
        io.bg1.aboveEnable = true;
        io.bg1.tiledataAddress = 0x1000;
    }

    auto Run(uint16_t line, const std::function<void(unsigned)>& change = {}) {
        std::array<unsigned, 256> pixels{};
        pipeline.BeginLine(line, false, io);
        for (unsigned h = 0; h < 1080; h += 2) {
            if (change)
                change(h);
            if (pipeline.Clock(h, io, vram)) {
                const auto& p = pipeline.Samples().above[0];
                pixels[(h - 58) / 4] = p.priority ? p.palette : 0;
            }
        }
        return pixels;
    }
};

void NameAndPlaneLatches() {
    FetchFixture names;
    for (unsigned y = 0; y < 8; ++y) {
        names.vram[0x1000 + y] = 0x00ff;
        names.vram[0x1008 + y] = 0xff00;
    }
    auto first = names.Run(1, [&](unsigned h) {
        if (h == 14)
            names.vram[0] = 1;
    });
    auto next = names.Run(2);
    for (unsigned x = 0; x < 8; ++x)
        Check(first[x] == 1 && next[x] == 2,
              "A name fetch retains its character until the next scanline");

    FetchFixture planes(true);
    for (unsigned y = 0; y < 8; ++y)
        planes.vram[0x1000 + y] = 0x00ff;
    auto mixed = planes.Run(1, [&](unsigned h) {
        if (h == 26)
            for (unsigned y = 0; y < 8; ++y) {
                planes.vram[0x1000 + y] = 0xff00;
                planes.vram[0x1008 + y] = 0x00ff;
            }
    });
    for (unsigned x = 0; x < 256; ++x)
        Check(mixed[x] == (x < 8 ? 5u : 6u),
              "A CPU update between plane fetches leaves the earlier planes latched");
}

void ScrollAndCharacterWrap() {
    FetchFixture scroll;
    for (unsigned x = 0; x < 32; ++x)
        scroll.vram[x] = x & 1;
    for (unsigned y = 0; y < 8; ++y) {
        scroll.vram[0x1000 + y] = 0x00ff;
        scroll.vram[0x1008 + y] = 0xff00;
    }
    const auto pixels = scroll.Run(1, [&](unsigned h) {
        if (h == 600)
            scroll.io.bg1.hoffset = 8;
    });
    for (unsigned x = 0; x < 256; ++x)
        Check(pixels[x] == 1 + ((x / 8 + (x >= 152)) & 1),
              "Scroll changes wait for the next name fetch");

    FetchFixture wrap(true);
    wrap.io.bg1.tileSize = true;
    for (unsigned x = 0; x < 32; ++x)
        wrap.vram[x] = 1023;
    wrap.vram[0x1000 + 15 * 16 + 1] = 0x00ff;
    wrap.vram[0x1000 + 16 * 16 + 1] = 0xff00;
    const auto wrapped = wrap.Run(9);
    for (unsigned x = 0; x < 16; ++x)
        Check(wrapped[x] == (x < 8 ? 1u : 2u),
              "Large characters wrap ten bits before the pattern base is added");
}

void MosaicReload() {
    auto ppu = std::make_unique<Ppu>();
    ppu->SetCurrentLine(7);
    ppu->SetCurrentHClock(600);
    ppu->WriteIO(0x2106, 0x31);
    Check(ppu->GetIO().mosaic.counter == 5,
          "Enabling mosaic loads size plus one at the write clock");
    ppu->SetCurrentLine(8);
    Check(ppu->GetIO().mosaic.counter == 4, "The next scanline consumes the first mosaic count");
    ppu->WriteIO(0x2106, 0x72);
    Check(ppu->GetIO().mosaic.counter == 4,
          "Size and layer changes preserve an active mosaic phase");
    ppu->WriteIO(0x2106, 0x70);
    Check(ppu->GetIO().mosaic.counter == 4,
          "Disabling mosaic retains the running vertical countdown");
}

void ResetRetainsColumns() {
    FetchFixture fixture;
    fixture.io.bgMode = 1;
    fixture.io.bg1.aboveEnable = false;
    auto& bg = fixture.io.bg2;
    bg.aboveEnable = true;
    bg.tileMode = Ppu::TileMode::BPP4;
    bg.priority[1] = 8;
    bg.hoffset = 5;
    bg.screenAddress = 0x400;
    bg.tiledataAddress = 0x1000;
    for (unsigned x = 0; x < 32; ++x)
        fixture.vram[0x400 + x] = 0x2400;
    for (unsigned y = 0; y < 8; ++y)
        fixture.vram[0x1000 + y] = 0x00ff;
    fixture.Run(1);
    fixture.pipeline.Reset();
    fixture.io.bgMode = 7;
    bg.tileMode = Ppu::TileMode::Inactive;
    fixture.pipeline.BeginLine(1, false, fixture.io);
    for (unsigned h = 0; h < 1080; h += 2)
        if (fixture.pipeline.Clock(h, fixture.io, fixture.vram)) {
            const auto& pixel = fixture.pipeline.Samples().above[1];
            const unsigned x = (h - 58) / 4;
            Check((pixel.priority ? pixel.palette : 0) == (x >= 251 && x <= 253 ? 17 : 0),
                  "Processor reset retains fetched pattern words while restarting the shift phase");
        }
}

void MidlineOutputMode() {
    auto ppu = std::make_unique<Ppu>();
    ppu->WriteIO(0x2100, 15);
    ppu->WriteIO(0x210b, 0x21);
    ppu->WriteIO(0x212c, 1);
    ppu->WriteIO(0x212d, 2);
    ppu->CgramData()[1] = 31;
    ppu->CgramData()[33] = 31 << 5;
    for (unsigned y = 0; y < 8; ++y) {
        ppu->VramData()[0x1000 + y] = 0x00ff;
        ppu->VramData()[0x2000 + y] = 0x00ff;
    }
    ppu->FrameBegin();
    ppu->SetCurrentLine(1);
    ppu->ScanlineBegin(1);
    ppu->SetCurrentHClock(570);
    ppu->WriteIO(0x2133, 8);
    ppu->VBlankBegin();
    Check(ppu->FrameWidth() == 512, "A midline hires write expands the output for that field");
    Check(Color(*ppu, 254, 0) == 31 && Color(*ppu, 255, 0) == 31 &&
              Color(*ppu, 256, 0) == (31 << 5) && Color(*ppu, 257, 0) == 31,
          "Pseudo-hires changes screen sampling at its own pixel without altering earlier dots");
}

void LiveObjectPriority() {
    auto ppu = std::make_unique<Ppu>();
    ppu->WriteIO(0x2100, 15);
    ppu->WriteIO(0x2105, 3);
    ppu->WriteIO(0x210b, 1);
    ppu->WriteIO(0x2101, 2);
    ppu->WriteIO(0x212c, 0x11);
    ppu->CgramData()[1] = 31;
    ppu->CgramData()[129] = 31 << 5;
    for (unsigned x = 0; x < 32; ++x)
        ppu->VramData()[x] = 0x2000;
    for (unsigned y = 0; y < 8; ++y) {
        ppu->VramData()[0x1000 + y] = 0x00ff;
        ppu->VramData()[0x4000 + y] = 0x00ff;
        ppu->VramData()[0x4010 + y] = 0x00ff;
    }
    for (unsigned i = 0; i < 128; ++i)
        ppu->OamData()[i * 4 + 1] = 240;
    ppu->OamData()[0] = 128;
    ppu->OamData()[1] = 0;
    ppu->OamData()[3] = 0x20;
    ppu->OamData()[512] = 2;
    ppu->FrameBegin();
    ppu->SetCurrentLine(1);
    ppu->ScanlineBegin(1);
    ppu->SetCurrentHClock(570);
    ppu->WriteIO(0x2105, 0);
    ppu->VBlankBegin();
    Check(Color(*ppu, 127, 0) == 31 && Color(*ppu, 128, 0) == (31 << 5) &&
              Color(*ppu, 143, 0) == (31 << 5) && Color(*ppu, 144, 0) == 31,
          "A mode change updates object priority while prefetched background priority remains "
          "latched");
}

void RetainedTileTail() {
    auto ppu = std::make_unique<Ppu>();
    ppu->WriteIO(0x2100, 15);
    ppu->WriteIO(0x2105, 1);
    ppu->WriteIO(0x2108, 4);
    ppu->WriteIO(0x210b, 0x10);
    ppu->WriteIO(0x212c, 2);
    ppu->WriteIO(0x210f, 5);
    ppu->WriteIO(0x210f, 0);
    ppu->CgramData()[17] = 31;
    for (unsigned i = 0; i < 32; ++i)
        ppu->VramData()[0x400 + i] = 0x2400;
    for (unsigned i = 0; i < 8; ++i)
        ppu->VramData()[0x1000 + i] = 0x00ff;
    ppu->FrameBegin();
    for (uint16_t line = 1; line <= 2; ++line) {
        ppu->SetCurrentLine(line);
        ppu->SetCurrentHClock(0);
        if (line == 2)
            ppu->WriteIO(0x2105, 7);
        ppu->SetCurrentHClock(512);
        ppu->ScanlineBegin(line);
        ppu->SetCurrentHClock(1080);
    }
    ppu->VBlankBegin();
    for (int x = 0; x < 256; ++x) {
        Check(Color(*ppu, x, 0) == 31, "The tiled line primes all background shift columns");
        Check(Color(*ppu, x, 1) == (x >= 251 && x <= 253 ? 31 : 0),
              "An inactive background drains the unconsumed tail of the preceding tiled line");
    }
}

void ExtendedVramFetches() {
    for (uint32_t bytes : {65536u, 131072u}) {
        auto ppu = std::make_unique<Ppu>(Ppu::Config{3, bytes});
        ppu->WriteIO(0x2100, 15);
        ppu->WriteIO(0x2107, 0x80);
        ppu->WriteIO(0x210b, 9);
        ppu->WriteIO(0x212c, 1);
        ppu->CgramData()[1] = 31;
        ppu->CgramData()[2] = 31 << 5;
        for (unsigned entry = 0; entry < 32; ++entry)
            ppu->VramData()[0x8000 + entry] = 1;
        for (unsigned row = 0; row < 8; ++row) {
            ppu->VramData()[0x1000 + row] = 0x00ff;
            ppu->VramData()[0x9008 + row] = 0xff00;
        }
        ppu->FrameBegin();
        ppu->SetCurrentLine(1);
        ppu->ScanlineBegin(1);
        ppu->VBlankBegin();
        Check(Color(*ppu, 0, 0) == (bytes == 131072 ? 31 << 5 : 31),
              "Background names and patterns use the selected VRAM capacity");
    }
}

void FirstEdgeScrollAlignment() {
    const auto render = [](unsigned writeClock, bool beforeLine) {
        auto ppu = std::make_unique<Ppu>();
        ppu->WriteIO(0x2100, 15);
        ppu->WriteIO(0x2105, 0);
        ppu->WriteIO(0x210b, 1);
        ppu->WriteIO(0x212c, 1);
        ppu->CgramData()[1] = 31;
        for (unsigned row = 0; row < 8; ++row)
            ppu->VramData()[0x1000 + row] = 0x00aa;
        if (beforeLine) {
            ppu->WriteIO(0x210d, 5);
            ppu->WriteIO(0x210d, 0);
        }
        ppu->FrameBegin();
        ppu->SetCurrentLine(1);
        if (!beforeLine) {
            ppu->SetCurrentHClock(uint16_t(writeClock));
            ppu->WriteIO(0x210d, 5);
            ppu->WriteIO(0x210d, 0);
        }
        ppu->ScanlineBegin(1);
        ppu->VBlankBegin();
        std::array<uint16_t, 256> result{};
        for (unsigned x = 0; x < result.size(); ++x)
            result[x] = Color(*ppu, int(x), 0);
        return result;
    };
    Check(render(0, false) == render(0, true),
          "A line-start scroll write participates in the first background fetch edge");
    Check(render(2, false)[0] == 31 && render(0, true)[0] == 0,
          "A scroll write after the first fetch edge retains the earlier fine alignment");
}

void SnapshotRowsMatchFetchEdges() {
    uint32_t seed = 0x41f792ab;
    const auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    auto ppu = std::make_unique<Ppu>();
    std::array<uint16_t, Ppu::VramWords> vram{};
    for (unsigned trial = 0; trial < 2048; ++trial) {
        ppu->WriteIO(0x2105, trial % 8);
        ppu->WriteIO(0x2133, (trial / 8) & 1 ? 0x40 : 0);
        auto io = ppu->GetIO();
        io.interlace = next() & 1;
        io.mosaic.size = uint8_t(1 + (next() & 15));
        io.mosaic.counter = uint8_t(1 + next() % io.mosaic.size);
        for (auto* bg : {&io.bg1, &io.bg2, &io.bg3, &io.bg4}) {
            bg->aboveEnable = next() & 1;
            bg->belowEnable = next() & 1;
            bg->mosaicEnable = next() & 1;
            bg->tileSize = next() & 1;
            bg->screenSize = uint8_t(next() & 3);
            bg->screenAddress = uint16_t(next() & 0x7c00);
            bg->tiledataAddress = uint16_t(next() & 0x7000);
            bg->hoffset = uint16_t(next() & 1023);
            bg->voffset = uint16_t(next() & 1023);
        }
        auto& matrix = io.mode7;
        for (auto* word : {&matrix.a, &matrix.b, &matrix.c, &matrix.d, &matrix.x, &matrix.y,
                           &matrix.hoffset, &matrix.voffset})
            *word = uint16_t(next());
        matrix.hflip = next() & 1;
        matrix.vflip = next() & 1;
        matrix.repeat = uint8_t(next() & 3);
        for (auto& word : vram)
            word = uint16_t(next());
        const uint16_t y = trial % 64 ? uint16_t(1 + next() % 239) : 0;
        const bool field = next() & 1;
        PpuBackground clock, batch;
        clock.BeginLine(y, field, io, Ppu::RenderModel::Scanline);
        std::array<PpuBackground::Output, 256> expected{};
        for (unsigned h = 0; h < 1080; h += 2)
            if (clock.Clock(uint16_t(h), io, vram))
                expected[(h - 58) / 4] = clock.Samples();
        const auto actual = batch.SampleLine(y, field, io, vram);
        for (unsigned x = 0; x < 256; ++x) {
            for (unsigned id = 0; id < 4; ++id) {
                for (bool below : {false, true}) {
                    const auto& a = (below ? actual[x].below : actual[x].above)[id];
                    const auto& b = (below ? expected[x].below : expected[x].above)[id];
                    Check(a.palette == b.palette && a.group == b.group && a.priority == b.priority,
                          "A fixed background snapshot retains every sample produced by its fetch "
                          "edges");
                }
            }
        }
    }
}
} // namespace

int main() {
    try {
        NameAndPlaneLatches();
        ScrollAndCharacterWrap();
        MosaicReload();
        ResetRetainsColumns();
        MidlineOutputMode();
        LiveObjectPriority();
        RetainedTileTail();
        ExtendedVramFetches();
        FirstEdgeScrollAlignment();
        SnapshotRowsMatchFetchEdges();
        std::puts("Background fetch tests passed");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
