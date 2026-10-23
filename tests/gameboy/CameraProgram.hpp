// Capture an image and copy its cartridge data into work RAM.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace snes::tests::gameboy {

inline std::vector<uint8_t> CameraProgram() {
    std::vector<uint8_t> code{0xf3, 0x31, 0xfe, 0xff, 0xaf, 0xea, 0xff, 0xcf};
    const auto store = [&](uint16_t address, uint8_t value) {
        code.insert(code.end(), {0x3e, value, 0xea, uint8_t(address), uint8_t(address >> 8)});
    };
    store(0, 0x0a);
    store(0x4000, 0x10);
    store(0xa001, 4);
    store(0xa002, 0x10);
    store(0xa003, 0);
    store(0xa004, 0);
    for (unsigned cell = 0; cell < 16; ++cell) {
        store(uint16_t(0xa006 + cell * 3), uint8_t(64 + cell * 2));
        store(uint16_t(0xa007 + cell * 3), uint8_t(128 + cell * 2));
        store(uint16_t(0xa008 + cell * 3), uint8_t(192 - cell * 2));
    }
    store(0xa000, 1);
    code.insert(code.end(), {0xfa, 0, 0xa0, 0xe6, 1, 0x20, 0xf9});
    store(0x4000, 0);
    code.insert(code.end(), {0x21, 0, 0xa1, 0x11, 0, 0xc0, 0x01, 0, 0x0e, 0x2a, 0x12, 0x13, 0x0b,
                             0x78, 0xb1, 0x20, 0xf8});
    store(0xcfff, 0x5a);
    store(0x4000, 1);
    code.insert(code.end(), {0x21, 0, 0xc0, 0x11, 0, 0xa0, 0x01, 0, 0x0e, 0x2a, 0x12, 0x13, 0x0b,
                             0x78, 0xb1, 0x20, 0xf8});
    code.insert(code.end(), {0xfa, 0xff, 0xbf, 0x3c, 0xea, 0xff, 0xbf});
    code.push_back(0x76);
    std::vector<uint8_t> image(32768);
    image[0x147] = 0xfc;
    image[0x149] = 4;
    image[0x100] = 0xc3;
    image[0x102] = 2;
    std::copy(code.begin(), code.end(), image.begin() + 0x200);
    return image;
}

} // namespace snes::tests::gameboy
