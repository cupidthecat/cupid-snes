#include "snes/core/EpsonRtc.hpp"
#include "snes/core/Timing.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace snes::core {
EpsonRtc::EpsonRtc(Clock clock) : clock_(std::move(clock)) {
    if (!clock_)
        clock_ = [] {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
    digits_[1] = 8;
    Reset();
}

void EpsonRtc::Reset() {
    ticks_ = seconds_ = 0;
    balance_ = 0;
    selected_ = position_ = wait_ = 0;
    ready_ = heldTick_ = false;
    phase_ = Phase::Mode;
}

void EpsonRtc::SetPal(bool pal) {
    masterFrequency_ = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
}

void EpsonRtc::Advance(uint32_t masterClocks) {
    balance_ -= int64_t(masterClocks) * 2'097'152;
    if (balance_ >= 0)
        return;
    const uint64_t count = uint64_t((-balance_ + masterFrequency_ - 1) / masterFrequency_);
    balance_ += int64_t(count) * masterFrequency_;
    Ticks(count);
}

void EpsonRtc::Periodic(unsigned period) {
    if (!(digits_[15] & 3) && (digits_[14] >> 2) == period)
        irq_ = true;
}

void EpsonRtc::Round() {
    if (!(digits_[13] & 8))
        return;
    digits_[13] &= 7;
    if ((digits_[1] & 7) >= 3)
        Minute();
    digits_[0] = 0;
    digits_[1] &= 8;
}

void EpsonRtc::Ticks(uint64_t count) {
    if (wait_) {
        if (count >= wait_) {
            wait_ = 0;
            ready_ = true;
        } else
            wait_ = uint8_t(wait_ - count);
    }
    constexpr uint32_t period = 1u << 21;
    while (count) {
        const uint32_t span = uint32_t(std::min<uint64_t>(count, period - ticks_));
        if (ticks_ < 255)
            Round();
        if (ticks_ < 16383 && (digits_[14] & 2))
            irq_ = false;
        if (ticks_ < 32767)
            Periodic(0);
        ticks_ += span;
        count -= span;
        if (ticks_ == period) {
            ticks_ = 0;
            Round();
            if (digits_[14] & 2)
                irq_ = false;
            Periodic(0);
            ++seconds_;
            Periodic(1);
            if (seconds_ % 60 == 0)
                Periodic(2);
            if (seconds_ == 1440) {
                Periodic(3);
                seconds_ = 0;
            }
            if (!(digits_[15] & 3)) {
                if (digits_[13] & 1)
                    heldTick_ = true;
                else {
                    resync_ = true;
                    Second();
                }
            }
        }
    }
}

uint8_t EpsonRtc::Digit(unsigned index) {
    index &= 15;
    if (index == 13) {
        const uint8_t value = uint8_t((digits_[13] & 11) | ((irq_ && !(digits_[14] & 1)) ? 4 : 0));
        irq_ = false;
        return value;
    }
    const bool status = index == 3 || index == 5 || index == 7 || index == 9 || index == 12;
    return uint8_t(digits_[index] | ((status && resync_) ? 8 : 0));
}

void EpsonRtc::Put(unsigned index, uint8_t value) {
    index &= 15;
    value &= 15;
    constexpr uint8_t masks[]{15, 15, 15, 7, 15, 7, 15, 7, 15, 7, 15, 15, 7, 11, 15, 15};
    const uint8_t previous = digits_[index];
    digits_[index] = value & masks[index];
    if (index == 5 || index == 15) {
        if (digits_[15] & 4)
            digits_[5] &= 3;
        else
            digits_[5] &= 5;
    }
    if (index == 13 && (previous & 1) && !(value & 1) && heldTick_) {
        heldTick_ = false;
        Second();
    }
    if (index == 15 && (value & 1)) {
        digits_[0] = 0;
        digits_[1] &= 8;
    }
}

uint8_t EpsonRtc::Read(unsigned port, uint8_t openBus) {
    if (port == 0)
        return selected_;
    if (port == 2)
        return ready_ ? 0x80 : 0;
    if (port != 1)
        return openBus;
    if (selected_ != 1 || !ready_)
        return 0;
    if (phase_ == Phase::Write)
        return last_;
    if (phase_ != Phase::Read)
        return 0;
    ready_ = false;
    wait_ = 8;
    const uint8_t value = Digit(position_);
    position_ = (position_ + 1) & 15;
    return value;
}

void EpsonRtc::Write(unsigned port, uint8_t value) {
    value &= 15;
    if (port == 0) {
        selected_ = value & 3;
        if (selected_ != 1) {
            phase_ = Phase::Mode;
            position_ = 0;
            resync_ = false;
            digits_[15] &= 6;
        }
        ready_ = true;
        return;
    }
    if (port != 1 || selected_ != 1 || !ready_)
        return;
    if (phase_ == Phase::Mode) {
        if (value != 3 && value != 12)
            return;
        phase_ = Phase::Address;
    } else if (phase_ == Phase::Address) {
        phase_ = last_ == 3 ? Phase::Write : Phase::Read;
        position_ = value;
    } else if (phase_ == Phase::Write) {
        Put(position_, value);
        position_ = (position_ + 1) & 15;
    } else
        return;
    last_ = value;
    ready_ = false;
    wait_ = 8;
}

void EpsonRtc::Second() {
    auto& lo = digits_[0];
    auto& hi = digits_[1];
    if (lo <= 8 || lo == 12) {
        lo = (lo + 1) & 15;
        return;
    }
    lo = 0;
    if ((hi & 7) < 5) {
        ++hi;
        return;
    }
    hi &= 8;
    Minute();
}

void EpsonRtc::Minute() {
    auto& lo = digits_[2];
    auto& hi = digits_[3];
    if (lo <= 8 || lo == 12) {
        lo = (lo + 1) & 15;
        return;
    }
    lo = 0;
    if (hi < 5) {
        ++hi;
        return;
    }
    hi = 0;
    Hour();
}

void EpsonRtc::Hour() {
    auto& lo = digits_[4];
    uint8_t hi = digits_[5] & 3;
    bool meridian = digits_[5] & 4;
    bool nextDay = false;
    if (digits_[15] & 4) {
        nextDay = hi >= 2 && (lo == 3 || (lo & 4));
        if (nextDay) {
            lo = uint8_t((~lo) & 1);
            hi = 0;
        } else if (lo <= 8 || (hi < 2 ? lo == 12 : lo >= 12))
            lo = (lo + 1) & 15;
        else {
            lo = uint8_t((~lo) & 1);
            hi = (hi + 1) & 3;
        }
    } else if (hi == 0) {
        if (lo <= 8 || lo == 12)
            lo = (lo + 1) & 15;
        else {
            lo = uint8_t((~lo) & 1);
            hi ^= 1;
        }
    } else {
        if (lo & 1)
            meridian = !meridian;
        if ((0x1133u >> lo) & 1)
            lo = (lo + 1) & 15;
        else {
            lo = uint8_t((~lo) & 1);
            hi ^= 1;
        }
        nextDay = !meridian && !(lo & 1);
    }
    digits_[5] = uint8_t(hi | (meridian ? 4 : 0));
    if (nextDay)
        Day();
}

void EpsonRtc::Day() {
    if (!(digits_[13] & 2))
        return;
    digits_[12] = uint8_t((digits_[12] + 1 + (digits_[12] == 6)) & 7);
    constexpr uint8_t monthDays[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const unsigned month = ((digits_[9] & 1) << 4) | digits_[8];
    unsigned length = month >= 1 && month <= 12 ? monthDays[month - 1]
                      : month == 0              ? 30
                      : month & 1               ? 30
                                                : 31;
    if (length == 28 && ((digits_[10] + 10 * digits_[11]) & 3) == 0)
        ++length;
    const unsigned lo = digits_[6], hi = digits_[7] & 3;
    const bool end = length == 28   ? hi == 3 || (hi == 2 && lo >= 8)
                     : length == 29 ? hi == 3 || (hi == 2 && lo > 8 && lo != 12)
                     : length == 30 ? hi == 3 || (hi == 2 && (lo == 10 || lo == 14))
                                    : hi == 3 && (lo & 3);
    if (end) {
        digits_[6] = 1;
        digits_[7] &= 4;
        Month();
    } else if (lo <= 8 || lo == 12)
        digits_[6] = uint8_t((lo + 1) & 15);
    else {
        digits_[6] = uint8_t((~lo) & 1);
        digits_[7] = uint8_t((digits_[7] & 4) | ((hi + 1) & 3));
    }
}

void EpsonRtc::Month() {
    auto& lo = digits_[8];
    const bool hi = digits_[9] & 1;
    if (!hi || !(lo & 2)) {
        if (lo <= 8 || lo == 12)
            lo = (lo + 1) & 15;
        else {
            lo = uint8_t((~lo) & 1);
            digits_[9] ^= 1;
        }
    } else {
        lo = uint8_t((~lo) & 1);
        digits_[9] &= 6;
        Year();
    }
}

void EpsonRtc::Year() {
    auto& lo = digits_[10];
    auto& hi = digits_[11];
    if (lo <= 8 || lo == 12)
        lo = (lo + 1) & 15;
    else {
        lo = uint8_t((~lo) & 1);
        hi = hi <= 8 || hi == 12 ? uint8_t((hi + 1) & 15) : uint8_t((~hi) & 1);
    }
}

std::vector<uint8_t> EpsonRtc::Save() {
    auto raw = digits_;
    for (unsigned i : {3u, 5u, 7u, 9u, 12u})
        raw[i] |= resync_ ? 8 : 0;
    raw[13] |= irq_ ? 4 : 0;
    std::vector<uint8_t> result(raw.begin(), raw.end());
    const uint64_t stamp = uint64_t(clock_());
    for (unsigned i = 0; i < 8; ++i)
        result.push_back(uint8_t(stamp >> (i * 8)));
    return result;
}

bool EpsonRtc::Load(std::span<const uint8_t> data) {
    if (data.size() != 24 ||
        std::any_of(data.begin(), data.begin() + 16, [](uint8_t n) { return n > 15; }))
        return false;
    uint64_t stamp = 0;
    for (unsigned i = 0; i < 8; ++i)
        stamp |= uint64_t(data[16 + i]) << (i * 8);
    if (stamp > uint64_t(std::numeric_limits<int64_t>::max()))
        return false;
    std::copy_n(data.begin(), 16, digits_.begin());
    constexpr uint8_t masks[]{15, 15, 15, 7, 15, 7, 15, 7, 15, 7, 15, 15, 7, 11, 15, 15};
    for (unsigned i = 0; i < 16; ++i)
        digits_[i] &= masks[i];
    resync_ = data[3] & 8;
    irq_ = data[13] & 4;
    const int64_t elapsed = clock_() - int64_t(stamp);
    if (elapsed >= 0 && elapsed <= 315576000000ll) {
        int64_t left = elapsed;
        while (left >= 86400) {
            Day();
            left -= 86400;
        }
        while (left >= 3600) {
            Hour();
            left -= 3600;
        }
        while (left >= 60) {
            Minute();
            left -= 60;
        }
        while (left--)
            Second();
    }
    return true;
}
} // namespace snes::core
