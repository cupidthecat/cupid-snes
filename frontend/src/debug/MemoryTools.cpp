#include "MemoryTools.hpp"
#include "snes/core/Emulator.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
MemoryTools::MemoryTools(const core::Emulator& machine)
    : machine_(machine), regions_(core::MemoryInspector::Regions(machine)) {
    if (regions_.empty())
        throw std::runtime_error("Memory tools require a loaded game");
    Select(machine.CheatsSystem() == core::CheatSystem::Handheld ? "handheld-wram" : "wram");
}

const core::MemoryRegion& MemoryTools::Selected() const {
    return regions_.at(selected_);
}

void MemoryTools::Select(std::string_view key) {
    const auto region = std::find_if(regions_.begin(), regions_.end(),
                                     [&](const auto& item) { return item.key == key; });
    if (region == regions_.end())
        throw std::invalid_argument("Memory region is unavailable on the loaded hardware");
    const auto index = size_t(region - regions_.begin());
    if (index != selected_) {
        selected_ = index;
        search_.Clear();
    }
}

std::vector<uint8_t> MemoryTools::Read(size_t offset, size_t count) const {
    return core::MemoryInspector::Read(machine_, Selected().key, offset, count);
}

void MemoryTools::Start(RamSearch::Config config) {
    const auto bytes = Read(0, Selected().size);
    search_.Start(bytes, config);
}

void MemoryTools::Filter(RamSearch::Filter filter) {
    const auto bytes = Read(0, Selected().size);
    search_.Refine(bytes, filter);
}

bool MemoryTools::CanCreateCheat() const noexcept {
    return regions_[selected_].cheatBase.has_value() &&
           regions_[selected_].cheatSystem == machine_.CheatsSystem();
}

CheatEntry MemoryTools::MakeCheat(const RamSearch::Result& result, std::string name) const {
    if (!CanCreateCheat() || !search_.Active())
        throw std::invalid_argument(
            "This physical region has no direct cheat address for the loaded system");
    const auto& region = Selected();
    const auto config = search_.Configuration();
    if (result.offset > region.size || config.bytes > region.size - result.offset)
        throw std::out_of_range("Search result is outside the selected memory region");
    CheatEntry entry{std::move(name), {}, false};
    const auto value = uint32_t(result.current);
    for (unsigned byte = 0; byte < config.bytes; ++byte) {
        const auto shift = config.bigEndian ? config.bytes - byte - 1 : byte;
        if (!entry.codes.empty())
            entry.codes += '+';
        entry.codes += core::FormatCheat(
            {*region.cheatBase + result.offset + byte, uint8_t(value >> (shift * 8)), {}},
            region.cheatSystem);
    }
    return entry;
}
} // namespace snes::frontend
