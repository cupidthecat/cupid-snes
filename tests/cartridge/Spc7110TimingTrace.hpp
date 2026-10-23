#pragma once

#include "snes/core/Spc7110.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace spc7110_test {
inline std::vector<uint8_t> TimingTrace(unsigned profile) {
    std::vector<uint8_t> rom(profile & 4 ? 0x700000 : 0x300000), ram(8192, 0xff);
    for (size_t index = 0; index < rom.size(); ++index)
        rom[index] = uint8_t(index * 37 + (index >> 4) + profile * 13);
    for (unsigned entry = 0; entry < 16; ++entry) {
        const auto table = 0x100100 + entry * 4, origin = 0x1000 + entry * 0x1000;
        rom[table] = uint8_t(entry & 3);
        for (unsigned byte = 0; byte < 3; ++byte)
            rom[table + 1 + byte] = uint8_t(origin >> ((2 - byte) * 8));
    }
    snes::core::Spc7110 chip(rom, ram, (profile & 2) != 0, [] { return int64_t{0}; });
    chip.SetPal((profile & 1) != 0);
    std::vector<uint8_t> result;
    const auto observe = [&](uint32_t address) { result.push_back(chip.Read(address, 0x73)); };
    const auto table = [&] {
        chip.Write(0x4801, 0);
        chip.Write(0x4802, 1);
        chip.Write(0x4803, 0);
        chip.Write(0x4834, 1);
    };
    table();
    for (unsigned mode = 0; mode < 4; ++mode)
        for (unsigned cut = 0; cut <= 42; ++cut) {
            chip.Write(0x4804, uint8_t(mode));
            chip.Write(0x4805, 3);
            chip.Write(0x4806, 0);
            chip.Write(0x4820, uint8_t(cut * 17));
            chip.Write(0x4821, 0x81);
            chip.Write(0x4824, 0x37);
            chip.Write(0x4825, 0x93);
            chip.Write(0x4826, 3);
            chip.Write(0x4827, 0);
            chip.Advance(cut);
            chip.Write(0x4804, uint8_t((mode + 1) & 3));
            chip.Write(0x4820, uint8_t(cut * 11));
            for (unsigned edge = 0; edge < 64; ++edge) {
                observe(0x480c);
                observe(0x482f);
                for (unsigned reg = 0x4828; reg <= 0x482d; ++reg)
                    observe(reg);
                if (edge % 7 == 0)
                    observe(0x4800);
                chip.Advance(1);
            }
        }
    uint32_t random = 0x9317ab5u + profile;
    const auto next = [&] {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return random;
    };
    constexpr std::array<uint32_t, 20> reads{
        0x4800, 0x4808, 0x4809, 0x480a, 0x480c, 0x4810,   0x481a,   0x4828,   0x4829,   0x482a,
        0x482b, 0x482c, 0x482d, 0x482f, 0x4834, 0x006123, 0x806123, 0xc01234, 0x401234, 0x4841};
    constexpr std::array<uint32_t, 25> writes{
        0x4804, 0x4805, 0x4806, 0x4807, 0x4809, 0x480a, 0x480b, 0x4811, 0x4812,
        0x4813, 0x4814, 0x4815, 0x4816, 0x4817, 0x4818, 0x4820, 0x4821, 0x4822,
        0x4823, 0x4824, 0x4825, 0x4826, 0x4827, 0x482e, 0x4830};
    for (unsigned step = 0; step < 40000; ++step) {
        const auto value = next();
        switch (value & 15) {
        case 0:
        case 1:
        case 2:
        case 3:
            chip.Advance((value >> 8) % 97);
            break;
        case 4:
            observe(0x500123);
            break;
        case 5:
            chip.Write(0x4840, uint8_t(value >> 9));
            chip.Write(0x4841, uint8_t(value >> 14));
            break;
        case 6:
            chip.Write(0x006123, uint8_t(value >> 9));
            observe(0x806123);
            break;
        case 7:
            chip.Write(0x4831 + (value >> 20) % 4, uint8_t(value >> 8));
            observe(0xe01234);
            break;
        default:
            if (value & 16) {
                const auto address = writes[(value >> 8) % writes.size()];
                const auto data = address == 0x4806                        ? 0
                                  : address == 0x4805 || address == 0x4807 ? (value >> 16) & 31
                                                                           : (value >> 16) & 255;
                chip.Write(address, uint8_t(data));
            } else
                observe(reads[(value >> 8) % reads.size()]);
            break;
        }
        if (step % 997 == 996) {
            chip.Reset();
            table();
        }
    }
    chip.Advance(1000);
    for (unsigned reg = 0x4800; reg <= 0x4842; ++reg)
        observe(reg);
    result.insert(result.end(), ram.begin(), ram.end());
    const auto rtc = chip.SaveRtc();
    result.insert(result.end(), rtc.begin(), rtc.end());
    return result;
}
} // namespace spc7110_test
