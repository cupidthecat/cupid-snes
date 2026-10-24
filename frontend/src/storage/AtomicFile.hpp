#pragma once

#include "snes/core/StateOperation.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace snes::frontend {
std::vector<uint8_t> ReadFile(const std::filesystem::path &path, size_t maximumSize,
                              const core::StateOperation &operation = {});
void WriteFileAtomically(const std::filesystem::path &path, std::span<const uint8_t> bytes,
                         const core::StateOperation &operation = {});
} // namespace snes::frontend
