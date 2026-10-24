#include "snes/core/debug/MemoryInspection.hpp"
#include "snes/core/Emulator.hpp"
#include "snes/core/gameboy/Adapter.hpp"

#include <algorithm>
#include <functional>
#include <span>
#include <stdexcept>
#include <utility>

namespace snes::core {
struct MemoryInspector::View {
    MemoryRegion region;
    std::function<uint8_t(size_t)> read;
};

std::vector<MemoryInspector::View> MemoryInspector::Views(const Emulator& machine) {
    std::vector<View> views;
    if (!machine.initialized_)
        return views;
    const auto add = [&](const char* key, const char* label, const auto& storage,
                         bool readOnly = false, uint32_t base = 0,
                         std::optional<uint32_t> cheatBase = {},
                         CheatSystem system = CheatSystem::Console) {
        const auto data = std::span(storage);
        using Value = typename decltype(data)::value_type;
        if (data.empty())
            return;
        views.push_back({{key, label, data.size_bytes(), base, cheatBase, system, readOnly},
                         [data](size_t offset) {
                             return uint8_t(uint64_t(data[offset / sizeof(Value)]) >>
                                            ((offset % sizeof(Value)) * 8));
                         }});
    };
    const auto signal = [&](const char* key, const char* label, const UpdDsp& processor,
                            size_t size) {
        views.push_back(
            {{key, label, size, 0, {}, CheatSystem::Console, false},
             [&processor](size_t offset) { return processor.ReadRam(uint16_t(offset)); }});
    };
    add("wram", "Console work RAM", std::span(machine.bus_.WramData(), MemoryBus::WramSize), false,
        0x7e0000, 0x7e0000);
    add("apuram", "Audio RAM", std::span(machine.smp_.Ram(), Smp::RamSize));
    add("ipl", "Audio boot ROM", std::span(machine.smp_.IplRom(), Smp::IplRomSize), true, 0xffc0);
    add("vram", "Video RAM",
        std::span(machine.ppu_.VramData(), machine.ppu_.Configuration().vramBytes / 2));
    add("oam", "Object attribute memory", std::span(machine.ppu_.OamData(), Ppu::OamSize));
    add("cgram", "Color RAM", std::span(machine.ppu_.CgramData(), Ppu::CgramColors));
    const auto& cartridge = *machine.cartridge_;
    add("rom", "Cartridge ROM", cartridge.rom_, true);
    add("sram", "Cartridge save RAM", cartridge.sram_);
    add("pack", "Inserted memory pack", cartridge.MemoryPackData(), cartridge.memoryPackReadOnly_);
    if (cartridge.sa1_)
        add("sa1-iram", "SA-1 internal RAM", cartridge.sa1_->iram_);
    if (cartridge.superFx_)
        add("superfx-cache", "Super FX instruction cache", cartridge.superFx_->InspectCache(),
            false, 0x3100);
    if (cartridge.cx4_)
        add("cx4-command", "Graphics command RAM", cartridge.cx4_->ram_);
    if (cartridge.cx4Processor_) {
        const auto& state = cartridge.cx4Processor_->GetState();
        add("cx4-ram", "Graphics processor data RAM", state.dataRam);
        add("cx4-cache0", "Graphics processor cache 0", state.program[0]);
        add("cx4-cache1", "Graphics processor cache 1", state.program[1]);
    }
    if (cartridge.armDsp_)
        add("arm-ram", "ARM work RAM", cartridge.armDsp_->RamData());
    if (cartridge.st011_)
        add("command-work", "Board processor work RAM", cartridge.st011_->work_);
    if (cartridge.necDsp_)
        signal("signal-ram", "Signal processor RAM", cartridge.necDsp_->processor_,
               cartridge.necDsp_->HasSaveRam() ? 4096 : 512);
    if (cartridge.bsx_)
        add("broadcast-psram", "Broadcast program RAM", std::as_const(*cartridge.bsx_).Psram());
    if (cartridge.competition_) {
        const auto& board = *cartridge.competition_;
        add("competition-save", "Competition save RAM", board.save_);
        constexpr const char* keys[]{"competition-rom0", "competition-rom1", "competition-rom2",
                                     "competition-rom3"};
        constexpr const char* labels[]{"Competition boot ROM", "Competition game ROM 1",
                                       "Competition game ROM 2", "Competition game ROM 3"};
        for (size_t index = 0; index < board.images_.size(); ++index)
            add(keys[index], labels[index], board.images_[index], true);
        if (board.processor_)
            signal("competition-dsp", "Competition signal processor RAM", *board.processor_, 512);
    }
    if (cartridge.gameBoy_) {
        const auto& adapter = *cartridge.gameBoy_;
        const auto& handheld = adapter.machine_;
        const auto& video = adapter.devices_.video_;
        add("handheld-rom", "Handheld cartridge ROM", handheld.cartridge_.rom_, true);
        add("handheld-save", "Handheld cartridge RAM", handheld.cartridge_.ram_);
        add("handheld-wram", "Handheld work RAM", handheld.ram_, false, 0xc000, 0xc000,
            CheatSystem::Handheld);
        add("handheld-hram", "Handheld high RAM", handheld.highRam_, false, 0xff80, 0xff80,
            CheatSystem::Handheld);
        add("handheld-boot", "Handheld boot ROM", handheld.boot_, true);
        add("handheld-vram", "Handheld video RAM", video.vram_, false, 0x8000);
        add("handheld-oam", "Handheld object memory", video.oam_.bytes_, false, 0xfe00);
        add("handheld-wave", "Handheld wave RAM", adapter.devices_.audio_.channels_.wave_, false,
            0xff30);
    }
    return views;
}

std::vector<MemoryRegion> MemoryInspector::Regions(const Emulator& machine) {
    auto views = Views(machine);
    std::vector<MemoryRegion> regions;
    regions.reserve(views.size());
    for (auto& view : views)
        regions.push_back(std::move(view.region));
    return regions;
}

std::vector<uint8_t> MemoryInspector::Read(const Emulator& machine, std::string_view region,
                                           size_t offset, size_t count) {
    const auto views = Views(machine);
    const auto view = std::find_if(views.begin(), views.end(),
                                   [&](const auto& item) { return item.region.key == region; });
    if (view == views.end())
        throw std::invalid_argument("Memory region is unavailable on the loaded hardware");
    if (offset > view->region.size || count > view->region.size - offset)
        throw std::out_of_range("Memory range is outside the selected region");
    std::vector<uint8_t> bytes(count);
    for (size_t index = 0; index < count; ++index)
        bytes[index] = view->read(offset + index);
    return bytes;
}
} // namespace snes::core
