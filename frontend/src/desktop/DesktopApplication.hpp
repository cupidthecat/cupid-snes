#pragma once

#include "session/Session.hpp"
#include <optional>

namespace snes::frontend {
int RunDesktop(core::Emulator::Config config, std::array<core::ControllerDevice, 2> devices,
               const std::optional<LaunchRequest> &initial,
               const std::filesystem::path &preferencesPath = {});
} // namespace snes::frontend
