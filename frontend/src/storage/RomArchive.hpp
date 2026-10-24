#pragma once

#include "snes/core/StateOperation.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace snes::frontend {
class RomArchive {
  public:
    struct Entry {
        std::string name;
        uint64_t bytes = 0;
        bool operator==(const Entry&) const = default;
    };

    static constexpr size_t maximumMemberBytes = size_t(128) * 1024 * 1024;
    static bool Recognizes(const std::filesystem::path& path);
    static std::vector<Entry> List(const std::filesystem::path& path,
                                   const core::StateOperation& operation = {});
    static std::vector<uint8_t> Read(const std::filesystem::path& path, const std::string& member,
                                     const core::StateOperation& operation = {});
};
} // namespace snes::frontend
