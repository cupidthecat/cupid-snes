#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace snes::frontend {
struct LaunchRequest {
  enum class Kind { Cartridge, Handheld, SufamiTurbo, Broadcast, Campus92, PowerFest94 };
  Kind kind = Kind::Cartridge;
  std::array<std::string, 4> paths;
  uint8_t competitionDip = 0;
  bool operator==(const LaunchRequest &) const = default;
};
} // namespace snes::frontend
