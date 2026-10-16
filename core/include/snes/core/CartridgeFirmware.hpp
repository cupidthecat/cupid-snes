#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace snes::core {

enum class FirmwareKind { Dsp1, Dsp1b, Dsp2, Dsp3, Dsp4, St010, St011, St018, Cx4, Count };

class CartridgeFirmware {
public:
  static const char *FileName(FirmwareKind kind);
  static size_t Size(FirmwareKind kind);
  bool Set(FirmwareKind kind, std::span<const uint8_t> bytes, std::string *error = nullptr);
  bool LoadDirectory(const std::filesystem::path &directory, std::string *error = nullptr);
  std::span<const uint8_t> Find(FirmwareKind kind) const;

private:
  std::array<std::vector<uint8_t>, static_cast<size_t>(FirmwareKind::Count)> images_;
};

} // namespace snes::core
