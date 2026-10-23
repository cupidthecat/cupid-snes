#pragma once

#include "MsuFixture.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace dsp_batch_test {

inline std::vector<uint8_t> Rom(bool pal, unsigned phase, unsigned waits) {
    std::vector<uint8_t> sound;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        sound.insert(sound.end(), bytes.begin(), bytes.end());
    };
    auto branch = [&](unsigned target) { emit({0xd0, uint8_t(target - sound.size() - 2)}); };
    emit({0xcd, 0});
    const auto clear = sound.size();
    emit({0xd8, 0xf2, 0x8f, 0, 0xf3, 0x3d, 0xc8, 0x80});
    branch(clear);
    // Copy the directory and looping BRR block from the uploaded program.
    emit({0xcd, 0});
    const auto copy = sound.size();
    emit({0xf5, 0, 0, 0xd5, 0, 4, 0x3d, 0xc8, 13});
    branch(copy);
    emit({0xcd, 0});
    const auto configure = sound.size();
    emit({0xf5, 0, 0, 0xc4, 0xf2, 0x3d, 0xf5, 0, 0, 0xc4, 0xf3, 0x3d, 0xc8, 0});
    branch(configure);
    emit({0x8f, 0x55, 0xf4, 0x8f, uint8_t(0x0a | (waits & 0xf0) | ((waits & 3) << 6)), 0xf0});
    for (unsigned i = 0; i < phase; ++i)
        emit({0x00});
    emit({0xcd, 0, 0x8f, 0, 0});
    const auto loop = sound.size();
    emit({0xab, 0, 0x8f, 7, 0xf2, 0xe4, 0, 0xc4, 0xf3, 0x8f, 0x3d, 0xf2, 0x28, 1, 0xc4, 0xf3});
    for (uint8_t reg : {8, 9, 0x7c}) {
        emit({0x8f, reg, 0xf2, 0xe4, 0xf3, 0xd5, 0, uint8_t(0x70 + reg % 3), 0x3d});
    }
    emit({0xf5, 0, 0x60, 0xd5, 0, 0x74, 0xac, 5, 4, 0x8f, 0x7c, 0xf2, 0x8f, 0xff, 0xf3});
    emit({0x2f, uint8_t(loop - sound.size() - 2)});
    const uint16_t directory = uint16_t(0x200 + sound.size());
    emit({4, 4, 4, 4, 0x13, 0x71, 0x92, 0x38, 0x56, 0xba, 0xcd, 0xef, 0x19});
    const uint16_t registers = uint16_t(0x200 + sound.size());
    const std::array<uint8_t, 40> setup{0x5d, 4,  3,    0x10, 0,    127, 1,    111,  0x0c, 127,
                                        0x1c, 97, 7,    127,  0x2c, 48,  0x3c, 32,   0x0d, 64,
                                        0x4d, 1,  0x6d, 0x60, 0x7d, 1,   0x0f, 96,   0x1f, 24,
                                        0x2f, 8,  0x3f, 0,    0x5c, 0,   0x6c, 0x1f, 0x4c, 1};
    sound.insert(sound.end(), setup.begin(), setup.end());
    sound[copy + 1] = uint8_t(directory);
    sound[copy + 2] = uint8_t(directory >> 8);
    sound[configure + 1] = sound[configure + 7] = uint8_t(registers);
    sound[configure + 2] = sound[configure + 8] = uint8_t(registers >> 8);
    sound[configure + 13] = uint8_t(setup.size());
    if (sound.size() >= 254)
        throw std::runtime_error("DSP sound program exceeds one IPL transfer");

    msu_test::Program cpu;
    cpu.Wait(0x2140, 0xaa);
    cpu.Wait(0x2141, 0xbb);
    cpu.Store(0x2141, 1);
    cpu.Store(0x2142, 0);
    cpu.Store(0x2143, 2);
    cpu.Store(0x2140, 0xcc);
    cpu.Wait(0x2140, 0xcc);
    cpu.Emit({0xa2, 0});
    const auto transfer = cpu.code.size();
    cpu.Emit({0xbd, 0, 0x84, 0x8d, 0x41, 0x21, 0x8a, 0x8d, 0x40, 0x21, 0xcd, 0x40, 0x21, 0xd0, 0xfb,
              0xe8, 0xe0, uint8_t(sound.size())});
    cpu.Emit({0xd0, uint8_t(transfer - cpu.code.size() - 2)});
    cpu.Store(0x2141, 0);
    cpu.Store(0x2142, 0);
    cpu.Store(0x2143, 2);
    cpu.Store(0x2140, uint8_t(sound.size() + 1));
    cpu.Wait(0x2140, 0x55);
    cpu.Store(0x01ff, 0xa5);
    cpu.Emit({0x80, 0xfe});
    auto rom = msu_test::Rom(0, pal);
    std::fill_n(rom.begin(), 0x800, uint8_t{0xea});
    std::copy(cpu.code.begin(), cpu.code.end(), rom.begin());
    std::copy(sound.begin(), sound.end(), rom.begin() + 0x400);
    return rom;
}

} // namespace dsp_batch_test
