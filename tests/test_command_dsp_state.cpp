#include "snes/core/CompetitionBoard.hpp"
#include "snes/core/Dsp1.hpp"
#include "snes/core/Dsp2.hpp"
#include "snes/core/Dsp3.hpp"
#include "snes/core/Dsp4.hpp"
#include "snes/core/NecDsp.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/UpdDsp.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

using namespace snes::core;
using snes::core::detail::StateArchive;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class Device> std::vector<uint8_t> Capture(Device& device) {
    StateArchive archive;
    device.SerializeState(archive);
    return archive.Bytes();
}

template <class Device> void Restore(Device& device, std::span<const uint8_t> bytes) {
    StateArchive archive(bytes);
    device.SerializeState(archive);
    archive.Commit();
}

template <class Device> void Reject(Device& device, std::vector<uint8_t> bytes) {
    const auto before = Capture(device);
    bool rejected = false;
    try {
        Restore(device, bytes);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Check(rejected, "Invalid DSP state must reject");
    Check(Capture(device) == before,
          "Rejected state retains all registers, RAM and transfer phases");
}

template <class Device> void Truncation(Device& device) {
    const auto bytes = Capture(device);
    for (size_t prefix = 0; prefix < bytes.size(); prefix += std::max(size_t{1}, bytes.size() / 23))
        Reject(device, {bytes.begin(), bytes.begin() + prefix});
    Reject(device, {bytes.begin(), bytes.end() - 1});
    auto extra = bytes;
    extra.push_back(0);
    Reject(device, extra);
}

// Every cut resumes all remaining byte-port operations, not just the next transfer.
template <class Device> struct Trace {
    std::vector<std::function<uint64_t(Device&)>> operations;

    void Add(std::function<uint64_t(Device&)> operation) {
        operations.push_back(std::move(operation));
    }

    void Reset() {
        Add([](auto& chip) {
            chip.Reset();
            return 0;
        });
    }

    void Put(uint8_t byte) {
        Add([byte](auto& chip) {
            if constexpr (std::is_same_v<Device, Dsp1>)
                chip.SetDr(byte);
            else if constexpr (std::is_same_v<Device, Dsp2>)
                chip.Write(byte);
            else
                chip.Write(0x8000, byte);
            return 0;
        });
    }

    void Word(int word) {
        Put(uint8_t(word));
        Put(uint8_t(unsigned(word) >> 8));
    }

    void Read(unsigned count) {
        for (unsigned index = 0; index < count; ++index)
            Add([](auto& chip) {
                if constexpr (std::is_same_v<Device, Dsp1>)
                    return chip.GetDr();
                else if constexpr (std::is_same_v<Device, Dsp2>)
                    return chip.Read();
                else
                    return chip.Read(0x8000);
            });
    }

    template <class Factory> void Verify(Factory factory) const {
        auto baseline = factory();
        std::vector<uint64_t> expected;
        for (const auto& operation : operations)
            expected.push_back(operation(*baseline));
        const auto final = Capture(*baseline);
        auto source = factory(), target = factory();
        for (size_t cut = 0; cut <= operations.size(); ++cut) {
            const auto saved = Capture(*source);
            Restore(*target, saved);
            Check(Capture(*target) == saved, "Restore preserves the exact DSP transfer boundary");
            for (size_t next = cut; next < operations.size(); ++next)
                Check(operations[next](*target) == expected[next],
                      "Restored DSP reproduces every later bus read and clock edge");
            Check(Capture(*target) == final, "Continuation retains all private DSP storage");
            if (cut < operations.size())
                Check(operations[cut](*source) == expected[cut], "Deterministic trace");
        }
        Truncation(*target);
    }

    void Verify() const {
        Verify([] { return std::make_unique<Device>(); });
    }
};

void FrozenRegisterRead() {
    for (uint8_t command : {0x1a, 0x2a, 0x3a}) {
        Dsp1 chip;
        chip.SetDr(command);
        Check((chip.GetSr() | chip.GetSr()) == 4,
              "Freeze retains byte mode and clears the request");
        for (unsigned repeat = 0; repeat < 4; ++repeat) {
            Check(chip.GetDr() == command,
                  "Frozen DSP-1 data reads return the retained register byte");
            chip.SetDr(0xff);
        }
        Dsp1 target;
        Restore(target, Capture(chip));
        Check(target.GetDr() == command, "Frozen data register survives a checkpoint");
        target.Reset();
        Check(target.GetDr() == 0x80, "Reset releases a frozen data register");
    }
}

void GeometryContinuation() {
    Trace<Dsp1> trace;
    const auto command = [&](uint8_t number, std::initializer_list<int> parameters,
                             unsigned output) {
        trace.Put(number);
        for (auto word : parameters)
            trace.Word(word);
        trace.Read(output);
    };
    for (unsigned matrix = 0; matrix < 3; ++matrix)
        command(uint8_t(1 + matrix * 16), {0x4000, 0x1000, 0x2000, 0x3000}, 0);
    for (uint8_t number : {0x09, 0x19, 0x29})
        command(number, {12, 24, 36}, 6);
    command(2, {10, 20, 30, 100, 200, 0x1000, 0x2000}, 8);
    command(6, {40, 50, 60}, 6);
    command(0x0e, {32, 48}, 4);
    command(0x0a, {1}, 16);
    trace.Word(0x8000);
    command(0, {0x4000, 0x3000}, 2);
    trace.Add([](auto& chip) { return chip.GetSr(); });
    trace.Verify();

    Trace<Dsp1> dump;
    dump.Put(0x17);
    dump.Word(0);
    dump.Read(1031);
    dump.Add([](auto& chip) { return chip.GetSr(); });
    dump.Reset();
    dump.Put(0);
    dump.Word(0x4000);
    dump.Word(0x3000);
    dump.Read(2);
    dump.Verify();
    Dsp1 chip;
    chip.SetDr(0x17);
    chip.SetDr(0);
    chip.SetDr(0);
    std::array<uint8_t, 512> block{};
    for (auto& byte : block)
        byte = chip.GetDr();
    for (unsigned repeat = 0; repeat < 3; ++repeat)
        for (auto byte : block)
            Check(chip.GetDr() == byte, "Command dump retains its eight-bit word-counter wrap");
    Check((chip.GetSr() | chip.GetSr()) == 0x80, "Wrapped command dump remains in word mode");
    alignas(Dsp1) std::array<std::byte, sizeof(Dsp1)> first, second;
    first.fill(std::byte{0x35});
    second.fill(std::byte{0xca});
    auto* a = std::construct_at(reinterpret_cast<Dsp1*>(first.data()));
    auto* b = std::construct_at(reinterpret_cast<Dsp1*>(second.data()));
    Check(Capture(*a) == Capture(*b), "Cold DSP-1 state never depends on host allocation contents");
    std::destroy_at(a);
    std::destroy_at(b);
}

void BitmapContinuation() {
    Trace<Dsp2> trace;
    trace.Put(3);
    trace.Put(0x0a);
    trace.Put(1);
    for (unsigned index = 0; index < 32; ++index)
        trace.Put(uint8_t(index * 37));
    trace.Read(7);
    trace.Put(9);
    trace.Word(0x1234);
    trace.Word(0x5678);
    trace.Read(4);
    trace.Put(5);
    trace.Put(17);
    for (unsigned index = 0; index < 34; ++index)
        trace.Put(uint8_t(index * 19));
    trace.Read(19);
    trace.Put(6);
    trace.Put(9);
    for (unsigned index = 0; index < 9; ++index)
        trace.Put(uint8_t(index * 13));
    trace.Read(11);
    trace.Put(0x0d);
    trace.Put(21);
    trace.Put(29);
    for (unsigned index = 0; index < 11; ++index)
        trace.Put(uint8_t(index * 31));
    trace.Read(31);
    for (uint8_t number : {5, 6, 0x0d}) {
        trace.Put(number);
        trace.Put(0);
        if (number == 0x0d)
            trace.Put(0);
        trace.Read(1);
    }
    trace.Reset();
    trace.Put(9);
    trace.Word(2);
    trace.Word(3);
    trace.Read(4);
    trace.Verify();
    Dsp2 chip;
    auto bad = Capture(chip);
    bad[1024] = bad[1025] = 0xff;
    Reject(chip, bad);
}

struct Bits {
    std::vector<uint16_t> words;
    unsigned size = 0;

    void Add(unsigned value, unsigned count) {
        for (unsigned bit = count; bit; --bit) {
            if (!(size & 15))
                words.push_back(0);
            words.back() |= uint16_t(((value >> (bit - 1)) & 1) << (15 - (size++ & 15)));
        }
    }
};

void MapContinuation() {
    Trace<Dsp3> trace;
    trace.Put(6);
    trace.Word(0x1010);
    for (unsigned direction = 0; direction < 6; ++direction) {
        trace.Put(7);
        trace.Put(uint8_t(direction));
        trace.Word(0x0808);
        trace.Read(4);
    }
    trace.Put(0x18);
    trace.Word(2);
    for (unsigned tile = 0; tile < 2; ++tile) {
        for (unsigned pair = 0; pair < 4; ++pair)
            trace.Word((1 << (pair * 2)) | (1 << (pair * 2 + 9)));
        trace.Read(8);
    }
    trace.Put(0x3e);
    trace.Word(0x0808);
    trace.Read(2);
    trace.Put(0x1e);
    trace.Word(0x0101);
    for (unsigned side = 0; side < 6; ++side) {
        trace.Read(2);
        trace.Put(side == 2);
        trace.Put(uint8_t(side + 3));
    }
    trace.Read(2);
    trace.Word(0x0101);
    for (unsigned side = 0; side < 6; ++side)
        trace.Read(3);
    trace.Read(2);
    trace.Verify();

    Bits bits;
    bits.Add(0, 2);
    bits.Add(0x41, 9);
    bits.Add(1, 2);
    bits.Add(2, 2);
    bits.Add(0, 1);
    bits.Add(3, 2);
    bits.Add(0, 4);
    bits.Add(0, 2);
    bits.Add(0x100, 9);
    for (unsigned index = 0; index < 3; ++index)
        bits.Add(1, 2);
    bits.Add(0, 1);
    for (unsigned index = 0; index < 4; ++index)
        bits.Add(0, 3);
    for (unsigned index = 0; index < 4; ++index)
        bits.Add(index, 3);
    bits.Add(4, 3);
    bits.Add(0, 1);
    bits.Add(0x35, 8);
    bits.Add(5, 3);
    bits.Add(1, 1);
    bits.Add(0xabc, 12);
    bits.Add(0, 3);
    Trace<Dsp3> decode;
    decode.Put(0x38);
    decode.Word(8);
    decode.Word(7);
    Dsp3 setup;
    for (const auto& operation : decode.operations)
        operation(setup);
    size_t input = 0;
    std::vector<uint16_t> actual;
    for (unsigned step = 0; step < 100 && setup.Read(0xc000) != 0x84; ++step) {
        const bool writing = setup.Read(0xc000) & 0x40;
        const auto old = decode.operations.size();
        if (writing) {
            Check(input < bits.words.size(), "Decode trace stays within its bitstream");
            decode.Word(bits.words[input++]);
        } else
            decode.Read(2);
        unsigned result = 0;
        for (auto index = old; index < decode.operations.size(); ++index)
            result |= unsigned(decode.operations[index](setup)) << ((index - old) * 8);
        if (!writing)
            actual.push_back(uint16_t(result));
    }
    Check(actual ==
              std::vector<uint16_t>({0x41, 0x42, 0x44, 0x48, 0x8002, 0x35, 0x8003, 0xabc, 0x41}),
          "Decode fixture exercises deltas, prefix tables and both copy-offset widths");
    Check(setup.Read(0xc000) == 0x84, "Decode fixture completes");
    decode.Verify();
    Trace<Dsp3> dump;
    dump.Put(0x1f);
    dump.Word(0);
    dump.Read(2048);
    dump.Verify();
    Dsp3 chip;
    auto bad = Capture(chip);
    bad[0] = 0xff;
    Reject(chip, bad);
}

std::vector<int> Road(bool guide, bool lit, bool multiplayer) {
    std::vector<int> words;
    const auto pair = [&](int32_t value) {
        words.push_back(uint16_t(value));
        words.push_back(uint16_t(uint32_t(value) >> 16));
    };
    if (lit)
        words.push_back(0);
    pair(10 * 65536);
    for (int value : {10, 0, 0, 10})
        words.push_back(value);
    pair(4 * 65536);
    for (int value : {100, 0x1000, 0})
        words.push_back(value);
    if (guide) {
        for (int value : {0x4000, 8, -2, 6, 2, 0})
            words.push_back(value);
    } else {
        pair(-2 * 65536);
        pair(0);
        words.push_back(0x4000);
        words.push_back(0);
        if (multiplayer)
            words.push_back(0);
        else
            pair(0);
        for (unsigned index = 0; index < 3; ++index)
            words.push_back(0);
    }
    return words;
}

void ProjectionContinuation() {
    for (unsigned command : {1u, 7u, 0x0du, 0x0fu, 0x10u}) {
        const bool guide = command == 7 || command == 0x10;
        const bool lit = command == 0x0f || command == 0x10;
        Trace<Dsp4> trace;
        trace.Word(command);
        for (auto word : Road(guide, lit, command == 0x0d))
            trace.Word(word);
        trace.Read(guide ? 6 : 10);
        if (lit)
            for (unsigned color = 0; color < 4; ++color) {
                trace.Word(0x4000);
                trace.Word(0x7fff);
                trace.Read(2);
            }
        trace.Read((guide ? 3 : 5) * 6);
        trace.Word(0x8000);
        trace.Word(0);
        trace.Word(6);
        trace.Word(7);
        trace.Read(4);
        trace.Verify();
    }
    Trace<Dsp4> objects;
    objects.Word(3);
    objects.Word(5);
    for (unsigned index = 0; index < 130; ++index) {
        objects.Word(0x0b);
        objects.Word(-1);
        objects.Word(int(index % 28) * 8);
        objects.Word(0x1234);
        objects.Read(index < 128 ? 6 : 2);
    }
    objects.Word(6);
    objects.Read(32);
    objects.Verify();
    Trace<Dsp4> windows;
    windows.Word(8);
    for (unsigned index = 0; index < 4; ++index)
        windows.Word(255);
    for (unsigned index = 0; index < 12; ++index)
        windows.Word(0);
    for (int word : {80, 160, 100, 120, 0x1000, 0x1000, 0x2000, 0x2000, 10, 10, 10,  10, 0,   0, 0,
                     0,  0,   0,   0,   0,      0x4000, 0,      10,     0,  10, -20, 20, -20, 20})
        windows.Word(word);
    windows.Read(2);
    for (int word : {0x4000, 0, 8, 0, 8, -20, 20, -20, 20})
        windows.Word(word);
    windows.Read(20);
    windows.Word(0x8000);
    windows.Read(2);
    windows.Verify();
    Trace<Dsp4> stream;
    stream.Word(3);
    stream.Word(5);
    stream.Word(9);
    for (int word : {128, 0, 0, 0, 255, 0, 224, 100, 0x4000, 0, 0, 0, -20, 0x10, 0x2000, 0, 0})
        stream.Word(word);
    stream.Read(14);
    stream.Word(0x8000);
    stream.Word(6);
    stream.Read(32);
    stream.Word(5);
    stream.Word(9);
    for (int word : {128, 0, 0, 0, 255, 0, 224, 150, 0x9000, 0, 0, 20, 0, -10, 0x4000, 10})
        stream.Word(word);
    stream.Read(2);
    for (int word : {0, 0x20, 0x2000, 0, 0})
        stream.Word(word);
    stream.Read(8);
    stream.Word(0x8000);
    stream.Verify();
    Trace<Dsp4> large;
    auto words = Road(false, false, false);
    words[0] = 0;
    words[1] = words[2] = words[5] = 200;
    words[6] = words[7] = words[8] = words[15] = 0;
    words[9] = 4;
    large.Word(1);
    for (auto word : words)
        large.Word(word);
    large.Read(1210);
    large.Word(0x8000);
    large.Verify();
    Dsp4 chip;
    auto bad = Capture(chip);
    bad[96] = bad[97] = bad[98] = 0xff;
    bad[99] = 0x7f;
    Reject(chip, bad);
}

uint32_t Load(uint16_t value, unsigned destination) {
    return 0xc00000 | (uint32_t(value) << 6) | destination;
}

std::vector<uint8_t> Firmware(FirmwareKind kind) {
    std::vector<uint8_t> bytes(CartridgeFirmware::Size(kind));
    const std::array<uint32_t, 8> words{Load(0x400, 7),
                                        Load(0x55, 6),
                                        0x800000 | (0xbe << 13) | (2 << 2),
                                        (9 << 4) | 3,
                                        Load(0xa5, 6),
                                        0x800000 | (0xbe << 13) | (5 << 2),
                                        (9 << 4) | 15,
                                        0x800000 | (0x100 << 13) | (1 << 2)};
    for (unsigned index = 0; index < words.size(); ++index)
        for (unsigned byte = 0; byte < 3; ++byte)
            bytes[index * 3 + byte] = uint8_t(words[index] >> (byte * 8));
    return bytes;
}

void SignalProcessorContinuation() {
    for (auto revision : {UpdDsp::Revision::Upd7725, UpdDsp::Revision::Upd96050}) {
        const auto factory = [revision] {
            auto chip = std::make_unique<UpdDsp>(revision);
            Check(chip->LoadFirmware(Firmware(revision == UpdDsp::Revision::Upd7725
                                                  ? FirmwareKind::Dsp1
                                                  : FirmwareKind::St010)),
                  "DSP firmware loads");
            auto& state = chip->State();
            for (unsigned index = 0; index < 16; ++index)
                state.stack[index] = uint16_t(index * 73);
            state.stackPointer = 15;
            state.accumulator = {0x7fff, 0x8000};
            state.flags = {63, 41};
            state.serialInput = 0x4567;
            state.serialOutput = 0x1234;
            state.temporary = 0x89ab;
            state.temporaryBuffer = 0xcdef;
            state.factorX = -32768;
            state.factorY = 12345;
            state.inputAcknowledge = true;
            state.outputAcknowledge = true;
            for (unsigned index = 0; index < chip->Ram().size(); ++index)
                chip->Ram()[index] = uint16_t(index * 211 + 17);
            return chip;
        };
        Trace<UpdDsp> trace;
        for (unsigned index = 0; index < 64; ++index) {
            trace.Add([](auto& chip) {
                chip.Step();
                return chip.Cycles();
            });
            if (index % 3 == 0)
                trace.Add([](auto& chip) { return chip.ReadStatus(); });
            if (index % 7 == 0)
                trace.Add([](auto& chip) { return chip.ReadData(); });
            if (index % 11 == 0)
                trace.Add([index](auto& chip) {
                    chip.WriteData(uint8_t(index));
                    return 0;
                });
        }
        trace.Verify(factory);
        auto source = factory(), target = factory();
        const auto* ram = target->Ram().data();
        source->State().status = 0x9400;
        Restore(*target, Capture(*source));
        Check(target->State().status == 0x9400 && target->Ram().data() == ram,
              "Raw hidden byte phase and live RAM allocation are retained");
        target->Program()[100] ^= 1;
        Reject(*target, Capture(*source));
        target->Program()[100] ^= 1;
        target->DataRom()[0] ^= 1;
        Reject(*target, Capture(*source));
        auto bad = Capture(*source);
        bad[0] ^= 1;
        Reject(*source, bad);
        Trace<UpdDsp> arithmetic;
        for (unsigned step = 0; step < 48; ++step)
            arithmetic.Add([](auto& chip) {
                chip.Step();
                return chip.Cycles();
            });
        arithmetic.Verify([&] {
            auto chip = factory();
            chip->Program()[0] = Load(0x7fff, 1);
            chip->Program()[1] = Load(1, 3);
            chip->Program()[2] = 0x800000 | (0x140 << 13) | (6 << 2);
            chip->Program()[3] = Load(0x1234, 11);
            chip->Program()[4] = 0x800000 | (0x100 << 13);
            chip->Program()[6] = 0x100000 | (5 << 16) | (3 << 4) | 15;
            chip->Program()[7] = 0x400000 | (1 << 4) | 6;
            return chip;
        });
    }
}

void BridgeContinuation() {
    using Board = NecDsp::Board;
    for (bool pal : {false, true})
        for (auto board : {Board::LoRom, Board::LoRomSmallRam, Board::LoRomLargeRam, Board::HiRom,
                           Board::Seta}) {
            const auto kind = board == Board::Seta ? FirmwareKind::St011 : FirmwareKind::Dsp1;
            const auto factory = [=] {
                auto chip = std::make_unique<NecDsp>(kind, board);
                Check(chip->LoadFirmware(Firmware(kind)), "Bridge firmware loads");
                chip->SetPal(pal);
                return chip;
            };
            const auto data = board == Board::Seta    ? 0x600000u
                              : board == Board::HiRom ? 0x006000u
                                                      : 0x308000u;
            const auto status =
                board == Board::Seta ? data + 1 : data + (board == Board::HiRom ? 0x1000 : 0x4000);
            Trace<NecDsp> trace;
            for (unsigned index = 0; index < 20; ++index) {
                trace.Add([index](auto& chip) {
                    chip.Advance(index % 7);
                    return chip.Cycles();
                });
                trace.Add([status](auto& chip) { return chip.Read(status); });
                trace.Add([data](auto& chip) { return chip.Read(data); });
                trace.Add([index, data](auto& chip) {
                    chip.Write(data, uint8_t(index));
                    return 0;
                });
                if (board == Board::Seta)
                    trace.Add([index](auto& chip) {
                        chip.Write(0x680000 + index, uint8_t(index * 7));
                        return 0;
                    });
            }
            trace.Verify(factory);
            auto source = factory(), target = factory();
            if (board == Board::Seta) {
                source->Write(0x680abc, 0x72);
                target->SaveRam();
                Restore(*target, Capture(*source));
                Check(target->SaveRam()[0xabc] == 0x72,
                      "SETA save view reflects restored physical RAM");
            }
            target->SetPal(!pal);
            Reject(*target, Capture(*source));
        }
}

CompetitionBoard::Images Images() {
    CompetitionBoard::Images images;
    for (unsigned index = 0; index < 4; ++index)
        images[index].assign(32768, uint8_t(0x10 + index));
    return images;
}

void CompetitionContinuation() {
    for (auto board :
         {CompetitionBoard::Board::CampusChallenge92, CompetitionBoard::Board::PowerFest94})
        for (bool pal : {false, true})
            for (bool firmware : {false, true}) {
                const auto factory = [=] {
                    auto chip = std::make_unique<CompetitionBoard>(
                        CompetitionBoard::Settings{board, 1}, Images());
                    if (firmware)
                        Check(chip->LoadFirmware(Firmware(FirmwareKind::Dsp1)),
                              "Competition firmware loads");
                    chip->SetPal(pal);
                    return chip;
                };
                const bool campus = board == CompetitionBoard::Board::CampusChallenge92;
                const uint32_t control = campus ? 0xe00000 : 0x206000,
                               status = campus ? 0xc00000 : 0x106000;
                const uint32_t save = campus ? 0x700123 : 0x306123;
                const uint32_t frequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
                Trace<CompetitionBoard> trace;
                trace.Add([control](auto& chip) {
                    chip.Write(control, 9);
                    return 0;
                });
                trace.Add([save](auto& chip) {
                    chip.Write(save, 0x72);
                    return 0;
                });
                if (!firmware) {
                    trace.Add([frequency](auto& chip) {
                        chip.Advance(frequency * 120);
                        return 0;
                    });
                    trace.Add([frequency](auto& chip) {
                        chip.Advance(frequency * 120 - 1);
                        return 0;
                    });
                }
                for (uint32_t clocks : {0u, 1u, 7u, 19u}) {
                    trace.Add([clocks](auto& chip) {
                        chip.Advance(clocks);
                        return 0;
                    });
                    for (uint32_t address : {status, save, 0x008000u, 0x006000u, 0x007000u})
                        trace.Add([address](auto& chip) { return chip.Read(address, 0xa5); });
                }
                if (!firmware) {
                    trace.Add([frequency](auto& chip) {
                        chip.Advance(frequency * 5 - 27);
                        return 0;
                    });
                    trace.Add([status](auto& chip) { return chip.Read(status, 0xa5); });
                    trace.Add([](auto& chip) {
                        chip.Advance(1);
                        return 0;
                    });
                    trace.Add([status](auto& chip) { return chip.Read(status, 0xa5); });
                }
                trace.Add([control, campus](auto& chip) {
                    chip.Write(control, campus ? 5 : 12);
                    return 0;
                });
                trace.Add([](auto& chip) { return chip.Read(0x008000, 0xa5); });
                trace.Reset();
                trace.Add([save](auto& chip) { return chip.Read(save, 0xa5); });
                trace.Verify(factory);
                auto source = factory(), target = factory();
                const auto* ram = target->SaveRam().data();
                Restore(*target, Capture(*source));
                Check(target->SaveRam().data() == ram,
                      "Competition restore preserves mapped SRAM storage");
                auto changed = Images();
                changed[2][17] ^= 1;
                CompetitionBoard wrong({board, 1}, std::move(changed));
                wrong.SetPal(pal);
                if (firmware)
                    Check(wrong.LoadFirmware(Firmware(FirmwareKind::Dsp1)),
                          "Identity fixture loads");
                Reject(wrong, Capture(*source));
                target->SetPal(!pal);
                Reject(*target, Capture(*source));
            }
}

std::vector<uint8_t> PortableFixture() {
    StateArchive archive;
    Dsp1 geometry;
    geometry.SetDr(0);
    geometry.SetDr(0x34);
    geometry.SetDr(0x12);
    geometry.SetDr(0x78);
    geometry.GetSr();
    geometry.SerializeState(archive);
    Dsp2 bitmap;
    bitmap.Write(5);
    bitmap.Write(17);
    bitmap.Write(0x9a);
    bitmap.SerializeState(archive);
    Dsp3 map;
    map.Write(0x8000, 0x18);
    map.Write(0x8000, 2);
    map.Write(0x8000, 0);
    map.Write(0x8000, 0x35);
    map.SerializeState(archive);
    Dsp4 projection;
    projection.Write(0x8000, 1);
    projection.Write(0x8000, 0);
    for (auto word : Road(false, false, false)) {
        projection.Write(0x8000, uint8_t(word));
        projection.Write(0x8000, uint8_t(unsigned(word) >> 8));
    }
    projection.Read(0x8000);
    projection.SerializeState(archive);
    auto processor = std::make_unique<UpdDsp>();
    Check(processor->LoadFirmware(Firmware(FirmwareKind::Dsp1)), "Portable firmware loads");
    for (unsigned step = 0; step < 11; ++step)
        processor->Step();
    processor->ReadData();
    processor->SerializeState(archive);
    auto bridge = std::make_unique<NecDsp>(FirmwareKind::St010, NecDsp::Board::Seta);
    Check(bridge->LoadFirmware(Firmware(FirmwareKind::St010)), "Portable bridge loads");
    bridge->SetPal(true);
    bridge->Advance(13);
    bridge->Write(0x680abc, 0x72);
    bridge->SerializeState(archive);
    CompetitionBoard competition({CompetitionBoard::Board::PowerFest94, 1}, Images());
    competition.Write(0x206000, 9);
    competition.Advance(23);
    competition.Write(0x306123, 0x57);
    competition.SerializeState(archive);
    return archive.Bytes();
}
} // namespace

int main() {
    try {
        FrozenRegisterRead();
        GeometryContinuation();
        BitmapContinuation();
        MapContinuation();
        ProjectionContinuation();
        SignalProcessorContinuation();
        BridgeContinuation();
        CompetitionContinuation();
        Check(PortableFixture() == PortableFixture(), "Portable DSP fixture is deterministic");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("Command DSP, signal processor and competition state continuation passed");
    return 0;
}
