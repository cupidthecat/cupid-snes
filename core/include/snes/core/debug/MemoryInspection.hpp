#pragma once

#include "snes/core/debug/Cheat.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snes::core {
class Emulator;

struct MemoryRegion {
  std::string key, label;
  size_t size = 0;
  uint32_t base = 0;
  std::optional<uint32_t> cheatBase;
  CheatSystem cheatSystem = CheatSystem::Console;
  bool readOnly = false;
};

class MemoryInspector {
public:
  // Read current storage without clocking devices, polling input or reading I/O ports.
  // Offsets are physical bytes in the named region. Words use little-endian byte order.
  static std::vector<MemoryRegion> Regions(const Emulator &machine);
  static std::vector<uint8_t> Read(const Emulator &machine, std::string_view region, size_t offset,
                                   size_t count);

private:
  struct View;
  static std::vector<View> Views(const Emulator &machine);
};
} // namespace snes::core
