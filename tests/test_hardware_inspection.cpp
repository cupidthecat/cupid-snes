#include "ConsoleConfig.hpp"
#include "debug/HardwareSnapshot.hpp"
#include "state/EmulatorBoards.hpp"
#include "storage/InspectionBitmap.hpp"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

using snes::frontend::HardwareSnapshot;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class Function> void Reject(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception&) {
        rejected = true;
    }
    Check(rejected, "Invalid hardware inspection request rejects");
}

HardwareSnapshot Snapshot(size_t bytes = 65536) {
    HardwareSnapshot snapshot;
    snapshot.vram.resize(bytes / 2);
    for (unsigned index = 0; index < snapshot.cgram.size(); ++index)
        snapshot.cgram[index] = uint16_t(index);
    return snapshot;
}

void ColorsAndTiles() {
    using Format = HardwareSnapshot::TileFormat;
    Check(HardwareSnapshot::Color(0x001f) == 0xff0000ff &&
              HardwareSnapshot::Color(0x03e0) == 0xff00ff00 &&
              HardwareSnapshot::Color(0x7c00) == 0xffff0000,
          "Palette conversion retains red, green and blue channel order");
    Check(HardwareSnapshot::Color(0x7fff) == 0xffffffff &&
              HardwareSnapshot::Color(0xffff) == 0xffffffff &&
              HardwareSnapshot::Color(0x0010) == 0xff000084,
          "Five-bit channels expand through bit replication and ignore the unused high bit");
    for (size_t bytes : {65536u, 131072u}) {
        auto snapshot = Snapshot(bytes);
        snapshot.vram[0] = 0x4080;
        snapshot.vram[8] = 0x1020;
        snapshot.vram[16] = 0x0408;
        snapshot.vram[24] = 0x0102;
        for (auto format : {Format::Planar2, Format::Planar4, Format::Planar8}) {
            HardwareSnapshot::TileOptions options;
            options.format = format;
            options.transparentZero = false;
            const auto image = snapshot.Tiles(options);
            Check(image.width == 128 && image.height == 128 && image.pixels.size() == 16384,
                  "A tile page contains a 16 by 16 grid of eight-pixel tiles");
            for (unsigned pixel = 0; pixel < HardwareSnapshot::Depth(format); ++pixel)
                Check(image.pixels[pixel] == HardwareSnapshot::Color(uint16_t(1u << pixel)),
                      "Independent bit planes select their corresponding palette bits");
        }
        std::fill(snapshot.vram.begin(), snapshot.vram.end(), 0);
        snapshot.vram[0] = 0x0180;
        snapshot.vram[1] = 0x0200;
        HardwareSnapshot::TileOptions options;
        options.format = Format::Mode7;
        options.transparentZero = false;
        const auto packed = snapshot.Tiles(options);
        Check(packed.pixels[0] == HardwareSnapshot::Color(1) &&
                  packed.pixels[1] == HardwareSnapshot::Color(2),
              "Packed tiles read high VRAM bytes independently of map bytes");
        std::fill(snapshot.vram.begin(), snapshot.vram.end(), 0);
        options.format = Format::Planar2;
        options.baseByte = uint32_t(bytes - 2);
        snapshot.vram.back() = 0x0080;
        snapshot.vram.front() = 0x0040;
        const auto wrapped = snapshot.Tiles(options);
        Check(wrapped.pixels[0] == HardwareSnapshot::Color(1) &&
                  wrapped.pixels[128 + 1] == HardwareSnapshot::Color(1),
              "Tile rows wrap at the configured video memory boundary");
        options.baseByte = 0;
        options.format = Format::Planar4;
        options.palette = 3;
        snapshot.vram.front() = 0x0080;
        Check(snapshot.Tiles(options).pixels[0] == HardwareSnapshot::Color(49),
              "Palette selection contributes the correct color-bank offset");
        options.palette = 16;
        Reject([&] { snapshot.Tiles(options); });
        options.palette = 0;
        options.baseByte = 1;
        Reject([&] { snapshot.Tiles(options); });
        options.baseByte = uint32_t(bytes);
        Reject([&] { snapshot.Tiles(options); });
        options.baseByte = 0;
        options.firstTile = snapshot.TileCount(options.format);
        Reject([&] { snapshot.Tiles(options); });
    }
    HardwareSnapshot empty;
    Reject([&] { empty.Tiles({}); });
    Reject([&] { HardwareSnapshot::Depth(Format(99)); });
}

void Objects() {
    auto snapshot = Snapshot();
    snapshot.ppu.obj.baseSize = 2;
    snapshot.oam[0] = 250;
    snapshot.oam[1] = 255;
    snapshot.oam[2] = 0xab;
    snapshot.oam[3] = 0xeb;
    snapshot.oam[512] = 3;
    snapshot.oam[543] = 0xc0;
    const auto objects = snapshot.Objects();
    const auto& first = objects.front();
    Check(first.rawX == 506 && first.x == -6 && first.rawY == 255 && first.y == 0 &&
              first.tile == 0x1ab && first.palette == 5 && first.priority == 2 && first.large &&
              first.flipX && first.flipY && first.width == 64 && first.height == 64,
          "Object inspection combines high-table bits, coordinates, attributes and selected sizes");
    Check(objects.back().rawX == 256 && objects.back().x == -256 && objects.back().large,
          "The final object reads the final high-table bit pair");
    snapshot.oam.fill(0);
    snapshot.ppu.obj.baseSize = 0;
    snapshot.cgram[128] = 0;
    snapshot.cgram[129] = 0x1f;
    snapshot.cgram[130] = 0x3e0;
    snapshot.vram[0] = 0x0080;
    Check(snapshot.ObjectImage(0, true, false).pixels[0] == 0xff0000ff,
          "Object pixels use the object palette half");
    snapshot.oam[3] = 0x40;
    Check(snapshot.ObjectImage(0, true, false).pixels[7] == 0xff0000ff,
          "Horizontal object flips reverse pixel positions");
    snapshot.oam[3] = 0x80;
    Check(snapshot.ObjectImage(0, true, false).pixels[7 * 8] == 0xff0000ff,
          "Vertical object flips reverse square object rows");
    snapshot.vram[0] = 0;
    snapshot.ppu.obj.nameselect = 2;
    snapshot.oam[3] = 1;
    snapshot.vram[0x3000] = 0x0080;
    Check(snapshot.ObjectImage(0, true, false).pixels[0] == 0xff0000ff,
          "The object name bit selects the alternate tile region");
    std::fill(snapshot.vram.begin(), snapshot.vram.end(), 0);
    snapshot.ppu.obj.nameselect = 0;
    snapshot.ppu.obj.baseSize = 6;
    snapshot.oam[3] = 0x80;
    snapshot.vram[0] = 0x0080;
    snapshot.vram[512] = 0x8000;
    const auto rectangular = snapshot.ObjectImage(0, true, false);
    Check(rectangular.width == 16 && rectangular.height == 32 &&
              rectangular.pixels[15 * 16] == 0xff0000ff &&
              rectangular.pixels[31 * 16] == 0xff00ff00,
          "Rectangular vertical flips retain the order of their square tile groups");
    Reject([&] { snapshot.ObjectImage(128); });
    snapshot.ppu.obj.baseSize = 8;
    Reject([&] { snapshot.Objects(); });
}

void Audio() {
    auto snapshot = Snapshot();
    snapshot.dsp[snes::core::Dsp::kDir] = 0xff;
    snapshot.dsp[4] = 64;
    snapshot.audioRam[0] = 0x34;
    snapshot.audioRam[1] = 0x12;
    snapshot.audioRam[2] = 0x78;
    snapshot.audioRam[3] = 0x56;
    Check(snapshot.DirectoryAddress(0, false) == 0x1234 &&
              snapshot.DirectoryAddress(0, true) == 0x5678,
          "Directory entries retain little-endian words and 16-bit address wrapping");
    auto& voice = snapshot.voices[0];
    voice.bufPos = 8;
    for (unsigned index = 0; index < 12; ++index)
        voice.buf[index] = int(index * 1000);
    const auto samples = snapshot.DecodedSamples(0);
    Check(samples[0] == 8000.0f / 32768 && samples[4] == 0 && samples[11] == 7000.0f / 32768,
          "Decoded samples are returned in ring order with a consistent amplitude scale");
    snapshot.dsp[0] = 0x80;
    snapshot.dsp[1] = 0x7f;
    snapshot.dsp[2] = snapshot.dsp[3] = 0xff;
    voice.brrAddr = 0xabcd;
    voice.env = 2047;
    voice.envMode = snes::core::Dsp::Sustain;
    voice.output = -1234;
    Check(snapshot.AudioText().find(
              "0\t-128\t127\t16383\t64\t1234\t5678\tABCD\t2047\t3\t-1234\n") != std::string::npos,
          "Audio export retains signed volumes, masked pitch, directory pointers and voice state");
    Reject([&] { snapshot.DirectoryAddress(8, false); });
    Reject([&] { snapshot.DecodedSamples(8); });
    voice.bufPos = -1;
    Reject([&] { snapshot.DecodedSamples(0); });
}

void Bitmap() {
    Check(SDL_Init(0), SDL_GetError());

    struct Lifetime {
        ~Lifetime() {
            SDL_Quit();
        }
    } lifetime;

    const snes::core::VideoFrame picture{2, 2, {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff}};
    const auto bytes = snes::frontend::InspectionBitmap(picture);
    Check(bytes.size() > 54 && bytes[0] == 'B' && bytes[1] == 'M',
          "Inspection export creates a bitmap");
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> decoded(
        SDL_LoadBMP_IO(SDL_IOFromConstMem(bytes.data(), bytes.size()), true), SDL_DestroySurface);
    Check(bool(decoded), "The inspection bitmap can be opened independently");
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> converted(
        SDL_ConvertSurface(decoded.get(), SDL_PIXELFORMAT_ABGR8888), SDL_DestroySurface);
    Check(converted && converted->w == 2 && converted->h == 2, "Bitmap dimensions survive export");
    for (unsigned row = 0; row < 2; ++row) {
        const auto* pixels = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(converted->pixels) + row * converted->pitch);
        for (unsigned column = 0; column < 2; ++column)
            Check(pixels[column] == picture.pixels[row * 2 + column],
                  "Bitmap export retains pixel channels and row orientation");
    }
    auto invalid = picture;
    invalid.width = 0;
    Reject([&] { snes::frontend::InspectionBitmap(invalid); });
    invalid.width = 513;
    Reject([&] { snes::frontend::InspectionBitmap(invalid); });
    invalid.width = 2;
    invalid.pixels.pop_back();
    Reject([&] { snes::frontend::InspectionBitmap(invalid); });
}

struct Input : snes::core::IInputProvider {
    unsigned polls = 0, fields = 0;

    snes::core::InputState Poll(uint64_t) override {
        ++polls;
        return {};
    }

    void AdvanceFrame() noexcept override {
        ++fields;
    }
};

void Purity() {
    using namespace snes::tests::emulator_state;
    for (unsigned index = 0; index < unsigned(Board::Count); ++index) {
        const auto board = Board(index);
        for (auto region : {snes::core::Region::NTSC, snes::core::Region::PAL}) {
            auto config = snes::tests::DeterministicConsoleConfig();
            config.cartridgeFirmware = Firmware(board);
            config.regionOverride = region;
            auto machine = std::make_unique<snes::core::Emulator>(config);
            Input input;
            machine->AttachInputProvider(&input);
            Load(*machine, board);
            machine->StepFrame();
            const auto before = machine->CaptureState();
            const auto clock = machine->CurrentMasterCycles(), frame = machine->CurrentFrame();
            const auto polls = input.polls, fields = input.fields;
            const auto snapshot = HardwareSnapshot::Capture(*machine);
            Check(snapshot.clock == clock && snapshot.frame == frame,
                  "Hardware snapshots retain their capture position");
            for (auto format :
                 {HardwareSnapshot::TileFormat::Planar2, HardwareSnapshot::TileFormat::Planar4,
                  HardwareSnapshot::TileFormat::Planar8, HardwareSnapshot::TileFormat::Mode7}) {
                HardwareSnapshot::TileOptions options;
                options.format = format;
                Check(!snapshot.Tiles(options).pixels.empty(),
                      "Every raw tile view can be rendered");
            }
            Check(!snapshot.ObjectImage(0).pixels.empty() && !snapshot.AudioText().empty(),
                  "Graphics and audio views can be generated from one immutable snapshot");
            Check(machine->CurrentMasterCycles() == clock && machine->CurrentFrame() == frame &&
                      input.polls == polls && input.fields == fields &&
                      machine->CaptureState() == before,
                  "Hardware inspection retains every machine byte, clock, field and input poll");
        }
    }
    auto idle = std::make_unique<snes::core::Emulator>();
    Reject([&] { HardwareSnapshot::Capture(*idle); });
}
} // namespace

int main() {
    try {
        ColorsAndTiles();
        Objects();
        Audio();
        Bitmap();
        Purity();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("Hardware inspection checks passed");
    return 0;
}
