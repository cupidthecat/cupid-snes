#pragma once

#include "../gameboy/StateFixture.hpp"
#include "snes/core/Cartridge.hpp"
#include "snes/core/PowerOnState.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace snes::tests::cartridge_state {
using namespace snes::core;
enum class Board {
    Lo,
    Hi,
    ExHi,
    NoMad,
    Rom24,
    LargeRam,
    Obc1,
    Sdd1,
    Spc7110,
    SpcRtc,
    Sa1,
    SuperFx,
    Cx4Command,
    Cx4Processor,
    St010Command,
    St010Processor,
    St011Processor,
    St018Processor,
    Dsp1Command,
    Dsp1Processor,
    Dsp2Command,
    Dsp2Processor,
    Dsp3Processor,
    Dsp4Command,
    Dsp4Processor,
    Srtc,
    Disconnected,
    Handheld,
    Handheld2,
    Sufami,
    BsxFlash,
    BsxRom,
    SlotLo,
    SlotHi,
    SlotSa1,
    Competition92,
    Competition94,
    Competition92Processor,
    Competition94Processor,
    Count
};

inline std::vector<uint8_t> Image(uint8_t mode = 0x20, uint8_t type = 2, size_t size = 0x100000,
                                  std::string_view title = "STATE TEST") {
    std::vector<uint8_t> bytes(size);
    for (size_t n = 0; n < bytes.size(); ++n)
        bytes[n] = uint8_t((n >> 15) * 13 + n * 7);
    const size_t header = mode == 0x35 ? 0x40ffc0 : (mode & 1) && mode != 0x23 ? 0xffc0 : 0x7fc0;
    std::fill_n(bytes.begin() + header - 16, 80, 0);
    std::fill_n(bytes.begin() + header, 21, ' ');
    std::copy(title.begin(), title.end(), bytes.begin() + header);
    bytes[header + 0x15] = mode;
    bytes[header + 0x16] = type;
    bytes[header - 1] = type == 0xf3                   ? 0x10
                        : type == 0xf6                 ? 1
                        : type == 0xf5 && mode == 0x30 ? 2
                                                       : 0;
    bytes[header + 0x17] = 10;
    bytes[header + 0x18] = (type >> 4) == 1 || type == 0xf3 || type == 0xf6 ? 0 : 3;
    bytes[header + 0x19] = 1;
    bytes[header + 0x1c] = bytes[header + 0x1d] = 255;
    bytes[header + 0x3d] = 0x80;
    const auto entry = header & ~size_t(0x7fff);
    bytes[entry] = 0x80;
    bytes[entry + 1] = 0xfe;
    return bytes;
}

inline std::vector<uint8_t> SlotBase(Board board) {
    const bool hi = board == Board::SlotHi;
    auto image = Image(hi ? 0x21 : board == Board::SlotSa1 ? 0x23 : 0x20);
    const auto header = hi ? 0xffc0u : 0x7fc0u;
    image[header + 0x1a] = 0x33;
    std::copy_n(board == Board::BsxFlash || board == Board::BsxRom ? "ZBSJ" : "ZGNJ", 4,
                image.begin() + header - 14);
    return image;
}

inline std::vector<uint8_t> Pack(Board board, unsigned variant = 0) {
    std::vector<uint8_t> pack(board == Board::BsxRom ? 0x80000 : 0x100000, 255);
    pack[0x1234] = uint8_t(0x37 + variant);
    if (board == Board::BsxRom) {
        pack[0xff00] = 'M';
        pack[0xff02] = 'P';
        pack[0xff06] = 0x70;
    }
    return pack;
}

inline Cartridge Require(std::optional<Cartridge> cart, const std::string& error) {
    if (!cart)
        throw std::runtime_error("Load cartridge state fixture: " + error);
    return std::move(*cart);
}

inline Cartridge Create(Board board, unsigned variant = 0) {
    std::string error;
    auto rom = Image();
    CartridgeFirmware firmware;
    bool firmwarePresent = false;
    const auto addFirmware = [&](FirmwareKind kind) {
        std::vector<uint8_t> data(CartridgeFirmware::Size(kind));
        data.back() = uint8_t(variant);
        if (!firmware.Set(kind, data, &error))
            throw std::runtime_error(error);
        firmwarePresent = true;
    };
    switch (board) {
    case Board::Hi:
        rom = Image(0x21);
        break;
    case Board::ExHi:
        rom = Image(0x35, 2, 0x500000);
        break;
    case Board::Obc1:
        rom = Image(0x30, 0x25);
        break;
    case Board::Sdd1:
        rom = Image(0x32, 0x43);
        break;
    case Board::Spc7110:
        rom = Image(0x3a, 0xf5, 0x300000);
        break;
    case Board::SpcRtc:
        rom = Image(0x3a, 0xf9, 0x300000);
        break;
    case Board::Sa1:
        rom = Image(0x23, 0x35);
        break;
    case Board::SuperFx:
        rom = Image(0x20, 0x15);
        break;
    case Board::Cx4Command:
    case Board::Cx4Processor:
        rom = Image(0x20, 0xf3);
        if (board == Board::Cx4Processor)
            addFirmware(FirmwareKind::Cx4);
        break;
    case Board::St010Command:
    case Board::St010Processor:
        rom = Image(0x20, 0xf6);
        if (board == Board::St010Processor)
            addFirmware(FirmwareKind::St010);
        break;
    case Board::St011Processor:
        rom = Image(0x20, 0xf6, 0x100000, "2DAN MORITA SHOUGI");
        addFirmware(FirmwareKind::St011);
        break;
    case Board::St018Processor:
        rom = Image(0x30, 0xf5);
        addFirmware(FirmwareKind::St018);
        break;
    case Board::Dsp1Command:
    case Board::Dsp1Processor:
        rom = Image(0x20, 3);
        if (board == Board::Dsp1Processor)
            addFirmware(FirmwareKind::Dsp1b);
        break;
    case Board::Dsp2Command:
    case Board::Dsp2Processor:
        rom = Image(0x20, 3, 0x100000, "DUNGEON MASTER");
        if (board == Board::Dsp2Processor)
            addFirmware(FirmwareKind::Dsp2);
        break;
    case Board::Dsp3Processor:
        rom = Image(0x20, 3, 0x100000, "SD\xb6\xde\xdd\xc0\xde\xd1GX");
        addFirmware(FirmwareKind::Dsp3);
        break;
    case Board::Dsp4Command:
    case Board::Dsp4Processor:
        rom = Image(0x20, 3, 0x100000, "TOP GEAR 3000");
        if (board == Board::Dsp4Processor)
            addFirmware(FirmwareKind::Dsp4);
        break;
    case Board::Srtc:
        rom = Image(0x35, 0x55, 0x500000);
        break;
    case Board::Disconnected:
        rom = Image(0x21, 0x25);
        break;
    case Board::Handheld:
    case Board::Handheld2: {
        const auto bios = Image(0x20, 0xe3, 0x80000,
                                board == Board::Handheld2 ? "Super GAMEBOY2" : "Super GAMEBOY");
        auto game = handheld::Image();
        game[0x1234] ^= uint8_t(variant);
        return Require(Cartridge::FromSuperGameBoy(bios, game, handheld::Boot(), &error), error);
    }
    case Board::Sufami: {
        auto bios = Image(0x20, 0, 0x40000);
        std::copy_n("BANDAI SFC-ADX", 14, bios.begin());
        std::copy_n("SFC-ADX BACKUP", 14, bios.begin() + 0x10);
        std::array<std::vector<uint8_t>, 2> slots;
        for (unsigned n = 0; n < slots.size(); ++n) {
            slots[n] = std::vector<uint8_t>(0x20000, uint8_t(n * 31));
            std::copy_n("BANDAI SFC-ADX", 14, slots[n].begin());
            slots[n][0x37] = uint8_t(8 + n * 8);
            slots[n][0x1234] ^= uint8_t(variant);
        }
        return Require(Cartridge::FromSufamiTurbo(bios, slots[0], slots[1], &error), error);
    }
    case Board::BsxFlash:
    case Board::BsxRom:
    case Board::SlotLo:
    case Board::SlotHi:
    case Board::SlotSa1:
        return Require(
            Cartridge::FromBroadcastCartridge(SlotBase(board), Pack(board, variant), &error),
            error);
    case Board::Competition92:
    case Board::Competition94:
    case Board::Competition92Processor:
    case Board::Competition94Processor: {
        const bool campus = board == Board::Competition92 || board == Board::Competition92Processor;
        if (board == Board::Competition92Processor || board == Board::Competition94Processor)
            addFirmware(FirmwareKind::Dsp1b);
        std::array<std::vector<uint8_t>, 4> images;
        std::array<std::span<const uint8_t>, 4> spans;
        for (unsigned n = 0; n < images.size(); ++n) {
            images[n] = Image(0x20, 0, 0x8000);
            images[n][0x1234] = uint8_t(variant + n * 17);
            spans[n] = images[n];
        }
        CompetitionBoard::Settings settings;
        settings.board = campus ? CompetitionBoard::Board::CampusChallenge92
                                : CompetitionBoard::Board::PowerFest94;
        settings.dipSwitches = 3;
        settings.saveRamSize = 8192;
        return Require(Cartridge::FromCompetitionCartridge(settings, spans, &error, &firmware),
                       error);
    }
    default:
        break;
    }
    if (variant && !firmwarePresent)
        rom[0x1234] ^= uint8_t(variant);
    if (board == Board::NoMad || board == Board::Rom24 || board == Board::LargeRam) {
        auto base = Require(Cartridge::FromRomImage(rom, nullptr, nullptr, &error), error);
        CartridgeDatabase database;
        DatabaseOverride entry;
        entry.forceMapping = board == Board::NoMad   ? MappingType::LoRomNoMad1
                             : board == Board::Rom24 ? MappingType::LoRom24Mbit
                                                     : MappingType::LoRomLargeSram;
        entry.forceSramSize = 32768;
        database.AddOverride(base.RomCrc32(), entry);
        return Require(Cartridge::FromRomImage(rom, nullptr, &database, &error), error);
    }
    return Require(Cartridge::FromRomImage(rom, nullptr, nullptr, &error, &firmware), error);
}

struct Fixture {
    Board board;
    uint64_t now = 1700000000, timeCalls = 0, audioCalls = 0;
    uint64_t audioHash = 14695981039346656037ull, ports = 0;
    Cartridge cart;

    explicit Fixture(Board kind, bool pal = false) : board(kind), cart(Create(kind)) {
        const bool signal = kind == Board::Dsp1Processor || kind == Board::Dsp2Processor ||
                            kind == Board::Dsp3Processor || kind == Board::Dsp4Processor ||
                            kind == Board::St010Processor || kind == Board::St011Processor ||
                            kind == Board::Competition92Processor ||
                            kind == Board::Competition94Processor;
        if (cart.HasSignalProcessor() != signal ||
            cart.HasArmProcessor() != (kind == Board::St018Processor) ||
            cart.HasCx4Processor() != (kind == Board::Cx4Processor) ||
            (kind == Board::Dsp3Processor && cart.Header().chip != EnhancementChip::Dsp3))
            throw std::runtime_error("Generated cartridge selects unexpected execution hardware");
        cart.SetPal(pal);
        cart.SetSa1BusContention(false);
        cart.SetBroadcastTimeSource([this] {
            ++timeCalls;
            return BroadcastTime{2025, 7, 8, 2, 13, 47, 31};
        });
        if (cart.SetGameBoyTimeSource([this] {
                ++timeCalls;
                return now;
            })) {
            auto startup = std::make_shared<gameboy::StartupState>();
            startup->Configure(true, 0x739155);
            cart.InitializeGameBoyStartup(startup);
            cart.SetGameBoyAudioOutput([this](int16_t left, int16_t right, uint32_t rate) {
                ++audioCalls;
                handheld::Hash(audioHash, uint16_t(left));
                handheld::Hash(audioHash, uint16_t(right));
                handheld::Hash(audioHash, rate);
            });
            cart.Write(0x6003, 0x81);
            for (unsigned step = 0; step < 64; ++step) {
                cart.WriteGameBoyConsoleAudio(int16_t(step * 31), int16_t(-int(step * 17)));
                cart.AdvanceHardware(257);
            }
            if (audioCalls == 0)
                throw std::runtime_error(
                    "Handheld cartridge fixture must produce live audio before capture");
        }
        if (kind == Board::Sa1 || kind == Board::SlotSa1) {
            cart.Write(0x2203, 0);
            cart.Write(0x2204, 0x80);
            cart.Write(0x2200, 0);
        }
    }

    void Edit(unsigned step) {
        now = 1700000000 + step * 31;
        cart.SetMemselFast(step & 1);
        cart.Write(0x700000 + (step & 511), uint8_t(step * 13));
        cart.Write(0x206000 + (step & 511), uint8_t(step * 7));
        if (board != Board::Handheld && board != Board::Handheld2)
            cart.Write(0x006000 + (step & 511), uint8_t(step * 17));
        if (board == Board::Handheld || board == Board::Handheld2) {
            cart.Write(0x6004 + (step & 3), uint8_t(step * 31));
            cart.WriteGameBoyConsoleAudio(int16_t(step * 31), int16_t(-int(step * 17)));
        }
        if (board == Board::Spc7110 || board == Board::SpcRtc) {
            cart.Write(0x4820, uint8_t(step));
            cart.Write(0x4824, uint8_t(step * 7));
            cart.Write(0x4825, uint8_t(step * 13));
        }
        cart.AdvanceHardware(step % 31 + 1, 0x808000, false, step % 3 != 1);
        cart.SynchronizeState();
        for (uint32_t address :
             {0x008000u, 0x808123u, 0x206123u, 0x406123u, 0x700123u, 0x004800u, 0x003000u})
            handheld::Hash(ports, cart.Read(address, 0x53));
    }

    void SerializeState(detail::StateArchive& archive) {
        cart.SerializeState(archive);
        archive.Number(audioHash);
        archive.Number(ports);
    }
};
} // namespace snes::tests::cartridge_state
