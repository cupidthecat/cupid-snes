#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace snes::frontend {
std::vector<uint8_t> ReadFile(const std::filesystem::path &path, size_t maximumSize);
void WriteFileAtomically(const std::filesystem::path &path, std::span<const uint8_t> bytes);
} // namespace snes::frontend
