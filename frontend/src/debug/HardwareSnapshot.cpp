#include "HardwareSnapshot.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace snes::frontend {
HardwareSnapshot HardwareSnapshot::Capture(const core::Emulator& machine) {
    if (!machine.Initialized())
        throw std::invalid_argument("Hardware inspection requires a loaded game");
    HardwareSnapshot result;
    result.frame = machine.CurrentFrame();
    result.clock = machine.CurrentMasterCycles();
    const auto& ppu = machine.GetPpu();
    result.ppu = ppu.GetIO();
    result.vram.assign(ppu.VramData(), ppu.VramData() + ppu.Configuration().vramBytes / 2);
    std::copy_n(ppu.CgramData(), result.cgram.size(), result.cgram.begin());
    std::copy_n(ppu.OamData(), result.oam.size(), result.oam.begin());
    const auto& dsp = machine.GetDsp();
    result.dsp = dsp.Regs();
    for (unsigned voice = 0; voice < result.voices.size(); ++voice)
        result.voices[voice] = dsp.GetVoice(int(voice));
    result.dspPhase = dsp.Phase();
    result.noise = dsp.Noise();
    result.counter = dsp.Counter();
    std::copy_n(machine.GetSmp().Ram(), result.audioRam.size(), result.audioRam.begin());
    return result;
}

uint32_t HardwareSnapshot::Color(uint16_t color) noexcept {
    const auto expand = [](unsigned value) { return (value << 3) | (value >> 2); };
    return 0xff000000u | expand(color & 31) | (expand((color >> 5) & 31) << 8) |
           (expand((color >> 10) & 31) << 16);
}

void HardwareSnapshot::ValidateVram() const {
    if (vram.size() != 32768 && vram.size() != 65536)
        throw std::invalid_argument("Video RAM must contain 64 or 128 KiB");
}

unsigned HardwareSnapshot::Depth(TileFormat format) {
    switch (format) {
    case TileFormat::Planar2:
        return 2;
    case TileFormat::Planar4:
        return 4;
    case TileFormat::Planar8:
    case TileFormat::Mode7:
        return 8;
    }
    throw std::invalid_argument("Choose a supported tile format");
}

unsigned HardwareSnapshot::TileCount(TileFormat format) const {
    ValidateVram();
    return unsigned(vram.size() * 2 / (format == TileFormat::Mode7 ? 128 : Depth(format) * 8));
}

uint8_t HardwareSnapshot::VramByte(size_t address) const {
    return uint8_t(vram[(address >> 1) & (vram.size() - 1)] >> ((address & 1) * 8));
}

uint8_t HardwareSnapshot::Pixel(TileFormat format, size_t baseByte, unsigned x, unsigned y) const {
    if (format == TileFormat::Mode7)
        return VramByte(baseByte + (y * 8 + x) * 2 + 1);
    unsigned pixel = 0;
    for (unsigned plane = 0; plane < Depth(format); ++plane)
        pixel |= ((VramByte(baseByte + (plane / 2) * 16 + y * 2 + (plane & 1)) >> (7 - x)) & 1)
                 << plane;
    return uint8_t(pixel);
}

uint32_t HardwareSnapshot::PixelColor(unsigned index, unsigned x, unsigned y,
                                      bool transparentZero) const {
    if (transparentZero && index == 0)
        return ((x / 4) ^ (y / 4)) & 1 ? 0xff606060 : 0xff404040;
    return Color(cgram[index]);
}

core::VideoFrame HardwareSnapshot::Tiles(const TileOptions& options) const {
    const auto count = TileCount(options.format);
    const auto depth = Depth(options.format);
    if (options.baseByte >= vram.size() * 2 || (options.baseByte & 1) ||
        options.firstTile >= count || options.palette >= (256u >> depth))
        throw std::out_of_range("Tile base, page or palette is outside video memory");
    core::VideoFrame image{128, 128, {}};
    image.pixels.resize(128 * 128);
    const auto tileBytes = options.format == TileFormat::Mode7 ? 128u : depth * 8;
    for (unsigned tile = 0; tile < 256; ++tile) {
        const auto base =
            size_t(options.baseByte) + size_t((options.firstTile + tile) % count) * tileBytes;
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x) {
                const auto pixel = Pixel(options.format, base, x, y);
                const auto px = (tile & 15) * 8 + x, py = (tile >> 4) * 8 + y;
                const unsigned color = options.palette * (1u << depth) + pixel;
                image.pixels[py * 128 + px] = options.transparentZero && pixel == 0
                                                  ? PixelColor(0, px, py, true)
                                                  : Color(cgram[color]);
            }
    }
    return image;
}

std::array<HardwareSnapshot::Object, 128> HardwareSnapshot::Objects() const {
    if (ppu.obj.baseSize >= 8)
        throw std::out_of_range("Object size selection is outside the hardware range");
    std::array<Object, 128> result;
    for (unsigned index = 0; index < result.size(); ++index) {
        const auto extra = (oam[512 + index / 4] >> ((index & 3) * 2)) & 3;
        const auto attributes = oam[index * 4 + 3];
        const unsigned x = oam[index * 4] | ((extra & 1) << 8);
        const bool large = extra & 2;
        result[index] = {index,
                         x,
                         oam[index * 4 + 1],
                         uint8_t(oam[index * 4 + 1] + 1),
                         unsigned(oam[index * 4 + 2]) | ((attributes & 1u) << 8),
                         unsigned((attributes >> 1) & 7),
                         unsigned((attributes >> 4) & 3),
                         large ? core::Ppu::kObjLargeWidth[ppu.obj.baseSize]
                               : core::Ppu::kObjSmallWidth[ppu.obj.baseSize],
                         large ? core::Ppu::kObjLargeHeight[ppu.obj.baseSize]
                               : core::Ppu::kObjSmallHeight[ppu.obj.baseSize],
                         x & 256 ? int(x) - 512 : int(x),
                         large,
                         bool(attributes & 0x40),
                         bool(attributes & 0x80)};
    }
    return result;
}

core::VideoFrame HardwareSnapshot::ObjectImage(unsigned index, bool applyFlips,
                                               bool transparentZero) const {
    ValidateVram();
    if (index >= 128)
        throw std::out_of_range("Choose an object between 0 and 127");
    const auto object = Objects()[index];
    core::VideoFrame image{object.width, object.height, {}};
    image.pixels.resize(object.width * object.height);
    const auto bank = object.tile & 256 ? (1u + ppu.obj.nameselect) << 12 : 0;
    for (unsigned y = 0; y < object.height; ++y)
        for (unsigned x = 0; x < object.width; ++x) {
            const auto sx = applyFlips && object.flipX ? object.width - 1 - x : x;
            const auto sy = applyFlips && object.flipY ? (y / object.width) * object.width +
                                                             object.width - 1 - (y % object.width)
                                                       : y;
            const auto column = ((object.tile & 15) + sx / 8) & 15;
            const auto row = (((object.tile & 255) >> 4) + sy / 8) & 15;
            const auto word = uint16_t(ppu.obj.tiledataAddress + bank + row * 256 + column * 16);
            const auto pixel = Pixel(TileFormat::Planar4, size_t(word) * 2, sx & 7, sy & 7);
            image.pixels[y * object.width + x] =
                transparentZero && pixel == 0 ? PixelColor(0, x, y, true)
                                              : Color(cgram[128 + object.palette * 16 + pixel]);
        }
    return image;
}

uint16_t HardwareSnapshot::DirectoryAddress(unsigned voice, bool loop) const {
    if (voice >= voices.size())
        throw std::out_of_range("Choose an audio voice between 0 and 7");
    const auto address =
        uint16_t((unsigned(dsp[core::Dsp::kDir]) << 8) +
                 unsigned(dsp[voice * 16 + core::Dsp::kSrcn]) * 4 + (loop ? 2 : 0));
    return uint16_t(audioRam[address] | (unsigned(audioRam[uint16_t(address + 1)]) << 8));
}

std::array<float, 12> HardwareSnapshot::DecodedSamples(unsigned voice) const {
    if (voice >= voices.size())
        throw std::out_of_range("Choose an audio voice between 0 and 7");
    const auto& state = voices[voice];
    if (state.bufPos < 0 || state.bufPos >= 12)
        throw std::out_of_range("Invalid decoded sample ring position");
    std::array<float, 12> result;
    for (unsigned index = 0; index < result.size(); ++index)
        result[index] = float(state.buf[(unsigned(state.bufPos) + index) % 12]) / 32768.0f;
    return result;
}

std::string HardwareSnapshot::AudioText() const {
    std::ostringstream out;
    out << "frame\t" << frame << "\nmaster_clock\t" << clock << "\ndsp_phase\t" << dspPhase << "\n";
    out << "voice\tvolume_l\tvolume_r\tpitch\tsource\tstart\tloop\tBRR\tenvelope\tmode\toutput\n";
    for (unsigned index = 0; index < voices.size(); ++index) {
        const auto offset = index * 16;
        const auto& voice = voices[index];
        const auto signedByte = [](uint8_t value) {
            return value & 128 ? int(value) - 256 : int(value);
        };
        out << index << '\t' << signedByte(dsp[offset]) << '\t' << signedByte(dsp[offset + 1])
            << '\t' << ((unsigned(dsp[offset + 3] & 63) << 8) | dsp[offset + 2]) << '\t'
            << unsigned(dsp[offset + 4]) << '\t' << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << DirectoryAddress(index, false) << '\t' << std::setw(4)
            << DirectoryAddress(index, true) << '\t' << std::setw(4) << voice.brrAddr << '\t'
            << std::dec << voice.env << '\t' << int(voice.envMode) << '\t' << voice.output << '\n';
    }
    out << "\nregister\tvalue\n" << std::hex << std::uppercase << std::setfill('0');
    for (unsigned index = 0; index < dsp.size(); ++index)
        out << std::setw(2) << index << '\t' << std::setw(2) << unsigned(dsp[index]) << '\n';
    return out.str();
}
} // namespace snes::frontend
