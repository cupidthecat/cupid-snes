#pragma once

#include "snes/core/St018Processor.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace snes::tests {
inline std::vector<uint8_t> ArmFirmware(unsigned profile) {
    std::vector<uint8_t> firmware(0x28000);
    const std::array<std::vector<uint32_t>, 4> programs{
        std::vector<uint32_t>{0xe3a0020e, 0xe3a01000, 0xe2811001, 0xe5801000, 0xeafffffc},
        std::vector<uint32_t>{0xe3a00101, 0xe5901020, 0xe3110008, 0x0afffffc, 0xe5901010,
                              0xe2811001, 0xe5801000, 0xeafffff8},
        std::vector<uint32_t>{0xe3a0020e, 0xe3a01013, 0xe3a02027, 0xe0030291, 0xe880000e,
                              0xe8900070, 0xe0847005, 0xe5807010, 0xeafffff9},
        std::vector<uint32_t>{0xe3a00101, 0xe3a010f7, 0xe5801020, 0xe5801024, 0xe5801028,
                              0xe580102c, 0xe5801010, 0xe5902020, 0xe5802000, 0xeafffff8}};
    const auto& program = programs[profile % programs.size()];
    for (unsigned index = 0; index < program.size(); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
            firmware[index * 4 + byte] = uint8_t(program[index] >> (byte * 8));
    return firmware;
}

inline std::vector<uint8_t> ArmTimingTrace(unsigned profile) {
    snes::core::St018Processor chip(ArmFirmware(profile));
    chip.SetPal(profile & 4);
    std::vector<uint8_t> output;
    uint32_t random = 0x7195a3u + profile;
    const auto next = [&] {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return random;
    };
    const auto observe = [&] {
        output.push_back(chip.Read(0x3804));
        output.push_back(chip.Read(0x3800));
        output.push_back(chip.Read(0x3802));
        const auto ram = chip.RamData();
        output.insert(output.end(), ram.begin(), ram.begin() + 32);
    };
    for (uint32_t clocks : {0u, 1u, 65534u, 1u, 1u, 113u, 327u}) {
        chip.Advance(clocks, true);
        observe();
    }
    for (unsigned index = 0; index < 40000; ++index) {
        const auto choice = next();
        chip.Advance(choice & 31, choice & 128);
        if (choice & 32)
            chip.Write(0x3802, uint8_t(choice >> 8));
        if (index % 10000 == 1000) {
            chip.Write(0x3804, 1);
            chip.Advance((choice >> 16) & 1023, true);
            observe();
            chip.Write(0x3804, 0);
            chip.Advance(70000, true);
        }
        if (index % 7 == 0)
            observe();
    }
    observe();
    return output;
}
} // namespace snes::tests
