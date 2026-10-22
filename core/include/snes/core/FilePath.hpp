#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace snes::core {
// File loaders and platform text use UTF-8, including on Windows.
inline std::filesystem::path FilePath(std::string_view text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
inline std::string PathText(const std::filesystem::path &path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}
} // namespace snes::core
