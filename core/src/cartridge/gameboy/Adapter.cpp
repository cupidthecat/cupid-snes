#include "snes/core/gameboy/Adapter.hpp"

#include "snes/core/MemoryBus.hpp"
#include "snes/core/Timing.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace snes::core::gameboy {

Adapter::Adapter(bool secondModel) : secondModel_(secondModel) {
    Reset();
}

bool Adapter::Load(std::span<const uint8_t> image, std::span<const uint8_t> boot,
                   std::string* error) {
    if (!machine_.Load(image, boot, error))
        return false;
    loaded_ = true;
    Reset();
    return true;
}

void Adapter::ResetHandheld() {
    clockCredit_ = 0;
    machine_.Reset(secondModel_, true);
}

void Adapter::Reset() {
    host_.Reset();
    ResetHandheld();
    mixer_.Reset(ClockRate() / 128);
}

void Adapter::InitializeStartup(std::shared_ptr<StartupState> state) {
    machine_.SetStartupState(std::move(state));
    // Initialize the handheld circuitry before applying console power.
    machine_.Reset(secondModel_);
    Reset();
}

void Adapter::SetPal(bool pal) noexcept {
    masterHz_ = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
    mixer_.SetHandheldRate(ClockRate() / 128);
}

uint32_t Adapter::ClockRate() const noexcept {
    return (secondModel_ ? 20'971'520 : masterHz_) / host_.Divider();
}

void Adapter::Advance(uint32_t masterClocks, bool synchronize) {
    // Charge time under the current divider before a control write can reset it.
    const auto elapsed = int64_t(masterClocks) * ClockRate();
    if (clockCredit_ > std::numeric_limits<int64_t>::max() - elapsed)
        throw std::overflow_error("Handheld oscillator credit is outside the supported range");
    clockCredit_ += elapsed;
    if (!synchronize)
        return;
    while (clockCredit_ > 0) {
        unsigned clocks = 128;
        if (loaded_ && (host_.Control() & 0x80))
            clocks = machine_.RunInstruction();
        else
            Sample(0, 0, 0);
        clockCredit_ -= int64_t(clocks) * masterHz_;
    }
}

bool Adapter::Selects(uint32_t address) noexcept {
    if (address & 0x400000)
        return false;
    const auto offset = uint16_t(address);
    return (offset >= 0x6000 && offset <= 0x67ff) || (offset >= 0x7000 && offset <= 0x7fff);
}

uint8_t Adapter::Read(uint32_t address) {
    return host_.Read(address);
}

void Adapter::Write(uint32_t address, uint8_t value) {
    host_.Write(address, value);
    if (uint16_t(address) == 0x6003)
        mixer_.SetHandheldRate(ClockRate() / 128);
}

void Adapter::MapCpu(MemoryBus& bus, uint8_t slot) {
    bus.MapRange(0x00, 0x7d, 0x8000, 0xffff, slot);
    bus.MapRange(0x80, 0xff, 0x8000, 0xffff, slot);
    bus.MapRange(0x40, 0x7d, 0, 0x7fff, slot);
    bus.MapRange(0xc0, 0xff, 0, 0x7fff, slot);
    for (uint8_t bank : {0x00, 0x80}) {
        bus.MapRange(bank, uint8_t(bank + 0x3f), 0x6000, 0x67ff, slot);
        bus.MapRange(bank, uint8_t(bank + 0x3f), 0x7000, 0x7fff, slot);
    }
}

std::vector<uint8_t> Adapter::SaveBattery() const {
    return machine_.GameCartridge().SaveBattery();
}

void Adapter::LoadBattery(std::span<const uint8_t> data) {
    machine_.GameCartridge().LoadBattery(data);
}

void Adapter::Sample(uint64_t, int16_t left, int16_t right) {
    mixer_.WriteHandheld(left, right);
    if (audioOutput_)
        audioOutput_(left, right, ClockRate() / 128);
}

} // namespace snes::core::gameboy
