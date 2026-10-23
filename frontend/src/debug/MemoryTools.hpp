#pragma once

#include "RamSearch.hpp"
#include "cheats/CheatBook.hpp"
#include "snes/core/debug/MemoryInspection.hpp"

namespace snes::core {
class Emulator;
}

namespace snes::frontend {
class MemoryTools {
  public:
    explicit MemoryTools(const core::Emulator& machine);

    const std::vector<core::MemoryRegion>& Regions() const noexcept {
        return regions_;
    }

    const core::MemoryRegion& Selected() const;
    void Select(std::string_view key);
    std::vector<uint8_t> Read(size_t offset, size_t count) const;
    void Start(RamSearch::Config config);
    void Filter(RamSearch::Filter filter);

    RamSearch& Search() noexcept {
        return search_;
    }

    const RamSearch& Search() const noexcept {
        return search_;
    }

    CheatEntry MakeCheat(const RamSearch::Result& result, std::string name) const;
    bool CanCreateCheat() const noexcept;

  private:
    const core::Emulator& machine_;
    std::vector<core::MemoryRegion> regions_;
    size_t selected_ = 0;
    RamSearch search_;
};
} // namespace snes::frontend
