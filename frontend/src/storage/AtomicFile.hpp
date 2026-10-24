#pragma once

#include "snes/core/StateOperation.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace snes::frontend {
std::vector<uint8_t> ReadFile(const std::filesystem::path& path, size_t maximumSize,
                              const core::StateOperation& operation = {});
void WriteFileAtomically(const std::filesystem::path& path, std::span<const uint8_t> bytes,
                         const core::StateOperation& operation = {});
// Fill every requested chunk on the owning thread. A failure or cancellation
// removes only this write's temporary file and retains the previous destination.
void WriteFileAtomically(const std::filesystem::path& path, uint64_t size,
                         const std::function<void(uint64_t, std::span<uint8_t>)>& fill,
                         const core::StateOperation& operation = {});
} // namespace snes::frontend
