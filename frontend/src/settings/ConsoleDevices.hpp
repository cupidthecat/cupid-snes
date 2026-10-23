#pragma once

#include "snes/core/Emulator.hpp"
#include <array>
#include <optional>

namespace snes::frontend {
struct ConsoleDevices {
    std::array<core::ControllerDevice, 2> ports{core::ControllerDevice::Gamepad,
                                                core::ControllerDevice::Gamepad};
    core::Emulator::ExpansionDevice expansion = core::Emulator::ExpansionDevice::None;
    void Validate() const;
    bool operator==(const ConsoleDevices&) const = default;
};

struct DeviceOverrides {
    std::array<std::optional<core::ControllerDevice>, 2> ports;
    std::optional<core::Emulator::ExpansionDevice> expansion;
    ConsoleDevices Resolve(ConsoleDevices saved) const;
};
} // namespace snes::frontend
