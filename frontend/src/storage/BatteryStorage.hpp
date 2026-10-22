#pragma once

#include "SaveRamFile.hpp"
#include "session/LaunchRequest.hpp"
#include "snes/core/Emulator.hpp"

#include <array>
#include <filesystem>
#include <memory>

namespace snes::frontend {
class BatteryStorage {
public:
  BatteryStorage(const LaunchRequest &request, core::Emulator &machine,
                 const std::filesystem::path &directory);
  void Load(core::Emulator &machine);
  void Flush(core::Emulator &machine);

private:
  enum File { Ram, Clock, Pack, PackMetadata, SlotA, SlotB, Count };
  LaunchRequest::Kind kind_;
  std::array<std::unique_ptr<SaveRamFile>, Count> files_;
};
} // namespace snes::frontend
