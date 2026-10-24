#include "AtomicFile.hpp"
#include "snes/core/FilePath.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <stdexcept>

namespace snes::frontend {
std::vector<uint8_t> ReadFile(const std::filesystem::path &path, size_t maximumSize,
                              const core::StateOperation &operation) {
  const auto label = operation ? core::PathText(path.filename()) : std::string{};
  operation.Report(core::StatePhase::ReadFile, 0, 0, label);
  if (!std::filesystem::exists(path))
    return {};
  const auto size = std::filesystem::file_size(path);
  if (size > maximumSize)
    throw std::runtime_error("File is too large: " + core::PathText(path));
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("Cannot read file: " + core::PathText(path));
  for (size_t offset = 0; offset < bytes.size(); offset += 65536) {
    operation.Report(core::StatePhase::ReadFile, offset, bytes.size(), label);
    const auto count = std::min<size_t>(65536, bytes.size() - offset);
    if (!file.read(reinterpret_cast<char *>(bytes.data() + offset), std::streamsize(count)))
      throw std::runtime_error("Cannot read file: " + core::PathText(path));
  }
  operation.Report(core::StatePhase::ReadFile, bytes.size(), bytes.size(), label);
  return bytes;
}

void WriteFileAtomically(const std::filesystem::path &path, std::span<const uint8_t> bytes,
                         const core::StateOperation &operation) {
  const auto label = operation ? core::PathText(path.filename()) : std::string{};
  operation.Report(core::StatePhase::WriteFile, 0, bytes.size(), label);
  static std::atomic<unsigned> serial{0};
  auto temporary = path;
  temporary += ".tmp-" + std::to_string(SDL_GetTicksNS()) + "-" + std::to_string(serial++);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup{temporary};
  std::ofstream file(temporary, std::ios::binary);
  if (!file)
    throw std::runtime_error("Cannot create file: " + core::PathText(temporary));
  for (size_t offset = 0; offset < bytes.size(); offset += 65536) {
    operation.Report(core::StatePhase::WriteFile, offset, bytes.size(), label);
    const auto count = std::min<size_t>(65536, bytes.size() - offset);
    file.write(reinterpret_cast<const char *>(bytes.data() + offset), std::streamsize(count));
    if (!file)
      throw std::runtime_error("Cannot write file: " + core::PathText(path));
  }
  file.close();
  if (!file)
    throw std::runtime_error("Cannot write file: " + core::PathText(path));
  operation.Report(core::StatePhase::WriteFile, bytes.size(), bytes.size(), label);
  const auto source = core::PathText(temporary), destination = core::PathText(path);
  if (!SDL_RenamePath(source.c_str(), destination.c_str()))
    throw std::runtime_error("Cannot replace file: " + core::PathText(path) + ": " +
                             SDL_GetError());
}
} // namespace snes::frontend
