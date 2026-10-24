#include "ConsoleConfig.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>

using namespace snes::core;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class Files {
  public:
    Files() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 64; ++i) {
            directory = std::filesystem::current_path() /
                        ("firmware-test-" + std::to_string(stamp) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(directory)) {
                folders_.push_back(directory);
                return;
            }
        }
        throw std::runtime_error("Cannot create firmware test files");
    }

    ~Files() {
        std::error_code error;
        for (const auto& path : files_)
            std::filesystem::remove(path, error);
        for (auto it = folders_.rbegin(); it != folders_.rend(); ++it)
            std::filesystem::remove(*it, error);
    }

    std::filesystem::path Folder(const char* name) {
        auto path = directory / name;
        std::filesystem::create_directory(path);
        folders_.push_back(path);
        return path;
    }

    void Write(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
        if (std::find(files_.begin(), files_.end(), path) == files_.end())
            files_.push_back(path);
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.close();
        Check(bool(file), "Write firmware fixture");
    }

    std::filesystem::path directory;

  private:
    std::vector<std::filesystem::path> files_, folders_;
};

std::vector<uint8_t> Firmware(FirmwareKind kind, uint8_t value) {
    std::vector<uint8_t> bytes(CartridgeFirmware::Size(kind));
    const uint32_t instructions[]{0xc00000 | (0x400 << 6) | 7,
                                  0xc00000 | (uint32_t(value) << 6) | 6};
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned b = 0; b < 3; ++b)
            bytes[i * 3 + b] = static_cast<uint8_t>(instructions[i] >> (b * 8));
    return bytes;
}

std::vector<uint8_t> Rom(bool seta = false) {
    std::vector<uint8_t> rom(seta ? 0x10000 : 0x8000);
    std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
    const std::string title = seta ? "F1 ROC II" : "PILOTWINGS";
    std::copy(title.begin(), title.end(), rom.begin() + 0x7fc0);
    rom[0x7fd5] = seta ? 0x30 : 0x20;
    rom[0x7fd6] = seta ? 0xf6 : 3;
    rom[0x7fbf] = seta ? 1 : 0;
    rom[0x7fd7] = seta ? 6 : 5;
    rom[0x7fd9] = 1;
    rom[0x7fdc] = rom[0x7fdd] = 0xff;
    rom[0x7ffd] = 0x80;
    rom[0] = 0xdb;
    return rom;
}

void CatalogValidation() {
    CartridgeFirmware catalog;
    const auto original = Firmware(FirmwareKind::Dsp1, 0x45);
    Check(catalog.Set(FirmwareKind::Dsp1, original), "A complete program enters the catalog");
    std::string error;
    Check(
        !catalog.Set(FirmwareKind::Dsp1, std::span<const uint8_t>(original).first(8191), &error) &&
            !error.empty() &&
            std::equal(original.begin(), original.end(), catalog.Find(FirmwareKind::Dsp1).begin()),
        "Bad sizes preserve existing firmware");
    Check(!catalog.Set(FirmwareKind::Count, original, &error),
          "Invalid firmware kinds are rejected");
    Files files;
    const auto firmware = files.Folder("firmware");
    files.Write(firmware / "dsp1.rom", Firmware(FirmwareKind::Dsp1, 0x72));
    files.Write(firmware / "dsp2.rom", std::vector<uint8_t>(3));
    Check(
        !catalog.LoadDirectory(firmware, &error) &&
            std::equal(original.begin(), original.end(), catalog.Find(FirmwareKind::Dsp1).begin()),
        "A failed directory load preserves the entire catalog");
    files.Write(firmware / "dsp2.rom", Firmware(FirmwareKind::Dsp2, 0x39));
    Check(catalog.LoadDirectory(firmware, &error) &&
              catalog.Find(FirmwareKind::Dsp2).size() == 8192,
          "A directory can supply a subset of supported firmware files");
}

void AdjacentFiles() {
    Files files;
    const auto firmware = files.Folder("firmware");
    const auto rom = files.directory / "game.sfc";
    files.Write(rom, Rom());
    files.Write(firmware / "dsp1.rom", Firmware(FirmwareKind::Dsp1, 0x79));
    auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    std::string error;
    Check(machine->LoadCartridgeFromFile(rom.string(), &error) &&
              machine->LoadedCartridge()->HasSignalProcessor(),
          "File loading discovers the adjacent firmware directory");
    machine->StepFrame();
    Check(machine->GetBus().Read(0x308000) == 0x79,
          "Adjacent firmware executes through the CPU data port");
    machine->GetBus().Write(0x7e1234, 0x5a);
    const auto frame = machine->CurrentFrame();
    files.Write(firmware / "dsp1.rom", std::vector<uint8_t>(3));
    Check(!machine->LoadCartridgeFromFile(rom.string(), &error) &&
              machine->CurrentFrame() == frame && machine->GetBus().Read(0x7e1234) == 0x5a,
          "An invalid firmware replacement leaves the running cartridge intact");
    const auto other = files.Folder("other") / "game.sfc";
    files.Write(other, Rom());
    Check(machine->LoadCartridgeFromFile(other.string(), &error) &&
              !machine->LoadedCartridge()->HasSignalProcessor(),
          "Automatically discovered firmware does not leak into another ROM directory");
}

void EmbeddedImages() {
    for (bool seta : {false, true}) {
        const auto kind = seta ? FirmwareKind::St010 : FirmwareKind::Dsp1;
        auto bytes = Rom(seta);
        const auto base = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
        Check(base.has_value(), "A base cartridge loads without firmware");
        const auto baseSize = bytes.size();
        const auto firmware = Firmware(kind, 0x6a);
        bytes.insert(bytes.end(), firmware.begin(), firmware.end());
        auto catalog = std::make_shared<CartridgeFirmware>();
        Check(catalog->Set(kind, Firmware(kind, 0x18)), "Explicit alternate firmware is available");
        auto config = snes::tests::DeterministicConsoleConfig();
        config.cartridgeFirmware = catalog;
        auto machine = std::make_unique<Emulator>(config);
        Check(machine->LoadCartridge(bytes) && machine->LoadedCartridge()->HasSignalProcessor() &&
                  machine->LoadedCartridge()->RomData().size() == baseSize &&
                  machine->LoadedCartridge()->RomCrc32() == base->RomCrc32(),
              "A complete cartridge image separates processor firmware from the CPU ROM and CRC");
        machine->StepFrame();
        Check(machine->GetBus().Read(seta ? 0x600000 : 0x308000) == 0x6a,
              "Embedded cartridge firmware takes precedence over external files");
    }
}

std::vector<uint8_t> RequiredRom(FirmwareKind kind) {
    auto rom = Rom();
    rom.resize(0x80000);
    rom[0x7fd5] = 0x30;
    rom[0x7fd7] = 9;
    if (kind == FirmwareKind::Dsp3) {
        rom[0x7fd6] = 5;
        rom[0x7fda] = 0xb2;
        std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
        const std::string title = "SD\xb6\xde\xdd\xc0\xde\xd1GX";
        std::copy(title.begin(), title.end(), rom.begin() + 0x7fc0);
    } else if (kind == FirmwareKind::St011) {
        rom[0x7fd6] = 0xf6;
        rom[0x7fbf] = 1;
        std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
        const std::string title = "2DAN MORITA SHOUGI";
        std::copy(title.begin(), title.end(), rom.begin() + 0x7fc0);
    } else {
        rom[0x7fd6] = 0xf5;
        rom[0x7fbf] = 2;
        rom[0x7fd8] = 3;
    }
    return rom;
}

std::vector<uint8_t> RequiredFirmware(FirmwareKind kind, uint8_t value) {
    if (kind != FirmwareKind::St018)
        return Firmware(kind, value);
    std::vector<uint8_t> bytes(CartridgeFirmware::Size(kind));
    const uint32_t program[]{0xe3a00101, 0xe3a01000 | value, 0xe5801000, 0xeafffffe};
    for (unsigned i = 0; i < std::size(program); ++i)
        for (unsigned byte = 0; byte < 4; ++byte)
            bytes[i * 4 + byte] = uint8_t(program[i] >> (byte * 8));
    return bytes;
}

void RequiredFirmwareLoads() {
    for (auto kind : {FirmwareKind::Dsp3, FirmwareKind::St011, FirmwareKind::St018}) {
        const auto rom = RequiredRom(kind);
        Files files;
        const auto path = files.directory / "game.sfc";
        files.Write(path, rom);
        auto unrelated = std::make_shared<CartridgeFirmware>();
        Check(unrelated->Set(FirmwareKind::Dsp1, Firmware(FirmwareKind::Dsp1, 0x18)),
              "The catalog contains firmware for a different processor");
        auto config = snes::tests::DeterministicConsoleConfig();
        config.cartridgeFirmware = unrelated;
        auto machine = std::make_unique<Emulator>(config);
        auto ordinary = Rom();
        ordinary[0x7fd6] = 2;
        ordinary[0x7fd8] = 3;
        Check(machine->LoadCartridge(ordinary),
              "An ordinary cartridge needs no processor firmware");
        machine->StepFrame();
        auto& bus = machine->GetBus();
        bus.Write(0x7e1234, 0x5a);
        bus.Write(0x700123, 0x6b);
        const auto frame = machine->CurrentFrame();
        const auto clocks = machine->CurrentMasterCycles();
        const auto crc = machine->LoadedCartridge()->RomCrc32();
        std::string error;
        Check(!Cartridge::FromRomImage(rom, nullptr, nullptr, &error),
              "A processor without a complete command model requires cartridge firmware");
        Check(error.find(CartridgeFirmware::FileName(kind)) != std::string::npos,
              "The missing-firmware error names the required file");
        for (bool fromFile : {false, true}) {
            error.clear();
            const bool loaded = fromFile ? machine->LoadCartridgeFromFile(path.string(), &error)
                                         : machine->LoadCartridge(rom, &error);
            Check(!loaded && error.find(CartridgeFirmware::FileName(kind)) != std::string::npos,
                  "Memory and file loaders report missing firmware even with an unrelated catalog");
            Check(
                machine->CurrentFrame() == frame && machine->CurrentMasterCycles() == clocks &&
                    machine->LoadedCartridge()->RomCrc32() == crc && bus.Read(0x7e1234) == 0x5a &&
                    bus.Read(0x700123) == 0x6b,
                "Missing firmware preserves the running cartridge, clocks, work RAM, and save RAM");
        }

        auto catalog = std::make_shared<CartridgeFirmware>();
        Check(catalog->Set(kind, RequiredFirmware(kind, 0x18)),
              "Generated firmware enters the catalog");
        auto external = Cartridge::FromRomImage(rom, nullptr, nullptr, &error, catalog.get());
        Check(external && (kind == FirmwareKind::St018 ? external->HasArmProcessor()
                                                       : external->HasSignalProcessor()),
              "Supplied firmware selects the cartridge processor");
        external->AdvanceHardware(70000);
        const uint32_t port = kind == FirmwareKind::Dsp3    ? 0x308000
                              : kind == FirmwareKind::St011 ? 0x600000
                                                            : 0x3800;
        Check(external->Read(port) == 0x18, "The selected processor executes supplied firmware");

        auto embedded = rom;
        const auto program = RequiredFirmware(kind, 0x6a);
        embedded.insert(embedded.end(), program.begin(), program.end());
        auto combined = Cartridge::FromRomImage(embedded, nullptr, nullptr, &error, catalog.get());
        Check(combined && combined->RomData().size() == rom.size() &&
                  combined->RomCrc32() == external->RomCrc32(),
              "Required embedded firmware is separated from the cartridge ROM and checksum");
        combined->AdvanceHardware(70000);
        Check(combined->Read(port) == 0x6a,
              "Required embedded firmware takes precedence over the catalog");

        files.Write(files.Folder("firmware") / CartridgeFirmware::FileName(kind), program);
        auto adjacent = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
        Check(adjacent->LoadCartridgeFromFile(path.string(), &error),
              "The file loader discovers required firmware beside the game");
        adjacent->StepFrame();
        Check(adjacent->GetBus().Read(port) == 0x6a,
              "Adjacent required firmware runs through the console bus");
    }
}
} // namespace

int main() {
    try {
        CatalogValidation();
        AdjacentFiles();
        EmbeddedImages();
        RequiredFirmwareLoads();
        std::puts("Cartridge firmware asset tests passed");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
