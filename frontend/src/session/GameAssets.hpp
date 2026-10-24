#pragma once

#include "LaunchRequest.hpp"
#include "storage/RomArchive.hpp"

namespace snes::frontend {
struct ArchivePlan {
    LaunchRequest request;
    unsigned asset = 0;
    std::vector<RomArchive::Entry> choices;

    bool Ready() const noexcept {
        return choices.empty();
    }
};

class GameAssets {
  public:
    static ArchivePlan Resolve(LaunchRequest request, const core::StateOperation& operation = {});
    static bool Archived(const LaunchRequest& request) noexcept;
    static std::vector<uint8_t> Read(const LaunchRequest& request, unsigned asset,
                                     const core::StateOperation& operation = {});
    static std::filesystem::path Origin(const LaunchRequest& request, unsigned asset);
    static std::filesystem::path SaveBase(const LaunchRequest& request, unsigned asset);
    static std::string Label(const LaunchRequest& request, unsigned asset);
};
} // namespace snes::frontend
