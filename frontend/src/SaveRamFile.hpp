// Save-file persistence interface.

#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace snes::frontend {
class SaveRamFile {
public:
  explicit SaveRamFile(std::filesystem::path path) : path_(std::move(path)) {}
  std::vector<uint8_t> Load(size_t expectedSize);
  std::vector<uint8_t> LoadUpTo(size_t maximumSize);
  void Flush(std::span<const uint8_t> data);
  const std::filesystem::path &Path() const noexcept {
    return path_;
  }

private:
  std::vector<uint8_t> Read(size_t size);
  std::filesystem::path path_;
  std::vector<uint8_t> saved_;
};
} // namespace snes::frontend
