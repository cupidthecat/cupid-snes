#include "snes/core/gameboy/Tpp1.hpp"

#include <algorithm>

namespace snes::core::gameboy {
namespace {
uint64_t AdvanceCounter(uint8_t& value, unsigned limit, unsigned modulus, uint64_t ticks) noexcept {
    unsigned distance = (limit - value) & (modulus - 1);
    if (distance == 0)
        distance = modulus;
    if (ticks < distance) {
        value = uint8_t((value + ticks) & (modulus - 1));
        return 0;
    }
    ticks -= distance;
    value = uint8_t(ticks % limit);
    return 1 + ticks / limit;
}
} // namespace

void Tpp1::Reset(uint64_t timestamp) noexcept {
    timestamp_ = timestamp;
    romBank_ = 0;
    ramBank_ = mode_ = rumble_ = 0;
    bankWritten_ = false;
}

std::optional<uint8_t> Tpp1::Read(uint16_t address, std::span<const uint8_t> ram) noexcept {
    const unsigned port = address & 3;
    if (mode_ == 0) {
        const std::array<uint8_t, 4> registers = {uint8_t(romBank_), uint8_t(romBank_ >> 8),
                                                  ramBank_, uint8_t(flags_ | rumble_)};
        return registers[port];
    }
    if (mode_ == 5)
        return latched_[3 - port];
    if (mode_ != 2 && mode_ != 3)
        return std::nullopt;
    // ROM selector bit eleven redirects RAM accesses to legacy clock registers.
    if (romBank_ & 0x800) {
        if (ramBank_ >= latched_.size())
            return std::nullopt;
        latched_[0] &= 0x3f;
        latched_[1] &= 0x3f;
        latched_[2] &= 0x1f;
        latched_[4] &= 0xc1;
        return latched_[ramBank_];
    }
    if (ram.empty())
        return std::nullopt;
    return ram[(size_t(ramBank_) * 8192 + (address & 0x1fff)) & (ram.size() - 1)];
}

void Tpp1::Command(uint8_t value) noexcept {
    if (value == 0 || value == 2 || value == 3 || value == 5) {
        mode_ = value;
        return;
    }
    switch (value) {
    case 0x10:
        latched_ = clock_;
        break;
    case 0x11:
        clock_ = latched_;
        break;
    case 0x14:
        flags_ &= 0xf7;
        break;
    case 0x18:
        flags_ &= 0xfb;
        break;
    case 0x19:
        flags_ |= 4;
        break;
    default:
        if (value >= 0x20 && value <= 0x23)
            rumble_ = value & 3;
        break;
    }
}

void Tpp1::Write(uint16_t address, uint8_t value, std::span<uint8_t> ram) noexcept {
    if (address < 0x8000) {
        bankWritten_ = true;
        switch (address & 3) {
        case 0:
            romBank_ = uint16_t((romBank_ & 0xff00) | value);
            break;
        case 1:
            romBank_ = uint16_t((romBank_ & 0xff) | (unsigned(value) << 8));
            break;
        case 2:
            ramBank_ = value;
            break;
        case 3:
            Command(value);
            break;
        }
        return;
    }
    if (mode_ == 5) {
        latched_[3 - (address & 3)] = value;
        return;
    }
    if (mode_ != 3)
        return;
    if (romBank_ & 0x800) {
        if (ramBank_ < clock_.size()) {
            clock_[ramBank_] = value;
            if (ramBank_ == 0)
                phase_ = 0;
        }
    } else if (!ram.empty()) {
        ram[(size_t(ramBank_) * 8192 + (address & 0x1fff)) & (ram.size() - 1)] = value;
    }
}

bool Tpp1::Advance(unsigned clocks, uint32_t clocksPerSecond) noexcept {
    phase_ += uint64_t(clocks) * 2;
    const auto interval = std::max(clocksPerSecond, 32u) / 16;
    if (phase_ < interval)
        return false;
    phase_ %= interval;
    return true;
}

void Tpp1::AdvanceDays(uint64_t days) noexcept {
    uint8_t weekday = clock_[2] >> 5;
    const auto weeks = uint64_t(clock_[3]) + AdvanceCounter(weekday, 7, 8, days);
    clock_[2] = uint8_t((clock_[2] & 31) | (unsigned(weekday) << 5));
    clock_[3] = uint8_t(weeks);
    if (weeks >= 256)
        flags_ |= 8;
}

void Tpp1::Synchronize(uint64_t timestamp) noexcept {
    if (!(flags_ & 4) || timestamp <= timestamp_)
        return;
    auto seconds = timestamp - timestamp_;
    timestamp_ = timestamp;
    // Whole days precede sub-day carries, including when fields contain invalid values.
    const auto days = (seconds - 1) / 86400;
    AdvanceDays(days);
    seconds -= days * 86400;
    const auto minutes = AdvanceCounter(clock_[0], 60, 256, seconds);
    const auto hours = AdvanceCounter(clock_[1], 60, 256, minutes);
    uint8_t hour = clock_[2] & 31;
    const auto extraDays = AdvanceCounter(hour, 24, 32, hours);
    clock_[2] = uint8_t((clock_[2] & 0xe0) | hour);
    AdvanceDays(extraDays);
}

std::array<uint8_t, 20> Tpp1::Save(uint64_t now) const noexcept {
    std::array<uint8_t, 20> bytes = {'T', 'P', 'P', '1', 1, 0, flags_, 0};
    for (unsigned i = 0; i < 8; ++i)
        bytes[8 + i] = uint8_t(now >> (i * 8));
    for (unsigned i = 0; i < 4; ++i)
        bytes[16 + i] = clock_[3 - i];
    return bytes;
}

void Tpp1::Load(std::span<const uint8_t> bytes, uint64_t now) noexcept {
    if (bytes.size() >= 20) {
        timestamp_ = 0;
        for (unsigned i = 0; i < 8; ++i)
            timestamp_ |= uint64_t(bytes[8 + i]) << (i * 8);
        for (unsigned i = 0; i < 4; ++i)
            clock_[3 - i] = bytes[16 + i];
        if (timestamp_ <= now)
            return;
    }
    timestamp_ = now;
    clock_[4] |= 0x80;
}

} // namespace snes::core::gameboy
