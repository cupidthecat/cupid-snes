#include "snes/core/gameboy/Huc3Clock.hpp"

namespace snes::core::gameboy {

void Huc3Clock::Reset(uint64_t timestamp) noexcept {
    words_.fill(0);
    timestamp_ = timestamp;
    cursor_ = read_ = flags_ = 0;
    alarmEnabled_ = false;
}

void Huc3Clock::WriteNibble(uint8_t value) noexcept {
    constexpr std::array<unsigned, 4> starts = {0, 3, 0x58, 0x5b};
    constexpr std::array<unsigned, 4> widths = {3, 4, 3, 4};
    for (unsigned field = 0; field < words_.size(); ++field) {
        if (cursor_ < starts[field] || cursor_ >= starts[field] + widths[field])
            continue;
        const unsigned shift = (cursor_ - starts[field]) * 4;
        const unsigned mask = 15u << shift;
        words_[field] = uint16_t((words_[field] & ~mask) | (unsigned(value & 15) << shift));
        return;
    }
    if (cursor_ == 0x5f)
        alarmEnabled_ = (value & 1) != 0;
}

void Huc3Clock::Write(uint8_t value) noexcept {
    const unsigned command = value >> 4;
    if (command == 1) {
        if (cursor_ < 7) {
            const unsigned field = cursor_ < 3 ? 0 : 1;
            const unsigned shift = (cursor_ < 3 ? cursor_ : cursor_ - 3) * 4;
            read_ = uint8_t((words_[field] >> shift) & 15);
        }
        ++cursor_;
    } else if (command == 2 || command == 3) {
        WriteNibble(value);
        cursor_ = uint8_t(cursor_ + (command == 3));
    } else if (command == 4) {
        cursor_ = uint8_t((cursor_ & 0xf0) | (value & 15));
    } else if (command == 5) {
        cursor_ = uint8_t((cursor_ & 15) | ((value & 15) << 4));
    } else if (command == 6) {
        flags_ = value & 15;
    }
}

void Huc3Clock::Synchronize(uint64_t timestamp) noexcept {
    if (timestamp / 60 <= timestamp_ / 60)
        return;
    const uint64_t elapsed = timestamp / 60 - timestamp_ / 60;
    timestamp_ += elapsed * 60;
    // Out-of-range minute words wrap as 16-bit counters before the next day boundary.
    uint64_t untilDay = (1440u - unsigned(words_[0])) & 65535u;
    if (untilDay == 0)
        untilDay = 65536;
    if (elapsed < untilDay) {
        words_[0] = uint16_t(words_[0] + elapsed);
        return;
    }
    const auto afterFirstDay = elapsed - untilDay;
    words_[0] = uint16_t(afterFirstDay % 1440);
    words_[1] = uint16_t(words_[1] + 1 + afterFirstDay / 1440);
}

std::array<uint8_t, 17> Huc3Clock::Save() const noexcept {
    std::array<uint8_t, 17> bytes{};
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = uint8_t(timestamp_ >> (i * 8));
        bytes[8 + i] = uint8_t(words_[i / 2] >> ((i & 1) * 8));
    }
    bytes[16] = alarmEnabled_;
    return bytes;
}

void Huc3Clock::Load(std::span<const uint8_t> bytes, uint64_t now) noexcept {
    if (bytes.size() >= 17) {
        timestamp_ = 0;
        for (unsigned i = 0; i < 8; ++i)
            timestamp_ |= uint64_t(bytes[i]) << (i * 8);
        for (unsigned i = 0; i < words_.size(); ++i)
            words_[i] = uint16_t(bytes[8 + i * 2] | (unsigned(bytes[9 + i * 2]) << 8));
        alarmEnabled_ = bytes[16] != 0;
        if (timestamp_ <= now)
            return;
    }
    timestamp_ = now;
    words_[0] = 0xfff;
    words_[1] = 0xffff;
    alarmEnabled_ = false;
}

} // namespace snes::core::gameboy
