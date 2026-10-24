#include "snes/core/NecDspBoard.hpp"
#include "snes/core/MemoryBus.hpp"

namespace snes::core {
bool NecDspBoard::SelectsDevice(uint32_t address) const noexcept {
    const unsigned bank = (address >> 16) & 0x7f, offset = address & 0xffff;
    switch (layout_) {
    case Layout::LoRom:
        return bank >= 0x30 && bank <= 0x3f && offset >= 0x8000;
    case Layout::LoRomSmallRam:
        return bank >= 0x20 && bank <= 0x3f && offset >= 0x8000;
    case Layout::LoRomLargeRam:
        return bank >= 0x60 && bank <= 0x6f && offset < 0x8000;
    case Layout::HiRom:
        return bank <= 0x1f && offset >= 0x6000 && offset <= 0x7fff;
    case Layout::Seta:
        return (address & 0x788000) == 0x680000 ||
               (bank >= 0x60 && bank <= 0x67 && offset < 0x4000);
    }
    return false;
}

bool NecDspBoard::SelectsRom(uint32_t address) const noexcept {
    const unsigned bank = (address >> 16) & 0x7f, offset = address & 0xffff;
    if ((address >> 16) == 0x7e || (address >> 16) == 0x7f)
        return false;
    switch (layout_) {
    case Layout::LoRom:
    case Layout::LoRomSmallRam:
        return bank <= 0x1f && offset >= 0x8000;
    case Layout::LoRomLargeRam:
        return bank <= 0x3f && offset >= 0x8000;
    case Layout::HiRom:
        return bank >= 0x40 || offset >= 0x8000;
    case Layout::Seta:
        return offset >= 0x8000;
    }
    return false;
}

std::optional<uint32_t> NecDspBoard::RomAddress(uint32_t address) const noexcept {
    if (!SelectsRom(address))
        return std::nullopt;
    if (layout_ == Layout::HiRom)
        return address;
    return ((address & 0xff0000) >> 1) | (address & 0x7fff);
}

std::optional<uint32_t> NecDspBoard::SramAddress(uint32_t address) const noexcept {
    const unsigned bank = address >> 16, offset = address & 0xffff;
    if (layout_ == Layout::HiRom) {
        if ((bank & 0x7f) >= 0x20 && (bank & 0x7f) <= 0x3f && offset >= 0x6000 && offset < 0x8000)
            return ((bank & 0x1f) << 13) | (offset & 0x1fff);
        return std::nullopt;
    }
    if (layout_ != Layout::LoRomSmallRam && layout_ != Layout::LoRomLargeRam)
        return std::nullopt;
    if (!((bank >= 0x70 && bank <= 0x7d) || bank >= 0xf0))
        return std::nullopt;
    if (layout_ == Layout::LoRomSmallRam)
        return ((bank & 15) << 16) | offset;
    if (offset < 0x8000)
        return ((bank & 15) << 15) | offset;
    return std::nullopt;
}

bool NecDspBoard::StatusPort(uint32_t address) const noexcept {
    if (layout_ == Layout::Seta)
        return address & 1;
    return address & (layout_ == Layout::HiRom ? 0x1000 : 0x4000);
}

void NecDspBoard::MapDevice(MemoryBus& bus, uint8_t slot) const {
    const auto map = [&](uint8_t first, uint8_t last, uint16_t low, uint16_t high) {
        bus.MapRange(first, last, low, high, slot);
        bus.MapRange(first | 0x80, last | 0x80, low, high, slot);
    };
    switch (layout_) {
    case Layout::Seta:
        map(0x60, 0x67, 0, 0x3fff);
        map(0x68, 0x6f, 0, 0x7fff);
        break;
    case Layout::HiRom:
        map(0, 0x1f, 0x6000, 0x7fff);
        break;
    case Layout::LoRom:
        map(0x30, 0x3f, 0x8000, 0xffff);
        break;
    case Layout::LoRomSmallRam:
        map(0x20, 0x3f, 0x8000, 0xffff);
        break;
    case Layout::LoRomLargeRam:
        map(0x60, 0x6f, 0, 0x7fff);
        break;
    }
}

void NecDspBoard::MapCpu(MemoryBus& bus, uint8_t slot) const {
    const auto map = [&](uint8_t first, uint8_t last, uint16_t low, uint16_t high) {
        bus.MapRange(first, last, low, high, slot);
        bus.MapRange(first | 0x80, last | 0x80, low, high, slot);
    };
    if (layout_ == Layout::Seta) {
        map(0, 0x7d, 0x8000, 0xffff);
        bus.MapRange(0xfe, 0xff, 0x8000, 0xffff, slot);
    } else if (layout_ == Layout::HiRom) {
        map(0, 0x3f, 0x8000, 0xffff);
        map(0x40, 0x7d, 0, 0xffff);
        bus.MapRange(0xfe, 0xff, 0, 0xffff, slot);
        map(0x20, 0x3f, 0x6000, 0x7fff);
    } else {
        map(0, layout_ == Layout::LoRomLargeRam ? 0x3f : 0x1f, 0x8000, 0xffff);
        if (layout_ != Layout::LoRom) {
            const uint16_t last = layout_ == Layout::LoRomSmallRam ? 0xffff : 0x7fff;
            bus.MapRange(0x70, 0x7d, 0, last, slot);
            bus.MapRange(0xf0, 0xff, 0, last, slot);
        }
    }
    MapDevice(bus, slot);
}
} // namespace snes::core
