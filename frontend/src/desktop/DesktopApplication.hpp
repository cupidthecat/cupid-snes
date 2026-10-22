#pragma once

#include "session/Session.hpp"
#include <optional>

namespace snes::frontend {
int RunDesktop(core::Emulator::Config config, const DeviceOverrides &overrides,
               const std::optional<LaunchRequest> &initial,
               const std::filesystem::path &preferencesPath = {});
} // namespace snes::frontend
