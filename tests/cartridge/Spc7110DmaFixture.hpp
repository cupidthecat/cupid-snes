#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace spc7110_test {

enum class Transfer { Forward, Reverse, Decrement, Hdma };

struct DmaProgram {
    std::vector<uint8_t> image;
    unsigned controlOffset;
};

class DmaGuest {
  public:
    void Write(uint16_t address, uint8_t value) {
        code_.insert(code_.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
    }

    void Write(std::initializer_list<std::pair<uint16_t, uint8_t>> values) {
        for (const auto [address, value] : values)
            Write(address, value);
    }

    void Read(uint16_t address) {
        code_.insert(code_.end(), {0xad, uint8_t(address), uint8_t(address >> 8), 0x8f,
                                   uint8_t(output_), uint8_t(output_ >> 8), 0x7f});
        ++output_;
    }

    void Blank(bool high, bool vertical = false) {
        code_.insert(code_.end(), {0xad, 0x12, 0x42, 0x29, uint8_t(vertical ? 0x80 : 0x40),
                                   uint8_t(high ? 0xf0 : 0xd0), 0xf9});
    }

    void Delay(unsigned count) {
        code_.insert(code_.end(), count, 0xea);
    }

    void Beam() {
        for (uint16_t address : {0x2137, 0x213c, 0x213c, 0x213d, 0x213d})
            Read(address);
    }

    void Sample() {
        Read(0x480c);
        Read(0x482f);
        for (uint16_t address = 0x4820; address < 0x482e; ++address)
            Read(address);
        Read(0x4809);
        Read(0x480a);
        for (uint16_t address = 0x4810; address < 0x481b; ++address)
            Read(address);
    }

    DmaProgram Finish(bool pal,
                      std::initializer_list<uint16_t> controls = {0x4318, 0x4319, 0x431a, 0x4302,
                                                                  0x4303, 0x4304, 0x4305, 0x4306}) {
        const unsigned control = output_;
        for (uint16_t address : controls)
            Read(address);
        Write(0x120, 0xa5);
        code_.push_back(0xdb);
        std::vector<uint8_t> image(0x300000);
        for (size_t index = 0x100000; index < image.size(); ++index)
            image[index] = uint8_t((index * 37) ^ (index >> 4) ^ 0xa5);
        for (unsigned mode = 0; mode < 3; ++mode) {
            const unsigned offset = 0x100100 + mode * 4;
            image[offset] = uint8_t(mode);
            image[offset + 1] = 0;
            image[offset + 2] = uint8_t(0x12 + mode * 0x10);
            image[offset + 3] = 0x34;
        }
        std::copy(code_.begin(), code_.end(), image.begin() + 0x8000);
        constexpr std::array<uint16_t, 8> ports{0x4800, 0x480c, 0x482f, 0x4828,
                                                0x4829, 0x482a, 0x482b, 0x4810};
        for (unsigned line = 0; line < 32; ++line) {
            const unsigned offset = 0xe000 + line * 3;
            image[offset] = 1;
            image[offset + 1] = uint8_t(ports[line % ports.size()]);
            image[offset + 2] = uint8_t(ports[line % ports.size()] >> 8);
        }
        std::fill_n(image.begin() + 0xffc0, 21, ' ');
        std::copy_n("CARTRIDGE DMA TIME", 18, image.begin() + 0xffc0);
        image[0xffd5] = 0x3a;
        image[0xffd6] = 0xf5;
        image[0xffd7] = 12;
        image[0xffd8] = 3;
        image[0xffd9] = pal ? 2 : 0;
        image[0xffda] = 0x33;
        image[0xffdc] = image[0xffdd] = 255;
        image[0xfffd] = 0x80;
        return {std::move(image), control};
    }

  private:
    std::vector<uint8_t> code_{0x78};
    unsigned output_ = 0;
};

inline DmaProgram DmaTimeProgram(Transfer transfer, unsigned mode, unsigned alignment, bool pal) {
    DmaGuest guest;
    guest.Write(0x2100, 0x80);
    guest.Write({{0x4801, 0},    {0x4802, 1},    {0x4803, 0},    {0x4804, uint8_t(mode)},
                 {0x4834, 1},    {0x4811, 0x34}, {0x4812, 0x12}, {0x4813, 0},
                 {0x4816, 1},    {0x4817, 0},    {0x4818, 0},    {0x482e, 1},
                 {0x4820, 0x12}, {0x4821, 0x34}, {0x4822, 0x56}, {0x4823, 0x78},
                 {0x4824, 0x9a}, {0x4826, 7},    {0x4809, 0},    {0x480a, 2},
                 {0x4806, 0}});
    const bool reverse = transfer == Transfer::Reverse || transfer == Transfer::Decrement;
    guest.Write({{0x4310, 0x40},
                 {0x4311, uint8_t(reverse ? 0x22 : 0x80)},
                 {0x4312, 0},
                 {0x4313, 0xe0},
                 {0x4314, 0},
                 {0x4317, 0},
                 {0x2181, 0},
                 {0x2182, 0x20},
                 {0x2183, 0},
                 {0x420c, 2}});
    guest.Blank(true, true);
    guest.Blank(false, true);
    if (transfer == Transfer::Hdma) {
        for (unsigned line = 0; line < 32; ++line) {
            guest.Blank(true);
            guest.Blank(false);
            guest.Delay(60 + alignment + line % 9);
            if (line % 3 == 0)
                guest.Write(0x4806, 0);
            else if (line % 3 == 1)
                guest.Write(0x4825, 0xbc);
            else
                guest.Write(0x4827, 0);
            guest.Sample();
        }
    } else {
        std::array<uint8_t, 47> parameters{};
        for (auto [address, value] : std::initializer_list<std::pair<uint16_t, uint8_t>>{
                 {0x4801, 0},    {0x4802, 1},    {0x4803, 0},    {0x4804, uint8_t(mode)},
                 {0x4805, 0},    {0x4806, 0},    {0x4807, 2},    {0x480b, 1},
                 {0x4811, 0x34}, {0x4812, 0x12}, {0x4813, 0},    {0x4814, 0xfe},
                 {0x4815, 0xff}, {0x4816, 3},    {0x4817, 0},    {0x4818, 0x0f},
                 {0x4820, 0x12}, {0x4821, 0x34}, {0x4822, 0x56}, {0x4823, 0x78},
                 {0x4824, 0x9a}, {0x4825, 0xbc}, {0x4826, 7},    {0x4827, 0},
                 {0x482e, 1}})
            parameters[address - 0x4801] = value;
        for (unsigned index = 0; index < parameters.size(); ++index)
            guest.Write(uint16_t(0x1200 + index),
                        parameters[transfer == Transfer::Decrement ? parameters.size() - 1 - index
                                                                   : index]);
        const uint8_t control = transfer == Transfer::Forward     ? 8
                                : transfer == Transfer::Decrement ? 0x90
                                                                  : 0x80;
        const uint16_t port = transfer == Transfer::Forward     ? 0x4800
                              : transfer == Transfer::Decrement ? 0x482f
                                                                : 0x4801;
        const unsigned count = transfer == Transfer::Forward ? 256 : parameters.size();
        guest.Write({{0x4300, control},
                     {0x4301, 0x80},
                     {0x4302, uint8_t(port)},
                     {0x4303, uint8_t(port >> 8)},
                     {0x4304, 0},
                     {0x4305, uint8_t(count)},
                     {0x4306, uint8_t(count >> 8)},
                     {0x2181, 0},
                     {0x2182, uint8_t(transfer == Transfer::Forward ? 0x20 : 0x12)},
                     {0x2183, 0}});
        guest.Blank(true);
        guest.Blank(false);
        guest.Delay(60 + alignment);
        guest.Write(0x4825, 0xbc);
        guest.Write(0x4827, 0);
        guest.Write(0x4806, 0);
        guest.Beam();
        guest.Write(0x420b, 1);
        guest.Beam();
        guest.Sample();
    }
    guest.Blank(true, true);
    guest.Write(0x420c, 0);
    guest.Sample();
    return guest.Finish(pal);
}

inline DmaProgram ArithmeticDmaProgram(bool divide, unsigned mode, unsigned alignment, bool pal) {
    DmaGuest guest;
    guest.Write({{0x2100, 0x80},
                 {0x4820, 0x12},
                 {0x4821, 0x34},
                 {0x4822, 0x56},
                 {0x4823, uint8_t(mode == 2 ? 0x87 : 0x78)},
                 {0x4824, 0x9a},
                 {0x4826, 7},
                 {0x482e, uint8_t(mode != 0)},
                 {0x1200, uint8_t(divide ? 0 : 0xbc)}});
    const uint16_t trigger = divide ? 0x4827 : 0x4825;
    guest.Write({{0x4300, 0x88},
                 {0x4301, 0x80},
                 {0x4302, uint8_t(trigger)},
                 {0x4303, 0x48},
                 {0x4304, 0},
                 {0x4305, 1},
                 {0x4306, 0},
                 {0x4310, 8},
                 {0x4311, 0x80},
                 {0x4312, 0x2f},
                 {0x4313, 0x48},
                 {0x4314, 0},
                 {0x4315, 16},
                 {0x4316, 0},
                 {0x2181, 0},
                 {0x2182, 0x12},
                 {0x2183, 0}});
    guest.Blank(true);
    guest.Blank(false);
    guest.Delay(60 + alignment);
    guest.Beam();
    guest.Write(0x420b, 3);
    guest.Beam();
    guest.Sample();
    return guest.Finish(pal, {0x4302, 0x4303, 0x4305, 0x4306, 0x4312, 0x4313, 0x4315, 0x4316});
}

} // namespace spc7110_test
