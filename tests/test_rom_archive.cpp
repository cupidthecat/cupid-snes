#include "ConsoleConfig.hpp"
#include "archive/ArchiveFixture.hpp"
#include "cartridge/StateFixture.hpp"
#include "gameboy/StateFixture.hpp"
#include "session/GameAssets.hpp"
#include "session/Session.hpp"
#include "settings/Preferences.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "storage/AtomicFile.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

using namespace snes::frontend;
using namespace snes::core;
using namespace snes::tests::archives;
namespace fs = std::filesystem;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class Function> void Reject(Function perform, const char* message) {
    try {
        perform();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

std::vector<uint8_t> ConsoleImage(uint8_t value = 0x5a) {
    auto bytes = snes::tests::cartridge_state::Image(0x20, 2, 32768);
    const std::array<uint8_t, 7> code{0xa9, value, 0x8f, 0, 0, 0x70, 0xdb};
    std::copy(code.begin(), code.end(), bytes.begin());
    return bytes;
}

Session NewSession() {
    return Session(snes::tests::DeterministicConsoleConfig(), {},
                   {ControllerDevice::Gamepad, ControllerDevice::Gamepad});
}

void Formats(const fs::path& home) {
    const auto rar5 =
        RomArchive::Read(fs::path(SNES_ARCHIVE_FIXTURES) / "compressed-rar5.rar", "test.bin");
    Check(rar5.size() == 1200, "Compressed RAR 5 payload has its declared size");
    for (int k = 1; k <= 300; ++k) {
        const uint32_t expected = uint32_t(std::max(0, k * k - 3 * k + 1));
        for (unsigned byte = 0; byte < 4; ++byte)
            Check(rar5[size_t(k - 1) * 4 + byte] == uint8_t(expected >> (byte * 8)),
                  "Compressed RAR 5 decodes every payload byte");
    }
    const auto image = ConsoleImage();
    Write(home / "original.sfc", image);
    auto plain = NewSession();
    LaunchRequest request;
    request.paths[0] = PathText(home / "original.sfc");
    plain.Open(request);
    plain.StepFrame();
    const auto expected = plain.Machine()->CaptureState();
    for (const auto& format :
         {"zip", "7z", "tar", "tar.gz", "tar.bz2", "tar.xz", "tar.zst", "tar.lz4", "rar", "raw.gz",
          "raw.bz2", "raw.xz", "raw.zst", "raw.lz4"}) {
        const bool raw = std::string_view(format).starts_with("raw");
        const auto path = home / (raw ? std::string("game.sfc") + std::string(format).substr(3)
                                      : std::string("game.sfc.") + format);
        if (std::string_view(format) == "rar")
            Rar(path, {"game.sfc", image});
        else
            Archive(path, format,
                    raw ? std::vector<File>{{"data", image}}
                        : std::vector<File>{{"notes.txt", {1, 2, 3}}, {"nested/game.SFC", image}});
        const auto before = ReadFile(path, 1024 * 1024);
        auto session = NewSession();
        request.paths[0] = PathText(path);
        session.Open(request);
        Check(!session.Loaded()->archiveMembers[0].empty(),
              "A sole compatible ROM is selected automatically");
        session.StepFrame();
        Check(session.Machine()->CaptureState() == expected,
              "Every archive format preserves the complete raw-ROM machine");
        const auto selected = *session.Loaded();
        session.Close();
        auto save = GameAssets::SaveBase(selected, 0);
        save.replace_extension(".srm");
        Check(ReadFile(save, 8192).at(0) == 0x5a && ReadFile(path, 1024 * 1024) == before,
              "Archive saves persist without changing or unpacking the source");
        session.Open(selected);
        Check(session.Machine()->LoadedCartridge()->SramData()[0] == 0x5a,
              "Reopening the selected archive entry restores its save");
    }
}

void Choices(const fs::path& home) {
    const auto path = home / "collection.zip";
    Archive(path, "zip",
            {{"first/game.sfc", ConsoleImage(0x2a)}, {"second/game.sfc", ConsoleImage(0xb5)}});
    LaunchRequest request;
    request.paths[0] = PathText(path);
    const auto unresolved = GameAssets::Resolve(request);
    Check(!unresolved.Ready() && unresolved.choices.size() == 2 && unresolved.asset == 0,
          "Multiple compatible ROMs require a visible selection");
    auto session = NewSession();
    Reject([&] { session.Open(request); }, "A caller cannot silently choose the first ROM");
    request.archiveMembers[0] = "second/game.sfc";
    session.Open(request);
    session.StepFrame();
    Check(session.Machine()->LoadedCartridge()->SramData()[0] == 0xb5,
          "The chosen member supplies the loaded cartridge");
    session.Close();
    const auto secondStates = StateStorage(request).Directory();
    const auto secondSave = GameAssets::SaveBase(request, 0);
    PreferencesFile profile(home / "archive-profile.cfg");
    auto preferences = profile.Load();
    preferences.Remember(request);
    profile.Save(preferences);
    const auto remembered = PreferencesFile(profile.Path()).Load().recent.at(0);
    Check(remembered.archiveMembers == request.archiveMembers &&
              GameAssets::Resolve(remembered).Ready(),
          "Recent games retain the exact archive member across profile reloads");
    request.archiveMembers[0] = "first/game.sfc";
    Check(StateStorage(request).Directory() != secondStates &&
              GameAssets::SaveBase(request, 0) != secondSave,
          "Same-basename ROMs own separate checkpoints, cheats, movies and battery files");
    session.Open(request);
    session.StepFrame();
    Check(session.Machine()->LoadedCartridge()->SramData()[0] == 0x2a,
          "Another member never reads the previous member's save");
    const auto before = session.Machine()->CaptureState();
    auto* machine = session.Machine();
    request.archiveMembers[0] = "missing.sfc";
    Reject([&] { session.Open(request); }, "Missing remembered members reject");
    Check(session.Machine() == machine && machine->CaptureState() == before,
          "A bad archive replacement retains the complete running machine");
}

void InvalidAndSafe(const fs::path& home) {
    const auto path = home / "broken.zip";
    Archive(path, "zip", {{"game.sfc", ConsoleImage()}});
    auto bytes = ReadFile(path, 1024 * 1024);
    const auto image = ConsoleImage();
    auto found = std::search(bytes.begin(), bytes.end(), image.begin(), image.end());
    Check(found != bytes.end(), "Stored fixture exposes its payload for a checksum fault");
    found[32] ^= 0x80;
    Write(path, bytes);
    Reject([&] { RomArchive::Read(path, "game.sfc"); },
           "A corrupt ROM checksum rejects before publication");
    Archive(path, "zip", {{"same.sfc", ConsoleImage()}, {"same.sfc", ConsoleImage(1)}});
    LaunchRequest request;
    request.paths[0] = PathText(path);
    Reject([&] { GameAssets::Resolve(request); },
           "Duplicate ROM names reject instead of reopening an ambiguous entry");
    Archive(path, "tar", {{"link.sfc", {}, true}, {"notes.txt", {1, 2}}});
    Check(RomArchive::List(path).size() == 1, "Archive links do not become ROM candidates");
    Reject([&] { GameAssets::Resolve(request); },
           "Archives without compatible ROMs report an error");
    Archive(path, "zip", {{"../../outside.sfc", ConsoleImage()}});
    auto session = NewSession();
    session.Open(request);
    session.StepFrame();
    const auto selected = *session.Loaded();
    session.Close();
    Check(GameAssets::SaveBase(selected, 0).parent_path() == home / "archive-saves" &&
              !fs::exists(home.parent_path() / "outside.sfc") && !fs::exists(home / "outside.sfc"),
          "Member paths never extract files or escape the archive save directory");
    auto large = ConsoleImage();
    large.resize(1024 * 1024);
    Archive(path, "7z", {{"cancel.sfc", large}});
    bool cancelled = false;
    try {
        RomArchive::Read(path, "cancel.sfc", StateOperation([](const StateProgress& progress) {
                             return progress.item != "cancel.sfc" || progress.completed < 65536;
                         }));
    } catch (const StateCancelled&) {
        cancelled = true;
    }
    Check(cancelled && RomArchive::Read(path, "cancel.sfc") == large,
          "Cancelled reads release the decoder and can be retried");
}

void UnicodeAndAdjacent(const fs::path& home) {
    const auto path = home / fs::path(u8"\u65e5\u672c\u8a9e.zip");
    const auto member = PathText(fs::path(u8"folder/\u30b2\u30fc\u30e0.sfc"));
    Archive(path, "zip", {{member, ConsoleImage()}});
    LaunchRequest request;
    request.paths[0] = PathText(path);
    auto session = NewSession();
    session.Open(request);
    Check(session.Loaded()->archiveMembers[0] == member,
          "Archive and member filenames preserve Unicode");
    auto media = GameAssets::Origin(*session.Loaded(), 0);
    media.replace_extension(".msu");
    Write(media, std::vector<uint8_t>{1, 2, 3});
    fs::create_directories(home / "firmware");
    Write(home / "firmware/dsp1.rom", std::vector<uint8_t>{1});
    const auto before = session.Machine();
    Reject([&] { session.Open(request); }, "Adjacent firmware validation also applies to archives");
    Check(session.Machine() == before, "A bad adjacent asset keeps the running archive game");
    Write(home / "firmware/dsp1.rom",
          std::vector<uint8_t>(CartridgeFirmware::Size(FirmwareKind::Dsp1)));
    session.Open(request);
    Check(session.Machine()->GetBus().Read(0x2002) == 'S',
          "Archived ROMs attach external media using the member basename");
}

void LaunchModes(const fs::path& home) {
    using namespace snes::tests::cartridge_state;
    const auto path = home / "modes.7z";
    const auto run = [&](LaunchRequest request, const std::vector<File>& files) {
        Archive(path, "7z", files);
        for (size_t index = 0; index < files.size(); ++index) {
            request.paths[index] = PathText(path);
            request.archiveMembers[index] = files[index].name;
        }
        auto packed = NewSession();
        packed.Open(request);
        LaunchRequest plain = request;
        for (size_t index = 0; index < files.size(); ++index) {
            const auto target =
                home / (std::to_string(index) + fs::path(files[index].name).extension().string());
            Write(target, files[index].bytes);
            plain.paths[index] = PathText(target);
            plain.archiveMembers[index].clear();
        }
        auto direct = NewSession();
        direct.Open(plain);
        for (unsigned frame = 0; frame < 2; ++frame) {
            packed.StepFrame();
            direct.StepFrame();
        }
        Check(packed.Machine()->CaptureState() == direct.Machine()->CaptureState(),
              "All cartridge modes retain the complete raw-file machine when their assets are "
              "archived");
    };
    LaunchRequest request;
    const auto boot = snes::tests::handheld::Boot();
    request.kind = LaunchRequest::Kind::Handheld;
    run(request,
        {{"bios.sfc", snes::tests::cartridge_state::Image(0x20, 0xe3, 0x80000, "Super GAMEBOY2")},
         {"game.gb", snes::tests::handheld::Image()},
         {"boot.bin", std::vector<uint8_t>(boot.begin(), boot.end())}});
    request.kind = LaunchRequest::Kind::Broadcast;
    run(request, {{"base.sfc", SlotBase(Board::BsxFlash)}, {"pack.bs", Pack(Board::BsxFlash)}});
    request.kind = LaunchRequest::Kind::SufamiTurbo;
    auto bios = snes::tests::cartridge_state::Image(0x20, 0, 0x40000);
    std::copy_n("BANDAI SFC-ADX", 14, bios.begin());
    std::copy_n("SFC-ADX BACKUP", 14, bios.begin() + 0x10);
    std::vector<uint8_t> game(0x20000);
    std::copy_n("BANDAI SFC-ADX", 14, game.begin());
    game[0x37] = 8;
    run(request, {{"bios.sfc", bios}, {"one.st", game}, {"two.st", game}});
    for (auto kind : {LaunchRequest::Kind::Campus92, LaunchRequest::Kind::PowerFest94}) {
        request.kind = kind;
        run(request, {{"boot.sfc", ConsoleImage()},
                      {"one.sfc", ConsoleImage()},
                      {"two.sfc", ConsoleImage()},
                      {"three.sfc", ConsoleImage()}});
    }
}
} // namespace

int main() {
    const auto home =
        fs::temp_directory_path() / ("cupid-archive-" + std::to_string(SDL_GetTicksNS()));
    try {
        fs::create_directories(home);
        Formats(home);
        Choices(home);
        InvalidAndSafe(home);
        LaunchModes(home);
        UnicodeAndAdjacent(home);
        Check(fs::weakly_canonical(home).parent_path() ==
                  fs::weakly_canonical(fs::temp_directory_path()),
              "Archive test cleanup stays in its temporary directory");
        fs::remove_all(home);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "Archive tests: %s\n", failure.what());
        return 1;
    }
    std::puts("Archive checks passed");
    return 0;
}
