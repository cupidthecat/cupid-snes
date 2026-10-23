#include "snes/core/gameboy/Host.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

using snes::core::gameboy::Host;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

using Packet = std::array<uint8_t, 16>;

void Bit(Host& host, bool value) {
    host.JoypadWrite(true, true);
    host.JoypadWrite(value, !value);
}

void Send(Host& host, const Packet& packet, bool stop = true) {
    host.JoypadWrite(false, false);
    for (auto byte : packet)
        for (unsigned bit = 0; bit < 8; ++bit)
            Bit(host, byte & (1u << bit));
    if (stop)
        Bit(host, false);
}

void ExpectPacket(Host& host, const Packet& packet) {
    Check(host.Read(0x6002) == 1, "A complete packet makes the command port ready");
    for (unsigned i = 0; i < packet.size(); ++i)
        Check(host.Read(0x7000 + i) == packet[i], "The command bytes retain serial bit order");
}

void PortsAndReset() {
    unsigned resets = 0;
    Host* current = nullptr;
    Host host([&] {
        ++resets;
        Check(current->Control() == 0 && current->Divider() == 5,
              "The handheld resets after the host registers return to power state");
        Check(current->Read(0x6002) == 0 && current->Read(0x7000) == 0 &&
                  current->Read(0x7800) == 0xff,
              "Reset clears packets and restores the video buffer before the callback");
    });
    current = &host;
    for (uint32_t bank : {0u, 1u, 0x3fu, 0x80u, 0xbfu}) {
        Check(host.Read(bank * 0x10000 + 0x600f) == 0x21, "The revision register mirrors by bank");
        Check(host.Read(bank * 0x10000 + 0x6003) == 0, "Unused read ports return zero");
    }
    Check(host.Read(0x40600f) == 0, "The read decoder retains address bit 22");
    Check(host.Control() == 0 && host.Divider() == 5, "Power uses the normal oscillator divisor");
    constexpr unsigned divisors[] = {4, 5, 7, 9};
    for (unsigned bits = 0; bits < 4; ++bits) {
        host.Write(0x806003, uint8_t(bits));
        Check(host.Divider() == divisors[bits] && resets == 0,
              "Changing clock speed while halted does not reset the handheld");
    }
    host.Write(0x6003, 0x83);
    host.Write(0x6003, 0xb1);
    Check(resets == 1 && host.Control() == 0xb1 && host.Divider() == 5,
          "Only a rising run bit resets the handheld");
    host.Write(0x6003, 0);
    host.Write(0x6003, 0x82);
    Check(resets == 2 && host.Divider() == 7, "Reset applies the new divisor after the callback");
}

void VideoTiles() {
    Host host;
    std::array<uint8_t, 2048> expected;
    expected.fill(0xff);
    for (unsigned line = 0; line < 40; ++line) {
        const unsigned bank = (line / 8) & 3;
        for (unsigned x = 0; x < 200; ++x) {
            const unsigned color = (x * 3 + line * 7 + (x >> 3)) & 3;
            host.Pixel(uint8_t(color));
            if (x < 160) {
                for (unsigned plane = 0; plane < 2; ++plane) {
                    const unsigned address = bank * 512 + (line & 7) * 2 + (x / 8) * 16 + plane;
                    const auto mask = uint8_t(0x80 >> (x & 7));
                    expected[address] =
                        uint8_t((expected[address] & ~mask) | (((color >> plane) & 1) ? mask : 0));
                }
            }
        }
        host.BeginLine();
        Check(host.Read(0x6000) == uint8_t(((line + 1) & 0xf8) | (((line + 1) / 8) & 3)),
              "The line port combines the eight-line count and circular write bank");
    }
    for (unsigned bank = 0; bank < 4; ++bank) {
        host.Write(0x6001, uint8_t(bank | 0xfc));
        for (unsigned i = 0; i < 1024; ++i)
            Check(host.Read(0x7800) == expected[bank * 512 + (i & 511)],
                  "Video reads preserve planar tile bytes and wrap within the selected bank");
    }
    host.BeginFrame();
    Check(host.Read(0x6000) == 1, "A frame boundary resets counters and retains the write bank");
    host.Reset();
    for (unsigned x = 0; x < 256; ++x)
        host.Pixel(0);
    host.Pixel(3);
    host.Write(0x6001, 0);
    Check(host.Read(0x7800) == 1 && host.Read(0x7800) == 1,
          "The eight-bit horizontal counter wraps after pixels outside the visible area");
}

void PacketsAndOverflow() {
    Host host;
    Packet packet{};
    for (unsigned i = 0; i < packet.size(); ++i)
        packet[i] = uint8_t(i * 17);
    Send(host, packet, false);
    Check(host.Read(0x6002) == 0, "Sixteen bytes remain unavailable until the stop bit");
    Bit(host, true);
    Check(host.Read(0x6002) == 0, "A one stop bit leaves the complete packet pending");
    Bit(host, false);
    ExpectPacket(host, packet);
    Check(host.Read(0x6002) == 0 && host.Read(0x7001) == packet[1],
          "An empty queue keeps the previous command bytes");

    host.JoypadWrite(false, false);
    host.JoypadWrite(false, true);
    for (unsigned bit = 0; bit < 140; ++bit)
        Bit(host, false);
    Check(host.Read(0x6002) == 0, "Missing release pulses abort a malformed packet");

    for (unsigned index = 0; index < 65; ++index) {
        packet.fill(uint8_t(index));
        Send(host, packet);
    }
    for (unsigned index = 0; index < 64; ++index) {
        packet.fill(uint8_t(index));
        ExpectPacket(host, packet);
        if (index == 32) {
            Packet tail;
            tail.fill(0xee);
            Send(host, tail);
        }
    }
    packet.fill(0xee);
    ExpectPacket(host, packet);
    Check(host.Read(0x6002) == 0, "A full queue drops the newest packet and recovers after reads");
}

void Controllers() {
    Host host;
    host.Write(0x6004, 0xa5);
    Check(host.JoypadWrite(false, true) == 5 && host.JoypadWrite(true, false) == 10 &&
              host.JoypadWrite(false, false) == 0 && host.JoypadWrite(true, true) == 15,
          "Joypad select lines combine active-low direction and button nibbles");
    host.Reset();
    host.Write(0x6003, 0x30);
    for (unsigned i = 0; i < 4; ++i)
        host.Write(0x6004 + i, uint8_t((i + 1) * 0x11));
    Check(host.JoypadWrite(true, true) == 15, "The player counter starts locked at player one");
    for (unsigned i = 1; i <= 8; ++i) {
        host.JoypadWrite(true, false);
        Check(host.JoypadWrite(true, true) == 15 - (i & 3),
              "A button-select write unlocks the next multiplayer identifier");
        Check(host.JoypadWrite(false, true) == (i & 3) + 1,
              "The selected controller supplies its own inputs");
    }
    host.JoypadWrite(true, false);
    host.JoypadWrite(true, false);
    Check(host.JoypadWrite(true, true) == 15,
          "Repeated button-select writes toggle the controller lock on each write");
    for (unsigned i = 0; i < 3; ++i) {
        host.JoypadWrite(true, false);
        host.JoypadWrite(true, true);
    }
    host.Write(0x6003, 0x10);
    Check(host.JoypadWrite(true, true) == 13,
          "A two-player control write clears only the low identifier bit");
    host.Write(0x6003, 0);
    Check(host.JoypadWrite(true, true) == 13,
          "A one-player control write retains the current identifier until the next poll");
    host.JoypadWrite(true, false);
    Check(host.JoypadWrite(true, true) == 15,
          "The next poll restricts the identifier to the active player count");
}
} // namespace

int main() {
    try {
        PortsAndReset();
        VideoTiles();
        PacketsAndOverflow();
        Controllers();
        std::puts("Super Game Boy register, tile, packet and controller checks passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
