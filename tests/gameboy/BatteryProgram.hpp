#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace snes::tests::gameboy {

inline std::vector<uint8_t> BatteryBios(bool secondModel, bool pal) {
    std::vector<uint8_t> image(0x80000);
    std::fill_n(image.begin() + 0x7fc0, 21, ' ');
    const std::string_view title = secondModel ? "Super GAMEBOY2" : "Super GAMEBOY";
    std::copy(title.begin(), title.end(), image.begin() + 0x7fc0);
    image[0x7fd5] = secondModel ? 0x30 : 0x20;
    image[0x7fd6] = secondModel ? 0 : 0xe3;
    if (secondModel) {
        image[0x7fda] = 0x33;
        std::copy_n("042J", 4, image.begin() + 0x7fb2);
    }
    image[0x7fd7] = 9;
    image[0x7fd9] = pal ? 2 : 0;
    image[0x7fdc] = image[0x7fdd] = 0xff;
    image[0x7ffd] = 0x80;
    const std::array<uint8_t, 12> code{0x78, 0xa9, 0x80, 0x8d, 0,    0x21,
                                       0xa9, 0x81, 0x8d, 3,    0x60, 0xdb};
    std::copy(code.begin(), code.end(), image.begin());
    return image;
}

inline std::array<uint8_t, 256> BatteryBoot() {
    std::array<uint8_t, 256> image{};
    const std::array<uint8_t, 5> code{0x3e, 1, 0xc3, 0xfe, 0};
    std::copy(code.begin(), code.end(), image.begin());
    image[254] = 0xe0;
    image[255] = 0x50;
    return image;
}

inline std::vector<uint8_t> BatteryProgram() {
    std::vector<uint8_t> image(32768);
    image[0x147] = 3;
    image[0x149] = 3;
    image[0x100] = 0xc3;
    image[0x102] = 2;
    std::vector<uint8_t> code{0xf3, 0x31, 0xfe, 0xff, 0x3e, 0x0a, 0xea,
                              0,    0,    0x3e, 1,    0xea, 0,    0x60};
    for (uint8_t bank = 0; bank < 4; ++bank) {
        code.insert(code.end(), {0x3e, bank, 0xea, 0, 0x40, 0x3e, uint8_t(0x53 + bank * 37), 0xea,
                                 0x23, 0xa1, 0x3e, uint8_t(0xa0 + bank), 0xea, 0xfe, 0xbf});
    }
    code.insert(code.end(), {0x76, 0x18, 0xfd});
    std::copy(code.begin(), code.end(), image.begin() + 0x200);
    return image;
}

} // namespace snes::tests::gameboy
