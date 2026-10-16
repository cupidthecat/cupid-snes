#include "snes/core/CartridgeFirmware.hpp"

#include <fstream>
#include <iterator>

namespace snes::core {
const char *CartridgeFirmware::FileName(FirmwareKind kind) {
  constexpr std::array names{"dsp1.rom",  "dsp1b.rom", "dsp2.rom",  "dsp3.rom",    "dsp4.rom",
                             "st010.rom", "st011.rom", "st018.rom", "cx4.data.rom"};
  return names.at(static_cast<size_t>(kind));
}

size_t CartridgeFirmware::Size(FirmwareKind kind) {
  if (kind == FirmwareKind::Cx4)
    return 0xc00;
  if (kind == FirmwareKind::St018)
    return 0x28000;
  return kind == FirmwareKind::St010 || kind == FirmwareKind::St011 ? 0xd000 : 0x2000;
}

bool CartridgeFirmware::Set(FirmwareKind kind, std::span<const uint8_t> bytes, std::string *error) {
  if (static_cast<size_t>(kind) >= images_.size() || bytes.size() != Size(kind)) {
    if (error)
      *error = "Invalid cartridge processor firmware size";
    return false;
  }
  images_[static_cast<size_t>(kind)].assign(bytes.begin(), bytes.end());
  return true;
}

std::span<const uint8_t> CartridgeFirmware::Find(FirmwareKind kind) const {
  return images_.at(static_cast<size_t>(kind));
}

bool CartridgeFirmware::LoadDirectory(const std::filesystem::path &directory, std::string *error) {
  std::error_code code;
  if (!std::filesystem::is_directory(directory, code)) {
    if (error)
      *error = "Cannot open firmware directory: " + directory.string();
    return false;
  }
  auto loaded = *this;
  for (size_t i = 0; i < images_.size(); ++i) {
    const auto kind = static_cast<FirmwareKind>(i);
    const auto path = directory / FileName(kind);
    if (!std::filesystem::exists(path, code)) {
      if (!code)
        continue;
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (!file || bytes.size() != Size(kind)) {
      if (error)
        *error = "Invalid or unreadable firmware file: " + path.string();
      return false;
    }
    loaded.images_[i] = std::move(bytes);
  }
  *this = std::move(loaded);
  return true;
}
} // namespace snes::core
