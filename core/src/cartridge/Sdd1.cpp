#include "snes/core/Sdd1.hpp"
#include "snes/core/MemoryBus.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <array>
#include <bit>
#include <optional>

namespace snes::core {
namespace {

// S-DD1 probability states: Golomb order, next MPS state, next LPS state.
// Hardware algorithm: https://wiki.superfamicom.org/s-dd1
struct State {
    unsigned order, predicted, unexpected;
};

constexpr std::array<State, 33> states{
    {{0, 25, 25}, {0, 2, 1},   {0, 3, 1},   {0, 4, 2},   {0, 5, 3},   {1, 6, 4},   {1, 7, 5},
     {1, 8, 6},   {1, 9, 7},   {2, 10, 8},  {2, 11, 9},  {2, 12, 10}, {2, 13, 11}, {3, 14, 12},
     {3, 15, 13}, {3, 16, 14}, {3, 17, 15}, {4, 18, 16}, {4, 19, 17}, {5, 20, 18}, {5, 21, 19},
     {6, 22, 20}, {6, 23, 21}, {7, 24, 22}, {7, 24, 23}, {0, 26, 1},  {1, 27, 2},  {2, 28, 4},
     {3, 29, 8},  {4, 30, 12}, {5, 31, 16}, {6, 32, 18}, {7, 24, 22}}};

size_t Mirror(size_t offset, size_t size) {
    size_t base = 0;
    while (offset >= size) {
        const size_t block = std::bit_floor(offset);
        offset -= block;
        if (size > block) {
            base += block;
            size -= block;
        }
    }
    return base + offset;
}

std::optional<size_t> SaveAddress(uint32_t address) {
    const unsigned bank = address >> 16, offset = address & 0xffff;
    if (!(bank & 0x40) && offset >= 0x6000 && offset < 0x8000)
        return (bank << 13) | (offset & 0x1fff);
    if (bank >= 0x70 && bank <= 0x73)
        return (bank << 15) | (offset & 0x7fff);
    return std::nullopt;
}
} // namespace

class Sdd1Decoder {
  public:
    void SerializeState(detail::StateArchive& archive) {
        archive.Object(*this, [](auto& fields, auto& decoder) {
            fields.Number(decoder.inputOffset_);
            fields.Number(decoder.header_);
            fields.Bounded(decoder.bitsLeft_, 0u, 8u);
            fields.Bounded(decoder.pairs_, 0u, 31u);
            fields.Number(decoder.odd_);
            fields.Number(decoder.oddPending_);
            fields.Array(decoder.predictions_, [](auto& predictionFields, auto& prediction) {
                predictionFields.Bounded(prediction.state, 0u, 32u);
                predictionFields.Bounded(prediction.bit, 0u, 1u);
            });
            fields.Array(decoder.runs_, [](auto& runFields, auto& run) {
                runFields.Bounded(run.zeros, 0u, 128u);
                runFields.Number(run.one);
            });
            fields.Array(decoder.history_);
        });
    }

    void Initialize(const std::function<uint8_t(uint32_t)>& reader, uint32_t address) {
        *this = {};
        inputOffset_ = address;
        header_ = reader(address);
    }

    uint8_t Read(const std::function<uint8_t(uint32_t)>& reader) {
        const unsigned layout = header_ >> 6;
        if (layout == 3) {
            uint8_t pixel = 0;
            for (unsigned plane = 0; plane < 8; ++plane)
                pixel |= PlaneBit(plane, reader) << plane;
            return pixel;
        }
        if (oddPending_) {
            oddPending_ = false;
            return odd_;
        }
        // Decode a plane pair together, retaining the second byte until its bus read.
        const unsigned plane = layout == 0 ? 0 : ((pairs_ / 8) * 2) & (layout == 1 ? 7 : 3);
        uint8_t even = 0;
        odd_ = 0;
        for (unsigned x = 0; x < 8; ++x) {
            even = uint8_t((even << 1) | PlaneBit(plane, reader));
            odd_ = uint8_t((odd_ << 1) | PlaneBit(plane + 1, reader));
        }
        pairs_ = (pairs_ + 1) & 31;
        oddPending_ = true;
        return even;
    }

  private:
    unsigned InputBit(const std::function<uint8_t(uint32_t)>& reader) {
        if (bitsLeft_ == 0) {
            ++inputOffset_;
            bitsLeft_ = 8;
        }
        return (reader(inputOffset_) >> --bitsLeft_) & 1;
    }

    unsigned PlaneBit(unsigned plane, const std::function<uint8_t(uint32_t)>& reader) {
        constexpr unsigned masks[] = {0x1c0, 0x180, 0xc0, 0x180};
        const unsigned model = (header_ >> 4) & 3;
        const unsigned context = ((plane & 1) << 4) | ((history_[plane] & masks[model]) >> 5) |
                                 (history_[plane] & (model == 3 ? 3 : 1));
        auto& prediction = predictions_[context];
        const auto state = states[prediction.state];
        auto& run = runs_[state.order];
        if (run.zeros == 0 && !run.one) {
            run.one = InputBit(reader) != 0;
            run.zeros = 1u << state.order;
            if (run.one) {
                unsigned remainder = 0;
                for (unsigned bit = 0; bit < state.order; ++bit)
                    remainder |= InputBit(reader) << bit;
                run.zeros -= remainder + 1;
            }
        }
        const bool unexpected = run.zeros == 0;
        if (unexpected)
            run.one = false;
        else
            --run.zeros;

        const unsigned value = prediction.bit ^ unsigned(unexpected);
        if (run.zeros == 0 && !run.one) {
            if (unexpected && prediction.state < 2)
                prediction.bit ^= 1;
            prediction.state = unexpected ? state.unexpected : state.predicted;
        }
        history_[plane] = uint16_t((history_[plane] << 1) | value);
        return value;
    }

    uint32_t inputOffset_ = 0;
    uint8_t header_ = 0;
    unsigned bitsLeft_ = 4;
    unsigned pairs_ = 0;
    uint8_t odd_ = 0;
    bool oddPending_ = false;

    struct Prediction {
        unsigned state = 0, bit = 0;
    };

    struct Run {
        unsigned zeros = 0;
        bool one = false;
    };

    std::array<Prediction, 32> predictions_{};
    std::array<Run, 8> runs_{};
    std::array<uint16_t, 8> history_{};
};

Sdd1::Sdd1(std::span<const uint8_t> rom, std::span<uint8_t> ram)
    : rom_(rom), ram_(ram), decoder_(std::make_unique<Sdd1Decoder>()) {
}

Sdd1::~Sdd1() = default;

void Sdd1::SerializeState(detail::StateArchive& archive) {
    archive.Expect(uint64_t(rom_.size()));
    archive.Expect(uint64_t(ram_.size()));
    archive.Number(hardEnable_);
    archive.Number(softEnable_);
    archive.Array(banks_);
    archive.Array(channels_, [](auto& fields, auto& channel) {
        fields.Bounded(channel.address, 0u, 0xffffffu);
        fields.Number(channel.remaining);
    });
    archive.Number(streaming_);
    decoder_->SerializeState(archive);
}

void Sdd1::Reset() {
    hardEnable_ = softEnable_ = 0;
    banks_ = {0, 1, 2, 3};
    channels_ = {};
    streaming_ = false;
}

uint8_t Sdd1::ReadRom(uint32_t offset) const {
    return rom_.empty() ? 0xff : rom_[Mirror(offset, rom_.size())];
}

uint8_t Sdd1::ReadMmc(uint32_t address) const {
    const unsigned window = (address >> 20) & 3;
    return ReadRom((uint32_t(banks_[window] & 15) << 20) | (address & 0xfffff));
}

uint8_t Sdd1::Read(uint32_t address, uint8_t openBus) {
    address &= 0xffffff;
    if ((address & 0x40fff0) == 0x4800) {
        const unsigned port = address & 15;
        if (port == 0)
            return hardEnable_;
        if (port == 1)
            return softEnable_;
        if (port >= 4 && port <= 7)
            return banks_[port - 4];
        return ReadRom(0x4800 | port);
    }
    if (const auto offset = SaveAddress(address); offset && !ram_.empty())
        return ram_[Mirror(*offset, ram_.size())];
    if ((address & 0x408000) == 0x008000) {
        const unsigned mirror = (address & 0x800000) ? 3 : 1;
        if (banks_[mirror] & 0x80)
            address &= ~0x200000u;
        return ReadRom(((address >> 1) & 0x1f8000) | (address & 0x7fff));
    }
    if ((address & 0xc00000) != 0xc00000)
        return openBus;
    const uint8_t enabled = hardEnable_ & softEnable_;
    for (unsigned channel = 0; channel < channels_.size(); ++channel) {
        auto& transfer = channels_[channel];
        if (!(enabled & (1u << channel)) || transfer.address != address)
            continue;
        const auto reader = [this](uint32_t input) { return ReadMmc(input); };
        if (!streaming_) {
            decoder_->Initialize(reader, address);
            streaming_ = true;
        }
        const uint8_t value = decoder_->Read(reader);
        if (--transfer.remaining == 0) {
            softEnable_ &= uint8_t(~(1u << channel));
            streaming_ = false;
        }
        return value;
    }
    return ReadMmc(address);
}

std::optional<uint8_t> Sdd1::Peek(uint32_t address, uint8_t openBus) const {
    address &= 0xffffff;
    if ((address & 0x40fff0) == 0x4800) {
        const auto port = address & 15;
        if (port == 0)
            return hardEnable_;
        if (port == 1)
            return softEnable_;
        if (port >= 4 && port <= 7)
            return banks_[port - 4];
        return ReadRom(0x4800 | port);
    }
    if (const auto offset = SaveAddress(address); offset && !ram_.empty())
        return ram_[Mirror(*offset, ram_.size())];
    if ((address & 0x408000) == 0x008000) {
        const auto mirror = address & 0x800000 ? 3 : 1;
        if (banks_[mirror] & 0x80)
            address &= ~0x200000u;
        return ReadRom(((address >> 1) & 0x1f8000) | (address & 0x7fff));
    }
    if ((address & 0xc00000) != 0xc00000)
        return openBus;
    for (unsigned channel = 0; channel < channels_.size(); ++channel)
        if ((hardEnable_ & softEnable_ & (1u << channel)) && channels_[channel].address == address)
            return std::nullopt;
    return ReadMmc(address);
}

void Sdd1::Write(uint32_t address, uint8_t value) {
    address &= 0xffffff;
    if ((address & 0x40fff0) == 0x4800) {
        const unsigned port = address & 15;
        if (port == 0)
            hardEnable_ = value;
        else if (port == 1)
            softEnable_ = value;
        else if (port >= 4 && port <= 7)
            banks_[port - 4] = value & 0x8f;
        return;
    }
    if (const auto offset = SaveAddress(address); offset && !ram_.empty())
        ram_[Mirror(*offset, ram_.size())] = value;
}

void Sdd1::ObserveDmaWrite(uint32_t address, uint8_t value) {
    if ((address & 0x40ff80) != 0x4300)
        return;
    auto& channel = channels_[(address >> 4) & 7];
    const unsigned port = address & 15;
    if (port >= 2 && port <= 4) {
        const unsigned shift = (port - 2) * 8;
        channel.address = (channel.address & ~(0xffu << shift)) | (uint32_t(value) << shift);
    } else if (port == 5)
        channel.remaining = uint16_t((channel.remaining & 0xff00) | value);
    else if (port == 6)
        channel.remaining = uint16_t((channel.remaining & 0x00ff) | (unsigned(value) << 8));
}

void Sdd1::MapCpu(MemoryBus& bus, uint8_t slot, bool hasRam) {
    for (unsigned mirror : {0u, 0x80u}) {
        bus.MapRange(uint8_t(mirror), uint8_t(mirror | 0x3f), 0x4800, 0x480f, slot);
        bus.MapRange(uint8_t(mirror), uint8_t(mirror | 0x3f), 0x8000, 0xffff, slot);
        if (hasRam)
            bus.MapRange(uint8_t(mirror), uint8_t(mirror | 0x3f), 0x6000, 0x7fff, slot);
    }
    bus.MapRange(0xc0, 0xff, 0, 0xffff, slot);
    if (hasRam)
        bus.MapRange(0x70, 0x73, 0, 0xffff, slot);
}

std::vector<uint8_t> DecompressSdd1(const std::function<uint8_t(uint32_t)>& reader,
                                    size_t outputSize) {
    Sdd1Decoder decoder;
    decoder.Initialize(reader, 0);
    std::vector<uint8_t> result(outputSize);
    for (auto& byte : result)
        byte = decoder.Read(reader);
    return result;
}

} // namespace snes::core
