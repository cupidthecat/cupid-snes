// SDL3 desktop entry point and command-line launch options.

// Usage: snes_frontend [rom_file.smc]

#include "desktop/DesktopApplication.hpp"

#include "snes/core/Emulator.hpp"
#include "snes/core/FilePath.hpp"

#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
    // 1. Parse command-line arguments
    using snes::core::ControllerDevice;
    snes::frontend::DeviceOverrides devices;
    std::vector<std::string> arguments;
    std::filesystem::path firmwareDirectory;
    std::filesystem::path preferencesPath;
    std::optional<uint8_t> competitionDip;
    std::optional<snes::core::Region> regionOverride;
    for (int i = 1; i < argc; ++i) {
        const std::string_view value(argv[i]);
        if (value.starts_with("--preferences=")) {
            preferencesPath = snes::core::FilePath(value.substr(14));
            if (preferencesPath.empty()) {
                std::cerr << "Preferences path must not be empty\n";
                return EXIT_FAILURE;
            }
            continue;
        }
        if (value.starts_with("--region=")) {
            const auto name = value.substr(9);
            if (name == "auto")
                regionOverride.reset();
            else if (name == "ntsc")
                regionOverride = snes::core::Region::NTSC;
            else if (name == "pal")
                regionOverride = snes::core::Region::PAL;
            else {
                std::cerr << "Region must be auto, ntsc, or pal\n";
                return EXIT_FAILURE;
            }
            continue;
        }
        if (value.starts_with("--expansion=")) {
            const auto name = value.substr(12);
            if (name == "none")
                devices.expansion = snes::core::Emulator::ExpansionDevice::None;
            else if (name == "satellaview")
                devices.expansion = snes::core::Emulator::ExpansionDevice::Satellaview;
            else {
                std::cerr << "Unknown expansion device: " << name << '\n';
                return EXIT_FAILURE;
            }
            continue;
        }
        if (value.starts_with("--competition-dip=")) {
            const auto digits = value.substr(18);
            unsigned setting = 0;
            const auto parsed =
                std::from_chars(digits.data(), digits.data() + digits.size(), setting);
            if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
                setting > 255) {
                std::cerr << "Competition DIP setting must be between 0 and 255\n";
                return EXIT_FAILURE;
            }
            competitionDip = uint8_t(setting);
            continue;
        }
        if (value.starts_with("--firmware-dir=")) {
            firmwareDirectory = snes::core::FilePath(value.substr(15));
            if (firmwareDirectory.empty()) {
                std::cerr << "Firmware directory must not be empty\n";
                return EXIT_FAILURE;
            }
            continue;
        }
        if (value.starts_with("--port1=") || value.starts_with("--port2=")) {
            auto& device = devices.ports[value[6] - '1'];
            const auto name = value.substr(8);
            if (name == "pad")
                device = ControllerDevice::Gamepad;
            else if (name == "mouse")
                device = ControllerDevice::Mouse;
            else if (name == "multitap")
                device = ControllerDevice::Multitap;
            else if (name == "none")
                device = ControllerDevice::None;
            else if (name == "scope")
                device = ControllerDevice::SuperScope;
            else if (name == "justifier")
                device = ControllerDevice::Justifier;
            else if (name == "justifiers")
                device = ControllerDevice::Justifiers;
            else if (name == "rifle")
                device = ControllerDevice::MacsRifle;
            else {
                std::cerr << "Unknown controller device: " << name << '\n';
                return EXIT_FAILURE;
            }
        } else
            arguments.emplace_back(value);
    }
    const bool sufami = !arguments.empty() && arguments[0] == "--sufami";
    const bool handheld = !arguments.empty() && arguments[0] == "--super-game-boy";
    const bool broadcast = !arguments.empty() && arguments[0] == "--broadcast";
    const bool campus = !arguments.empty() && arguments[0] == "--competition92";
    const bool powerfest = !arguments.empty() && arguments[0] == "--competition94";
    const bool competition = campus || powerfest;
    if ((!sufami && !handheld && !broadcast && !competition && arguments.size() > 1) ||
        (sufami && (arguments.size() < 3 || arguments.size() > 4)) ||
        (handheld && arguments.size() != 4) || (broadcast && arguments.size() != 3) ||
        (competition && arguments.size() != 5) || (competitionDip.has_value() && !competition)) {
        std::cerr
            << "Usage: snes_frontend [--port1=pad|mouse|multitap|none] [--port2=...] "
               "<rom_file.smc>\n"
            << "       snes_frontend [controller options] --sufami <bios.bin> <slot-a.st|-> "
               "[slot-b.st|-]\n"
            << "       snes_frontend [controller options] --broadcast <base.sfc> <pack.bs|->\n";
        std::cerr << "       snes_frontend [controller options] --super-game-boy <bios.sfc> "
                     "<game.gb> <boot.bin>\n";
        std::cerr << "       snes_frontend --competition92|--competition94 <boot.sfc> <game1.sfc> "
                     "<game2.sfc> <game3.sfc> [--competition-dip=0..255]\n";
        std::cerr << "       Port 2 also supports scope, justifier, justifiers, and rifle\n";
        std::cerr << "       --firmware-dir=<directory> supplies cartridge processor firmware\n";
        std::cerr << "       --expansion=none|satellaview selects the console expansion device\n";
        std::cerr << "       --region=auto|ntsc|pal selects the console hardware region\n";
        return EXIT_FAILURE;
    }
    std::optional<snes::frontend::LaunchRequest> initial;
    if (!arguments.empty()) {
        snes::frontend::LaunchRequest request;
        using Kind = snes::frontend::LaunchRequest::Kind;
        request.kind = sufami      ? Kind::SufamiTurbo
                       : handheld  ? Kind::Handheld
                       : broadcast ? Kind::Broadcast
                       : campus    ? Kind::Campus92
                       : powerfest ? Kind::PowerFest94
                                   : Kind::Cartridge;
        const unsigned first = sufami || handheld || broadcast || competition ? 1 : 0;
        for (unsigned index = first; index < arguments.size(); ++index)
            request.paths[index - first] = arguments[index];
        request.competitionDip = competitionDip.value_or(0);
        initial = std::move(request);
    }

    snes::core::Emulator::Config emulatorConfig;
    emulatorConfig.regionOverride = regionOverride;
    if (!firmwareDirectory.empty()) {
        auto firmware = std::make_shared<snes::core::CartridgeFirmware>();
        std::string error;
        if (!firmware->LoadDirectory(firmwareDirectory, &error)) {
            std::cerr << error << '\n';
            return EXIT_FAILURE;
        }
        emulatorConfig.cartridgeFirmware = std::move(firmware);
    }

    return snes::frontend::RunDesktop(std::move(emulatorConfig), devices, initial, preferencesPath);
}
