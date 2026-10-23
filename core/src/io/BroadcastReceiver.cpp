#include "snes/core/BroadcastReceiver.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "snes/core/detail/StateArchive.hpp"
#include <algorithm>
#include <array>
#include <ctime>
#include <map>
#include <utility>
#include <vector>

namespace snes::core {
BroadcastTime CurrentBroadcastTime() {
    BroadcastTime value;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &now) != 0)
        return value;
#else
    if (!localtime_r(&now, &local))
        return value;
#endif
    value.year = uint16_t(local.tm_year + 1900);
    value.month = uint8_t(local.tm_mon + 1);
    value.day = uint8_t(local.tm_mday);
    value.weekday = uint8_t(local.tm_wday + 1);
    value.hour = uint8_t(local.tm_hour);
    value.minute = uint8_t(local.tm_min);
    value.second = uint8_t(local.tm_sec);
    return value;
}

struct BroadcastReceiver::Impl {
    using StreamKey = std::pair<uint16_t, uint8_t>;

    struct Receiver {
        bool prefixEnabled = false, dataEnabled = false, loaded = false, first = false;
        uint8_t nextSequence = 0;
        StreamKey asset{};
        uint32_t cursor = 0, remaining = 0;
    };

    struct Asset {
        std::vector<uint8_t> bytes;
        std::array<uint8_t, 32> digest;
    };

    std::array<uint8_t, 24> radio{};
    bool recordedStreams = false;
    std::array<Receiver, 2> receivers{};
    std::map<StreamKey, Asset> assets;
    std::function<BroadcastTime()> timeSource;
    std::array<uint8_t, 23> clockRecord{};
    unsigned clockIndex = 0;

    void Reset() {
        radio = {};
        receivers = {};
        clockIndex = 0;
        clockRecord = {};
    }

    uint16_t Channel(unsigned receiver) const {
        const unsigned base = receiver * 6;
        return uint16_t(radio[base] | (uint16_t(radio[base + 1]) << 8));
    }

    void SelectAsset(unsigned receiver) {
        auto& state = receivers[receiver];
        auto key = StreamKey{Channel(receiver), state.nextSequence++};
        auto asset = assets.find(key);
        if (asset == assets.end() && key.second != 0) {
            key.second = 0;
            state.nextSequence = 1;
            asset = assets.find(key);
        }
        state.loaded = asset != assets.end();
        state.remaining = 0;
        state.cursor = 0;
        state.first = false;
        radio[receiver * 6 + 3] = 0;
        if (!state.loaded)
            return;
        state.asset = key;
        state.remaining = uint32_t((asset->second.bytes.size() + 21) / 22);
        state.first = true;
        radio[receiver * 6 + 5] = 0;
    }

    uint8_t ClockByte() {
        if (clockIndex == 0) {
            const BroadcastTime time = timeSource ? timeSource() : BroadcastTime{};
            clockRecord = {};
            if (recordedStreams)
                clockRecord[4] = 0x10;
            clockRecord[5] = clockRecord[6] = 1;
            clockRecord[10] = time.second;
            clockRecord[11] = time.minute;
            clockRecord[12] = time.hour;
            if (recordedStreams) {
                clockRecord[13] = time.weekday;
                clockRecord[14] = time.day;
                clockRecord[15] = time.month;
                clockRecord[16] = uint8_t(time.year);
                clockRecord[17] = uint8_t(time.year >> 8);
            }
        }
        const uint8_t value = clockRecord[clockIndex];
        clockIndex = (clockIndex + 1) % (recordedStreams ? 23u : 18u);
        return value;
    }

    uint8_t ReadRadio(unsigned reg, uint8_t openBus) {
        if (!recordedStreams) {
            switch (reg) {
            case 0:
            case 1:
            case 2:
            case 4:
            case 6:
            case 7:
            case 8:
            case 12:
            case 14:
            case 15:
            case 17:
                return radio[reg];
            case 10:
                return ClockByte();
            case 11:
                return radio[reg] & ~0x0c;
            default:
                return openBus;
            }
        }
        if (reg >= 18)
            return openBus;
        if (reg >= 12)
            return radio[reg];
        const unsigned receiver = reg / 6, field = reg % 6, base = receiver * 6;
        auto& state = receivers[receiver];
        const bool clock = Channel(receiver) == 0;
        switch (field) {
        case 0:
        case 1:
            return radio[reg];
        case 2:
            if (!state.prefixEnabled || !state.dataEnabled)
                return 0;
            if (clock)
                return 1;
            if (!state.remaining)
                SelectAsset(receiver);
            return uint8_t(std::min(state.remaining, uint32_t{127}));
        case 3:
            if (!state.prefixEnabled)
                return 0;
            if (clock)
                radio[reg] = 0x90;
            else {
                if (!state.loaded || !state.remaining)
                    return 0;
                radio[reg] = uint8_t((state.first ? 0x10 : 0) | (state.remaining == 1 ? 0x80 : 0));
                state.first = false;
                --state.remaining;
            }
            radio[base + 5] |= radio[reg];
            return radio[reg];
        case 4: {
            if (!state.dataEnabled)
                return 0;
            if (clock)
                return ClockByte();
            if (!state.loaded)
                return 0;
            const auto asset = assets.find(state.asset);
            if (asset == assets.end() || state.cursor >= asset->second.bytes.size())
                return 0xff;
            return asset->second.bytes[state.cursor++];
        }
        case 5:
            return std::exchange(radio[reg], 0);
        }
        return 0;
    }

    void WriteRadio(unsigned reg, uint8_t value) {
        if (!recordedStreams) {
            switch (reg) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 6:
            case 11:
            case 12:
            case 15:
            case 17:
                radio[reg] = value;
                break;
            case 7:
                radio[6] = uint8_t(radio[7] - (radio[6] >> 1));
                radio[7] >>= 1;
                break;
            case 9:
                radio[reg] = value;
                clockIndex = 0;
                break;
            case 10:
                radio[8] = 0x80;
                break;
            default:
                break;
            }
            return;
        }
        if (reg >= 18)
            return;
        if (reg >= 12) {
            if (reg == 12)
                radio[reg] = value & 15;
            if (reg == 15)
                radio[reg] = value;
            return;
        }
        const unsigned receiver = reg / 6, field = reg % 6;
        auto& state = receivers[receiver];
        switch (field) {
        case 0:
        case 1:
            radio[reg] = field == 1 ? value & 0x3f : value;
            state.loaded = false;
            state.cursor = state.remaining = state.nextSequence = 0;
            radio[receiver * 6 + 3] = radio[receiver * 6 + 5] = 0;
            break;
        case 3:
            state.prefixEnabled = value != 0;
            break;
        case 4:
            state.dataEnabled = value != 0;
            if (Channel(receiver) == 0)
                clockIndex = 0;
            break;
        default:
            break;
        }
    }
};

BroadcastReceiver::BroadcastReceiver() : impl_(std::make_unique<Impl>()) {
}

BroadcastReceiver::~BroadcastReceiver() = default;

void BroadcastReceiver::Reset() {
    impl_->Reset();
}

void BroadcastReceiver::SerializeState(detail::StateArchive& archive) {
    archive.Expect(impl_->recordedStreams);
    archive.Expect(uint32_t(impl_->assets.size()));
    for (const auto& [key, asset] : impl_->assets) {
        archive.Expect(key.first);
        archive.Expect(key.second);
        archive.Expect(uint32_t(asset.bytes.size()));
        for (auto byte : asset.digest)
            archive.Expect(byte);
    }
    archive.Array(impl_->radio);
    for (auto& receiver : impl_->receivers) {
        archive.Number(receiver.prefixEnabled);
        archive.Number(receiver.dataEnabled);
        const auto loaded = archive.Number(receiver.loaded);
        archive.Number(receiver.first);
        archive.Number(receiver.nextSequence);
        const auto channel = archive.Number(receiver.asset.first);
        const auto sequence = archive.Number(receiver.asset.second);
        const auto cursor = archive.Number(receiver.cursor);
        const auto remaining = archive.Number(receiver.remaining);
        if (loaded) {
            const auto asset = impl_->assets.find({channel, sequence});
            archive.Validate(asset != impl_->assets.end(), "Missing broadcast reception asset");
            const auto size = asset->second.bytes.size();
            archive.Validate(cursor <= size && remaining <= (size + 21) / 22,
                             "Invalid broadcast reception position");
        } else {
            archive.Validate(cursor == 0 && remaining == 0, "Invalid unloaded broadcast receiver");
        }
    }
    archive.Array(impl_->clockRecord);
    archive.Bounded(impl_->clockIndex, 0u, impl_->recordedStreams ? 22u : 17u);
}

bool BroadcastReceiver::Selects(uint32_t address) noexcept {
    const unsigned bank = (address >> 16) & 0x7f;
    const unsigned offset = address & 0xffff;
    return bank < 0x40 && offset >= 0x2188 && offset <= 0x219f;
}

uint8_t BroadcastReceiver::ReadCpu(uint32_t address, uint8_t openBus) {
    return Selects(address) ? impl_->ReadRadio((address & 0xffff) - 0x2188, openBus) : openBus;
}

void BroadcastReceiver::WriteCpu(uint32_t address, uint8_t value) {
    if (Selects(address))
        impl_->WriteRadio((address & 0xffff) - 0x2188, value);
}

bool BroadcastReceiver::LoadStream(uint16_t channel, uint8_t sequence,
                                   std::span<const uint8_t> payload) {
    if (!channel || channel > 0x3fff || payload.size() > size_t(22) * 65535)
        return false;
    if (!payload.empty() && !impl_->recordedStreams) {
        impl_->recordedStreams = true;
        impl_->radio = {};
        impl_->receivers = {};
        impl_->clockIndex = 0;
    }
    const auto key = Impl::StreamKey{channel, sequence};
    if (payload.empty())
        impl_->assets.erase(key);
    else
        impl_->assets[key] = {std::vector<uint8_t>(payload.begin(), payload.end()),
                              detail::Sha256(payload)};
    for (auto& receiver : impl_->receivers) {
        if (receiver.loaded && receiver.asset == key) {
            receiver.loaded = false;
            receiver.remaining = receiver.cursor = 0;
            receiver.nextSequence = sequence;
        }
    }
    return true;
}

void BroadcastReceiver::ClearStreams() {
    impl_->assets.clear();
    impl_->recordedStreams = false;
    impl_->receivers = {};
    impl_->radio = {};
    impl_->clockIndex = 0;
}

void BroadcastReceiver::SetTimeSource(std::function<BroadcastTime()> source) {
    impl_->timeSource = std::move(source);
}

} // namespace snes::core
