#include "snes/core/Srtc.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string_view>

using namespace snes::core;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void ProgrammedDigits() {
    Srtc clock([] { return int64_t(0); });
    for (uint8_t value : {uint8_t(9), uint8_t(10)}) {
        clock.Write(14);
        clock.Write(4);
        clock.Write(14);
        clock.Write(0);
        clock.Write(value);
        clock.Write(13);
        Check(clock.Read() == 15, "Reading a programmed clock starts with its marker");
        Check(clock.Read() == value % 10,
              "Programmed seconds expose decimal units even with an incomplete date");
        Check(clock.Read() == value / 10, "Programmed seconds retain the decimal carry");
    }
}

void ColdCalendar() {
    Srtc clock([] { return int64_t(1700000000); });
    const auto saved = clock.Save();
    for (unsigned index = 0; index < 13; ++index)
        Check(saved[index] == 0,
              "A fresh clock has cleared calendar storage before synchronization or loading");
}

std::array<uint8_t, 13> Calendar(Srtc& clock) {
    clock.Write(13);
    Check(clock.Read() == 15, "A calendar stream begins with its marker");
    std::array<uint8_t, 13> result;
    for (auto& value : result)
        value = clock.Read();
    Check(clock.Read() == 15, "A calendar stream ends with its marker");
    return result;
}

void Program(Srtc& clock, const std::array<uint8_t, 12>& digits) {
    clock.Write(14);
    clock.Write(0);
    for (uint8_t digit : digits)
        clock.Write(digit);
}

void OscillatorEdges() {
    for (bool pal : {false, true}) {
        const uint32_t frequency = pal ? 21'281'370 : 21'477'272;
        Srtc clock([] { return int64_t(0); });
        clock.SetPal(pal);
        clock.Advance(0);
        Check(Calendar(clock)[0] == 0, "Zero elapsed clocks preserve the calendar");
        clock.Advance(1);
        Check(Calendar(clock)[0] == 1,
              "The first positive clock interval runs the initial one-Hz tick");
        clock.Advance(frequency - 1);
        Check(Calendar(clock)[0] == 1, "The final oscillator clock keeps the next second pending");
        clock.Advance(1);
        Check(Calendar(clock)[0] == 2, "The next oscillator interval advances a second");
        clock.Reset();
        Check(Calendar(clock)[0] == 2, "Reset preserves calendar storage");
        clock.Advance(1);
        Check(Calendar(clock)[0] == 3, "Reset restarts the oscillator phase");
    }
}

void CalendarCarries() {
    for (uint8_t century : {uint8_t(10), uint8_t(11), uint8_t(12)}) {
        Srtc clock([] { return int64_t(0); });
        Program(clock, {9, 5, 9, 5, 3, 2, 8, 2, 2, 0, 0, century});
        const auto weekday = Calendar(clock)[12];
        clock.Advance(1);
        const auto date = Calendar(clock);
        Check(date[0] == 0 && date[2] == 0 && date[4] == 0, "A midnight carry resets time fields");
        Check(date[6] == (century == 12 ? 9 : 1) && date[8] == (century == 12 ? 2 : 3),
              "February length follows the stored year counter");
        Check(date[12] == weekday, "Calendar carries retain the programmed weekday");
    }
    Srtc invalid([] { return int64_t(0); });
    Program(invalid, {});
    const auto date = Calendar(invalid);
    Check(date[6] == 0 && date[8] == 0 && date[12] == 3,
          "Weekday calculation clamps incomplete dates without rewriting their stored digits");
}

void Persistence() {
    int64_t host = 1700000000;
    Srtc clock([&] { return host; });
    Program(clock, {9, 5, 9, 5, 3, 2, 8, 2, 2, 4, 2, 10});
    const auto before = Calendar(clock);
    const auto saved = clock.Save();
    host += 86401;
    Check(Calendar(clock) == before, "Host time alone leaves an active calendar unchanged");
    Srtc restored([&] { return host; });
    Check(restored.Load(saved), "The existing clock-save format loads");
    const auto after = Calendar(restored);
    Check(after[0] == 0 && after[6] == 1 && after[8] == 3 && after[12] == before[12],
          "Loading applies elapsed days and seconds while retaining weekday storage");
    const auto rollback = restored.Save();
    --host;
    Check(restored.Load(rollback) && Calendar(restored) == after,
          "A future save timestamp does not rewind the clock");
    auto bad = saved;
    bad[3] = 16;
    Check(!restored.Load(bad) && Calendar(restored) == after,
          "Rejected calendar data retains the running state");
    bad = saved;
    bad[23] = 0x80;
    Check(!restored.Load(bad), "Negative save timestamps are rejected");
}

void ReloadPhase() {
    for (bool pal : {false, true}) {
        int64_t host = 1700000000;
        Srtc clock([&] { return host; });
        clock.SetPal(pal);
        clock.Advance(1);
        const auto saved = clock.Save();
        host += 61;
        Check(clock.Load(saved), "A running clock accepts saved calendar data");
        const auto resumed = Calendar(clock);
        Check(resumed[0] == 2 && resumed[2] == 1, "Reloading applies elapsed host time once");
        clock.Advance((pal ? 21'281'370u : 21'477'272u) - 1);
        Check(Calendar(clock) == resumed, "Reloading retains the pending oscillator interval");
        clock.Advance(1);
        Check(Calendar(clock)[0] == 3, "The retained oscillator reaches its original next tick");
    }
}

void ReloadSerialState() {
    for (unsigned state = 0; state < 4; ++state) {
        Srtc clock([] { return int64_t(1700000000); });
        clock.Advance(1);
        const auto saved = clock.Save();
        if (state == 0) {
            clock.Write(13);
            Check(clock.Read() == 15 && clock.Read() == 1, "Read two nibbles before loading");
        } else {
            clock.Write(14);
            if (state == 1) {
                clock.Write(0);
                clock.Write(7);
            } else if (state == 3) {
                clock.Write(4);
            }
        }
        Check(clock.Load(saved), "Loading restores calendar data during a serial transaction");
        if (state == 0) {
            Check(clock.Read() == 0, "Reloading retains the read cursor");
        } else if (state == 1) {
            clock.Write(5);
            const auto date = Calendar(clock);
            Check(date[0] == 1 && date[1] == 5,
                  "Reloading retains write mode and its digit cursor");
        } else if (state == 2) {
            clock.Write(4);
            Check(Calendar(clock) == std::array<uint8_t, 13>{}, "Reloading retains command mode");
        } else {
            Check(clock.Read() == 0, "Reloading retains the idle serial state");
            Check(Calendar(clock)[0] == 1, "An idle reload still restores calendar storage");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1 || std::string_view(argv[1]) == "digits")
            ProgrammedDigits();
        if (argc == 1 || std::string_view(argv[1]) == "cold")
            ColdCalendar();
        if (argc == 1 || std::string_view(argv[1]) == "reload-phase")
            ReloadPhase();
        if (argc == 1 || std::string_view(argv[1]) == "reload-protocol")
            ReloadSerialState();
        if (argc == 1) {
            OscillatorEdges();
            CalendarCarries();
            Persistence();
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("S-RTC tests passed");
    return 0;
}
