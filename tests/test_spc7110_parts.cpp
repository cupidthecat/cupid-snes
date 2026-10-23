#include "snes/core/EpsonRtc.hpp"
#include "snes/core/Spc7110Decoder.hpp"

#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void CodingRows() {
    constexpr uint32_t rows[3][2]{{0x03, 0x03}, {0, 0x180c}, {0x2033393b, 0x3f4089ff}};
    for (unsigned mode = 0; mode < 3; ++mode) {
        snes::core::Spc7110Decoder decoder([](uint32_t address) {
            address &= 0x1fffff;
            return uint8_t((address * 37) ^ (address >> 4) ^ 0xa5);
        });
        decoder.Initialize(mode, 0x1234);
        for (unsigned row = 0; row < 2; ++row) {
            const uint32_t mask = mode == 0 ? 0xff : mode == 1 ? 0xffff : 0xffffffff;
            Check((decoder.NextRow() & mask) == rows[mode][row],
                  "Compressed rows match recorded plane-order vectors");
        }
    }
    snes::core::Spc7110Decoder invalid([](uint32_t) { return uint8_t(0); });
    bool rejected = false;
    try {
        invalid.Initialize(3, 0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Check(rejected, "Reserved compression modes cannot create coding planes");
}

void RtcPorts() {
    snes::core::EpsonRtc rtc([] { return int64_t(1700000000); });
    rtc.Write(0, 1);
    rtc.Write(1, 3);
    Check(rtc.Read(2, 0) == 0, "Selecting the RTC mode starts an eight-tick wait");
    rtc.Advance(71);
    Check(rtc.Read(2, 0) == 0, "Seven oscillator ticks leave the port busy");
    rtc.Advance(1);
    Check(rtc.Read(2, 0) == 0x80 && rtc.Read(2, 0) == 0x80,
          "The eighth tick restores persistent readiness");
    rtc.Write(1, 0);
    rtc.Advance(82);
    rtc.Write(1, 5);
    Check(rtc.Read(1, 0) == 0, "A pending calendar write blocks data reads");
    rtc.Advance(82);
    Check(rtc.Read(1, 0) == 5, "Write mode returns the last accepted nibble");
    Check(rtc.Read(3, 0x5a) == 0x5a, "The unused RTC port preserves open bus");
}

void ColdRtc() {
    snes::core::EpsonRtc rtc([] { return int64_t(1700000000); });
    const auto saved = rtc.Save();
    for (unsigned index = 0; index < 16; ++index)
        Check(saved[index] == (index == 1 ? 8 : 0), "A fresh RTC has cleared calendar and control "
                                                    "storage with its battery-failure flag set");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            CodingRows();
            RtcPorts();
        }
        if (argc == 1 || std::string_view(argv[1]) == "cold")
            ColdRtc();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("SPC7110 coding and RTC port tests passed");
}
