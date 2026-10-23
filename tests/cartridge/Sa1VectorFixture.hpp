#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace sa1_test {

inline std::vector<uint8_t> VectorReads(uint8_t switches, bool pal, bool bitstream) {
    const auto store = [](std::vector<uint8_t>& code, uint32_t address, uint8_t value) {
        code.insert(code.end(), {0xa9, value, 0x8f, uint8_t(address), uint8_t(address >> 8),
                                 uint8_t(address >> 16)});
    };
    const auto read = [&](std::vector<uint8_t>& code, uint32_t address, uint32_t destination,
                          bool bits = false) {
        if (bits) {
            store(code, 0x2258, 0x88);
            store(code, 0x2259, uint8_t(address));
            store(code, 0x225a, uint8_t(address >> 8));
            store(code, 0x225b, uint8_t(address >> 16));
            address = 0x230c;
        }
        code.insert(code.end(),
                    {0xaf, uint8_t(address), uint8_t(address >> 8), uint8_t(address >> 16), 0x8f,
                     uint8_t(destination), uint8_t(destination >> 8), uint8_t(destination >> 16)});
    };
    constexpr std::array<uint32_t, 3> tables{0x00ffe0, 0x80ffe0, 0xc07fe0};
    std::vector<uint8_t> worker{0x78, 0x18, 0xfb, 0xc2, 0x10};
    store(worker, 0x222a, 0xff);
    for (const auto [address, value] : {std::pair<uint32_t, uint8_t>{0x220c, 0x56},
                                        {0x220d, 0x78},
                                        {0x220e, 0x9a},
                                        {0x220f, 0xbc},
                                        {0x2209, switches}})
        store(worker, address, value);
    for (unsigned table = 0; table < tables.size(); ++table)
        for (unsigned byte = 0; byte < 32; ++byte)
            read(worker, tables[table] + byte, 0x3400 + table * 32 + byte, bitstream);
    store(worker, 0x3020, 0x5a);
    worker.push_back(0xdb);

    std::vector<uint8_t> main{0x78, 0x18, 0xfb, 0xc2, 0x10};
    for (const auto [address, value] : {std::pair<uint32_t, uint8_t>{0x120, 0},
                                        {0x2229, 0xff},
                                        {0x2203, 0},
                                        {0x2204, 0x90},
                                        {0x2200, 0}})
        store(main, address, value);
    // Both processors read the table through their own bus before the console copies the results.
    main.insert(main.end(), {0xaf, 0x20, 0x30, 0, 0xc9, 0x5a, 0xd0, 0xf8});
    for (unsigned table = 0; table < tables.size(); ++table)
        for (unsigned byte = 0; byte < 32; ++byte) {
            read(main, tables[table] + byte, 0x7e0200 + table * 32 + byte);
            read(main, 0x3400 + table * 32 + byte, 0x7e0300 + table * 32 + byte);
        }
    store(main, 0x120, 0xa5);
    main.push_back(0xdb);

    std::vector<uint8_t> rom(0x400000);
    for (unsigned bank = 0; bank < 64; ++bank)
        std::fill_n(rom.begin() + bank * 0x10000, 0x10000, uint8_t(bank * 29 + 0x31));
    std::copy(worker.begin(), worker.end(), rom.begin() + 0x1000);
    std::copy(main.begin(), main.end(), rom.begin() + 0x2000);
    std::fill_n(rom.begin() + 0x7fb0, 80, 0);
    std::copy_n("ZGNJ", 4, rom.begin() + 0x7fb2);
    std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
    constexpr std::string_view title = "SA1 VECTOR READS";
    std::copy(title.begin(), title.end(), rom.begin() + 0x7fc0);
    rom[0x7fd5] = 0x23;
    rom[0x7fd6] = 0x35;
    rom[0x7fd7] = 12;
    rom[0x7fd8] = 5;
    rom[0x7fd9] = pal ? 2 : 0;
    rom[0x7fda] = 0x33;
    rom[0x7fdc] = rom[0x7fdd] = 0xff;
    rom[0x7ffc] = 0;
    rom[0x7ffd] = 0xa0;
    return rom;
}

} // namespace sa1_test
