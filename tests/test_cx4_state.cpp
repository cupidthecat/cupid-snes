#include "cartridge/Cx4TimingTrace.hpp"
#include "snes/core/Cx4.hpp"
#include "snes/core/Hg51b.hpp"
#include "snes/core/detail/ExecutionContext.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
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
    Check(rejected, "Invalid Cx4 state must reject");
    Check(Capture(device) == before,
          "Rejected Cx4 state retains suspended execution and mapped memory");
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

struct Clocked : Hg51b {
    std::array<uint8_t, 4096> memory{};
    uint64_t clocks = 0, reads = 0, writes = 0;
    int64_t balance = 0;
    std::unique_ptr<detail::ExecutionContext> worker;

    bool IsRom(uint32_t address) const override {
        return address < 0x800000;
    }

    bool IsRam(uint32_t address) const override {
        return address >= 0x800000;
    }

    uint8_t ReadExternal(uint32_t address) override {
        ++reads;
        return memory[address & 4095];
    }

    void WriteExternal(uint32_t address, uint8_t value) override {
        ++writes;
        memory[address & 4095] = value;
    }

    void StepClocks(unsigned elapsed) override {
        clocks += elapsed;
        balance += elapsed;
        if (balance >= 0)
            worker->Suspend();
    }

    void Advance(unsigned elapsed) {
        balance -= elapsed;
        if (balance >= 0)
            return;
        if (!worker)
            worker = std::make_unique<detail::ExecutionContext>([this] {
                for (;;)
                    RunStep();
            });
        worker->Resume();
    }

    void SerializeState(StateArchive& archive) {
        archive.BeforeRestore([this]() noexcept { worker.reset(); });
        Hg51b::SerializeState(archive);
        archive.Array(memory);
        archive.Number(clocks);
        archive.Number(reads);
        archive.Number(writes);
        archive.Number(balance);
    }
};

void Prepare(Clocked& chip, unsigned scenario) {
    for (unsigned i = 0; i < chip.memory.size(); ++i)
        chip.memory[i] = uint8_t(i * 37 + 11);
    auto state = chip.GetState();
    state.dataRom[3] = 0xaabbcc;
    state.stack[0] = 0x12345;
    state.control.halted = false;
    state.program[0].fill(0);
    if (scenario == 0)
        state.control.cache.base = 0x8000;
    if (scenario == 1) {
        state.control.halted = true;
        state.control.dma = {true, 0x100, 0x800080, 33};
    }
    if (scenario == 2) {
        state.control.bus = {true, true, true, 7, 0x800012};
        state.program[0][0] = 0x1c00;
        state.program[0][1] = 0x0800;
    }
    if (scenario == 3) {
        state.program[0][0] = 0x2803;
        state.program[0][3] = 0x3c00;
    }
    if (scenario == 4) {
        state.control.suspended = true;
        state.control.suspendClocks = 19;
    }
    if (scenario == 5 || scenario == 6) {
        state.registers.counter = 253;
        state.registers.page = 1;
        state.program[0][253] = 0x7c01;
        state.program[0][254] = scenario == 6 ? 0x2400 : 0x6401;
        state.program[0][255] = 0x642a;
    }
    chip.SetState(state);
}

void CoreCuts() {
    for (unsigned scenario = 0; scenario < 7; ++scenario)
        for (unsigned cut = 0; cut < 2400; cut += scenario == 0 || scenario >= 5 ? 7 : 3) {
            Clocked first, second;
            Prepare(first, scenario);
            Prepare(second, scenario);
            first.Advance(cut);
            const auto clocks = first.clocks, reads = first.reads, writes = first.writes;
            auto saved = Capture(first);
            Check(first.clocks == clocks && first.reads == reads && first.writes == writes,
                  "Capture never advances a clock or accesses the graphics processor bus");
            second.Advance(cut + 17);
            Restore(second, saved);
            for (unsigned elapsed : {0u, 1u, 2u, 7u, 31u}) {
                first.WriteHost(0x7f50, uint8_t(elapsed * 13));
                second.WriteHost(0x7f50, uint8_t(elapsed * 13));
                first.Advance(elapsed);
                second.Advance(elapsed);
                Check(Capture(first) == Capture(second),
                      "Cache, DMA, opcode and bus phases retain exact clock returns");
            }
        }
    Clocked chip;
    Prepare(chip, 2);
    chip.Advance(3);
    Truncation(chip);
    auto bad = Capture(chip);
    bad[0] ^= 1;
    Reject(chip, bad);
}

void LiveRegisterReplacement() {
    Clocked first, second;
    Prepare(first, 1);
    Prepare(second, 1);
    first.Advance(3);
    const auto value = first.memory[0x100];
    auto state = first.GetState();
    state.control.dma.source = 0x200;
    state.control.dma.target = 0x8000c0;
    state.control.dma.length = 1;
    first.SetState(state);
    Restore(second, Capture(first));
    first.Advance(400);
    second.Advance(400);
    Check(first.writes == 33 && first.memory[0x80] == value &&
              first.memory[0xc1] == first.memory[0x201],
          "Register replacement retains latched DMA length/byte addresses and uses live registers "
          "for later bytes");
    Check(Capture(first) == Capture(second),
          "Latched DMA work survives both register replacement and checkpoint restore");
}

struct Board {
    std::vector<uint8_t> save;
    Cx4Processor chip;

    Board(snes::tests::Cx4Fixture& fixture, unsigned profile)
        : save(fixture.ram),
          chip(fixture.rom, save, fixture.firmware,
               profile & 2 ? Cx4Processor::Layout::HiRom : Cx4Processor::Layout::LoRom) {
        chip.InitializeStorage(fixture.storage);
        chip.SetPal(profile & 1);
        chip.SetDelayedSync(profile & 4);
    }

    void SerializeState(StateArchive& archive) {
        archive.Fixed(std::span<uint8_t>(save));
        chip.SerializeState(archive);
    }
};

void Setup(Board& board, unsigned profile, unsigned scenario) {
    const uint32_t ram = profile & 2 ? 0x306000 : 0x700000;
    snes::tests::Cx4Address(board.chip, 0x7f83, ram);
    if (scenario == 0) {
        snes::tests::Cx4Address(board.chip, 0x7f49, 0x008000);
        board.chip.Write(0x7f48, 0);
    } else if (scenario == 1) {
        snes::tests::Cx4Address(board.chip, 0x7f40, 0x008000);
        snes::tests::Cx4Address(board.chip, 0x7f43, 33, 2);
        snes::tests::Cx4Address(board.chip, 0x7f45, ram);
    } else if (scenario == 2) {
        board.chip.Write(0x7f4f, 0);
    } else if (scenario == 3) {
        board.chip.Write(0x7f58, 0);
    } else if (scenario == 5) {
        snes::tests::Cx4ZeroWaitSetup(board.chip);
    } else {
        board.chip.Advance(1, true);
    }
}

void Observe(Board& first, Board& second) {
    Check(Capture(first) == Capture(second),
          "Board state retains regional clocks, IRQ and complete processor storage");
    for (uint32_t address : {0x7f53u, 0x7f50u, 0x7f80u, 0x7f81u, 0x7f86u, 0x6000u, 0x700011u})
        Check(first.chip.Read(address, 0xa5) == second.chip.Read(address, 0xa5),
              "Restored host ports and IRQ acknowledgements agree");
    Check(first.chip.CpuIrqPending() == second.chip.CpuIrqPending(),
          "Restored IRQ line remains independent of status reads");
}

void BoardCuts() {
    snes::tests::Cx4Fixture fixture;
    for (unsigned profile = 0; profile < 8; ++profile)
        for (unsigned scenario = 0; scenario < 6; ++scenario) {
            Board target(fixture, profile);
            for (unsigned cut = 0; cut < 2300; cut += scenario == 0 ? 7 : 19) {
                Board source(fixture, profile);
                Setup(source, profile, scenario);
                source.chip.Advance(cut, scenario != 4);
                const auto* allocation = target.save.data();
                Restore(target, Capture(source));
                Check(target.save.data() == allocation,
                      "Board restore retains mapped SRAM allocations");
                for (uint32_t clocks : {0u, 1u, 2u, 11u, 37u}) {
                    source.chip.Write(0x7f81, uint8_t(clocks + 0x35));
                    target.chip.Write(0x7f81, uint8_t(clocks + 0x35));
                    source.chip.Advance(clocks, clocks & 1);
                    target.chip.Advance(clocks, clocks & 1);
                    Observe(source, target);
                }
            }
            Truncation(target);
        }
}

void Malformed() {
    snes::tests::Cx4Fixture fixture;
    Board source(fixture, 0), target(fixture, 0);
    Setup(source, 0, 0);
    source.chip.Advance(11, true);
    constexpr size_t phase = 8192 + 25 + 4341;
    auto bad = Capture(source);
    Check(bad[phase] == 4 || bad[phase] == 5, "Malformed fixture reaches a partial cache fill");
    bad[phase] = 255;
    Reject(target, bad);
    for (uint8_t transient : {6, 9, 10, 12}) {
        bad = Capture(source);
        bad[phase] = transient;
        Reject(target, bad);
    }
    bad = Capture(source);
    bad[phase + 1] = 255;
    Reject(target, bad);
    bad = Capture(source);
    bad[phase + 6] = 0xff;
    bad[phase + 7] = 0xff;
    Reject(target, bad);
    target.chip.SetPal(true);
    Reject(target, Capture(source));
    target.chip.SetPal(false);
    target.chip.SetDelayedSync(true);
    Reject(target, Capture(source));
    target.chip.SetDelayedSync(false);
    auto changed = fixture.firmware;
    changed[4] ^= 1;
    Cx4Processor other(fixture.rom, fixture.ram, changed);
    Reject(other, Capture(source.chip));
    Restore(target, Capture(source));
    bad = Capture(source);
    bad[phase] = 255;
    Reject(target, bad);
    source.chip.Advance(100, true);
    target.chip.Advance(100, true);
    Observe(source, target);
}

void BoundaryCuts() {
    for (bool skip : {false, true}) {
        snes::tests::Cx4Fixture fixture;
        fixture.rom = snes::tests::Cx4BoundaryRom(skip);
        for (unsigned profile = 0; profile < 8; ++profile) {
            Board target(fixture, profile);
            for (unsigned cut = 0; cut < 2300; cut += 7) {
                Board source(fixture, profile);
                snes::tests::Cx4BoundarySetup(source.chip);
                source.chip.Advance(cut, true);
                Restore(target, Capture(source));
                for (uint32_t clocks : {0u, 1u, 3u, 31u}) {
                    source.chip.Advance(clocks, true);
                    target.chip.Advance(clocks, true);
                    Observe(source, target);
                }
            }
            Board source(fixture, profile);
            snes::tests::Cx4BoundarySetup(source.chip);
            source.chip.Advance(3000, true);
            Check(source.chip.Read(0x7f80, 0xa5) == (skip ? 0 : 0x2a),
                  "Counter wrap preserves the old opcode or skips it after refilling the next "
                  "program "
                  "bank");
        }
    }
}

void CommandMemory() {
    std::vector<uint8_t> rom(32768);
    Cx4 first(rom), second(rom);
    for (unsigned address = 0; address < 8192; ++address)
        if (address != 0x1f4f)
            first.Write(uint32_t(0x6000 + address), uint8_t(address * 31));
    Restore(second, Capture(first));
    Check(Capture(first) == Capture(second),
          "Command fallback retains hidden and directly readable work RAM");
    for (uint32_t address = 0x6000; address < 0x8000; ++address)
        Check(first.Read(address) == second.Read(address),
              "Command fallback retains every byte of work RAM");
    Truncation(second);
}

void ClockSafety() {
    snes::tests::Cx4Fixture fixture;
    Board board(fixture, 0);
    board.chip.Advance(1, true);
    auto bytes = Capture(board);
    const auto offset = bytes.size() - 9;
    std::fill_n(bytes.begin() + offset, 8, 0);
    bytes[offset + 7] = 0x80;
    Restore(board, bytes);
    const auto before = Capture(board);
    bool rejected = false;
    try {
        board.chip.Advance(1, false);
    } catch (const std::overflow_error&) {
        rejected = true;
    }
    Check(rejected,
          "An unrepresentable Cx4 elapsed interval must reject before changing its clock");
    Check(Capture(board) == before, "Rejected clock arithmetic retains all pending hardware");
    board.chip.Advance(0, true);
    bytes = Capture(board);
    uint64_t balance = 0;
    for (unsigned byte = 0; byte < 8; ++byte)
        balance |= uint64_t(bytes[offset + byte]) << (byte * 8);
    const uint64_t frequency = 21'477'272, debt = uint64_t{1} << 63;
    Check(balance == (frequency - debt % frequency) % frequency,
          "Idle advancement preserves the exact edge for every representable deferred clock debt");
}

std::vector<uint8_t> PortableFixture() {
    StateArchive archive;
    snes::tests::Cx4Fixture fixture;
    for (unsigned profile = 0; profile < 8; ++profile)
        for (unsigned scenario = 0; scenario < 6; ++scenario) {
            Board board(fixture, profile);
            Setup(board, profile, scenario);
            for (uint32_t clocks : {0u, 1u, 7u, 31u, 127u, 511u}) {
                board.chip.Advance(clocks, true);
                board.SerializeState(archive);
            }
        }
    return archive.Bytes();
}
} // namespace

int main() {
    try {
        ClockSafety();
        LiveRegisterReplacement();
        CoreCuts();
        BoardCuts();
        BoundaryCuts();
        Malformed();
        CommandMemory();
        Check(PortableFixture() == PortableFixture(), "Portable Cx4 fixture is deterministic");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("HG51B clock-return, Cx4 board and command-memory continuation checks passed");
    return 0;
}
