// The console waits in work RAM while the cartridge processor fills its cache.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace snes::tests::cx4 {

enum class Probe { None, Status, Rom, DataRead, DataWrite, RegisterWrite, RegisterRead };

inline std::vector<uint8_t> IrqProgram(uint8_t count, unsigned delay, bool pal,
                                       Probe probe = Probe::None) {
    const auto store = [](std::vector<uint8_t>& code, uint16_t address, uint8_t value) {
        code.insert(code.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
    };
    std::vector<uint8_t> worker;
    store(worker, 0x7f49, 0);
    store(worker, 0x7f4a, 0x90);
    store(worker, 0x7f4b, 0);
    store(worker, 0x7f4d, 0);
    store(worker, 0x7f4e, 0);
    worker.insert(worker.end(), delay, 0xea);
    store(worker, 0x7f4f, 0);
    worker.push_back(0x58);
    std::vector<uint8_t> loop;
    if (probe != Probe::None)
        loop = {0xee, 4, 0x11};
    switch (probe) {
    case Probe::None:
        break;
    case Probe::Status:
        loop.insert(loop.end(), {0xad, 0x5e, 0x7f});
        break;
    case Probe::Rom:
        loop.insert(loop.end(), {0xaf, 0, 0x80, 0});
        break;
    case Probe::DataRead:
        loop.insert(loop.end(), {0xad, 0x17, 0x60});
        break;
    case Probe::DataWrite:
        loop.insert(loop.end(), {0xa9, 0x5a, 0x8d, 0x17, 0x60});
        break;
    case Probe::RegisterWrite:
        loop.insert(loop.end(), {0xa9, 0x5a, 0x8d, 0x80, 0x7f});
        break;
    case Probe::RegisterRead:
        loop.insert(loop.end(), {0xad, 0x80, 0x7f});
        break;
    }
    worker.insert(worker.end(), loop.begin(), loop.end());
    worker.insert(worker.end(), {0x80, uint8_t(-int(loop.size()) - 2)});

    std::vector<uint8_t> boot{0x78};
    store(boot, 0x2100, 0x80);
    for (unsigned byte = 0; byte < 16; ++byte)
        store(boot, uint16_t(0x1100 + byte), 0);
    boot.insert(boot.end(), {0xa2, 0, 0xbd, 0, 0x81, 0x9f, 0, 0, 0x7e, 0xe8, 0xe0,
                             uint8_t(worker.size()), 0xd0, 0xf4, 0x5c, 0, 0, 0x7e});

    std::vector<uint8_t> interrupt{0xad, 0x37, 0x21};
    constexpr std::array<uint16_t, 4> counters{0x213c, 0x213c, 0x213d, 0x213d};
    for (unsigned byte = 0; byte < counters.size(); ++byte)
        interrupt.insert(interrupt.end(),
                         {0xad, uint8_t(counters[byte]), 0x21, 0x8d, uint8_t(byte), 0x11});
    store(interrupt, 0x7f51, 1);
    store(interrupt, 0x1108, 0xa5);
    interrupt.push_back(0xdb);

    std::vector<uint8_t> image(0x80c00, 0);
    std::fill_n(image.begin(), 0x80000, 0xea);
    std::copy(boot.begin(), boot.end(), image.begin());
    std::copy(worker.begin(), worker.end(), image.begin() + 0x100);
    std::fill_n(image.begin() + 0x1000, unsigned(count) * 2, 0);
    constexpr std::array<uint16_t, 3> program{0x642a, 0xe060, 0xfc00};
    for (unsigned word = 0; word < program.size(); ++word) {
        const unsigned offset = 0x1000 + (count + word) * 2;
        image[offset] = uint8_t(program[word]);
        image[offset + 1] = uint8_t(program[word] >> 8);
    }
    std::copy(interrupt.begin(), interrupt.end(), image.begin() + 0x2000);
    std::fill_n(image.begin() + 0x7fb0, 80, 0);
    image[0x7fbf] = 0x10;
    constexpr std::array<uint8_t, 11> header{0x20, 0xf3, 9, 0, 1, 0, 0, 255, 255, 0, 0};
    std::copy(header.begin(), header.end(), image.begin() + 0x7fd5);
    image[0x7fd9] = pal ? 2 : 1;
    image[0x7ffc] = 0;
    image[0x7ffd] = 0x80;
    image[0x7ffa] = image[0x7ffe] = 0;
    image[0x7ffb] = image[0x7fff] = 0xa0;
    return image;
}

inline std::vector<uint8_t> RetainedStorageProgram(bool consume, bool pal) {
    auto image = IrqProgram(127, 0, pal);
    if (!consume) {
        image[0x11a] = 16;
        std::fill_n(image.begin() + 0x1000, 512, 0);
        const auto word = [&](unsigned address, uint16_t value) {
            image[0x1000 + address * 2] = uint8_t(value);
            image[0x1001 + address * 2] = uint8_t(value >> 8);
        };
        // Eight nested calls leave every return-stack entry in use. The consumer
        // returns through each one before selecting the completed result.
        for (unsigned depth = 0; depth < 8; ++depth) {
            const unsigned address = 16 * (depth + 1);
            word(address, uint16_t(0x2800 | (address + 16)));
            if (depth)
                word(address + 1, 0x3c00);
        }
        for (unsigned address : {0, 17, 144}) {
            word(address, address == 0 ? 0x647d : 0x642a);
            word(address + 1, 0xe060);
            word(address + 2, 0xfc00);
        }
        word(240, 0x3c00);
        return image;
    }

    const auto store = [](std::vector<uint8_t>& code, uint16_t address, uint8_t value) {
        code.insert(code.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
    };
    std::vector<uint8_t> worker;
    for (uint16_t address : {0x7f49, 0x7f4a, 0x7f4b, 0x7f4d, 0x7f4e})
        store(worker, address, 0);
    store(worker, 0x7f4f, 148);
    store(worker, 0x7f48, 1);
    worker.insert(worker.end(), {0x58, 0x80, 0xfe});
    image[97] = uint8_t(worker.size());
    std::copy(worker.begin(), worker.end(), image.begin() + 0x100);

    std::vector<uint8_t> interrupt{0xad, 0x37, 0x21};
    constexpr std::array<uint16_t, 4> counters{0x213c, 0x213c, 0x213d, 0x213d};
    for (unsigned byte = 0; byte < counters.size(); ++byte)
        interrupt.insert(interrupt.end(),
                         {0xad, uint8_t(counters[byte]), 0x21, 0x8d, uint8_t(byte), 0x11});
    interrupt.insert(interrupt.end(), {0xad, 0x80, 0x7f, 0x8d, 5, 0x11});
    store(interrupt, 0x7f51, 1);
    store(interrupt, 0x1108, 0xa5);
    interrupt.push_back(0xdb);
    std::copy(interrupt.begin(), interrupt.end(), image.begin() + 0x2000);
    return image;
}

} // namespace snes::tests::cx4
