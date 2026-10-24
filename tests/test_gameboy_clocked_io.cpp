#include "snes/core/gameboy/ClockedIo.hpp"

#include <cstdio>
#include <stdexcept>
#include <vector>

using snes::core::gameboy::ClockedIo;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Machine final : ClockedIo::Bus {
    uint8_t interrupts = 0;
    std::vector<bool> audioEdges;
    ClockedIo clock{*this};

    void RequestInterrupt(uint8_t mask) override {
        interrupts |= mask;
    }

    void AudioDividerEdge(bool rising) override {
        audioEdges.push_back(rising);
    }
};

void RatesAndDividerEdges() {
    constexpr unsigned periods[] = {1024, 16, 64, 256};
    for (unsigned mode = 0; mode < 4; ++mode) {
        Machine machine;
        machine.clock.Write(0xff04, 0xff);
        machine.clock.Write(0xff07, uint8_t(4 | mode));
        machine.clock.Advance(periods[mode] - 1);
        Check(machine.clock.Read(0xff05) == 0, "The timer waits for its divider falling edge");
        machine.clock.Advance(1);
        Check(machine.clock.Read(0xff05) == 1 && machine.interrupts == 0,
              "Each clock selection uses the specified divider period");
        machine.clock.Advance(periods[mode] * 3);
        Check(machine.clock.Read(0xff05) == 4, "Batched advancement preserves timer edge counts");
    }
    Machine glitch;
    glitch.clock.Write(0xff07, 5);
    glitch.clock.Write(0xff04, 0);
    Check(glitch.clock.Read(0xff05) == 1, "Resetting a high timer input clocks TIMA once");
    glitch.clock.Advance(8);
    glitch.clock.Write(0xff07, 0);
    Check(glitch.clock.Read(0xff05) == 2, "Disabling a high timer input clocks TIMA once");
    glitch.clock.Write(0xff07, 5);
    glitch.clock.Write(0xff07, 6);
    Check(glitch.clock.Read(0xff05) == 3, "Changing divider inputs can produce a timer edge");
    glitch.clock.Advance(8192 - 8);
    Check(glitch.audioEdges.size() == 2 && glitch.audioEdges[0] && !glitch.audioEdges[1],
          "The audio sequencer receives both transitions of divider bit twelve");
}

void OverflowAndWriteWindows() {
    Machine machine;
    machine.clock.Write(0xff04, 0);
    machine.clock.Write(0xff06, 0x42);
    machine.clock.Write(0xff05, 0xff);
    machine.clock.Write(0xff07, 5);
    machine.clock.Advance(16);
    Check(machine.clock.Read(0xff05) == 0 && machine.interrupts == 0,
          "Overflow exposes zero before asserting the timer interrupt");
    machine.clock.Write(0xff06, 0x53);
    machine.clock.Advance(3);
    Check(machine.clock.Read(0xff05) == 0 && machine.interrupts == 0,
          "The reload window lasts a full machine cycle");
    machine.clock.Advance(1);
    Check(machine.clock.Read(0xff05) == 0x53 && machine.interrupts == 4,
          "A modulo write during overflow supplies the reloaded value");
    machine.clock.Write(0xff05, 0x99);
    Check(machine.clock.Read(0xff05) == 0x53, "TIMA writes are ignored during the reload cycle");
    machine.clock.Write(0xff06, 0x64);
    Check(machine.clock.Read(0xff05) == 0x64, "Modulo writes update TIMA during reload");
    machine.clock.Advance(4);
    machine.clock.Write(0xff05, 0x75);
    Check(machine.clock.Read(0xff05) == 0x75, "TIMA becomes writable after the reload cycle");
}

void SerialAndStop() {
    Machine machine;
    machine.clock.Write(0xff04, 0);
    machine.clock.Write(0xff01, 0);
    machine.clock.Write(0xff02, 0x81);
    machine.clock.Advance(4095);
    Check(machine.clock.Read(0xff01) == 0x7f && (machine.clock.Read(0xff02) & 0x80) &&
              machine.interrupts == 0,
          "Internal serial transfer shifts seven bits before its last clock edge");
    machine.clock.Advance(1);
    Check(machine.clock.Read(0xff01) == 0xff && !(machine.clock.Read(0xff02) & 0x80) &&
              machine.interrupts == 8,
          "An unconnected serial port receives ones and interrupts after eight bits");
    machine.clock.Write(0xff02, 0x80);
    machine.clock.Advance(8192);
    Check(machine.clock.Read(0xff02) & 0x80, "External serial mode waits for external clocks");

    machine.clock.Write(0xff07, 5);
    machine.clock.Write(0xff05, 0);
    machine.clock.EnterStop(false);
    const auto timer = machine.clock.Read(0xff05);
    machine.clock.Advance(8192);
    Check(machine.clock.Read(0xff04) == 0 && machine.clock.Read(0xff05) == timer,
          "STOP freezes divider-driven peripherals");
    machine.clock.LeaveStop();
    machine.clock.Advance(19);
    Check(machine.clock.Read(0xff05) == timer, "STOP holds divider reset when IME is clear");
    machine.clock.Advance(1);
    Check(machine.clock.Read(0xff05) == uint8_t(timer + 1),
          "Divider counting resumes after the held reset interval");
}
} // namespace

int main() {
    try {
        RatesAndDividerEdges();
        OverflowAndWriteWindows();
        SerialAndStop();
        std::puts("Handheld divider, timer, serial and STOP checks passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
