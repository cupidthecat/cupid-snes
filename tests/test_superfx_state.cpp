#include "cartridge/SuperFxStateFixture.hpp"
#include "snes/core/FilePath.hpp"

#include <cstdio>
#include <fstream>
#include <stdexcept>

using snes::core::detail::StateArchive;
using snes::tests::SuperFxStateFixture;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<uint8_t> Capture(SuperFxStateFixture& fixture) {
    StateArchive archive;
    fixture.SerializeState(archive);
    return archive.Bytes();
}

void Restore(SuperFxStateFixture& fixture, std::span<const uint8_t> bytes) {
    StateArchive archive(bytes);
    fixture.SerializeState(archive);
    archive.Commit();
}

void Reject(SuperFxStateFixture& fixture, std::span<const uint8_t> bytes) {
    const auto before = Capture(fixture);
    bool rejected = false;
    try {
        Restore(fixture, bytes);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Check(rejected, "Invalid Super FX state must reject");
    Check(Capture(fixture) == before, "Rejected Super FX state retains execution and shared RAM");
}

uint64_t Cuts() {
    uint64_t count = 0;
    for (unsigned profile = 0; profile < 8; ++profile)
        for (unsigned scenario = 0; scenario < 18; ++scenario) {
            SuperFxStateFixture first(profile, scenario), second(profile, scenario);
            const unsigned end = scenario >= 10 && scenario <= 12 ? 512 : 96;
            for (unsigned cut = 0; cut < end; ++cut) {
                first.Drive(cut);
                const auto pc = first.Reg(15);
                const auto irq = first.chip.CpuIrqPending();
                auto* const mapped = first.ram.data();
                const auto saved = Capture(first);
                Check(first.Reg(15) == pc && first.chip.CpuIrqPending() == irq &&
                          Capture(first) == saved,
                      "Capture preserves the exact pending instruction and IRQ without advancing "
                      "clocks");
                second.Reg(0, uint16_t(cut * 7));
                second.Advance(31);
                auto* const target = second.ram.data();
                Restore(second, saved);
                Check(Capture(second) == saved && second.ram.data() == target,
                      "Restore replaces all internal state and retains mapped storage addresses");
                unsigned tail = 0;
                for (unsigned clocks : {0u, 1u, 2u, 7u, 23u, 107u}) {
                    first.Edit(cut, tail);
                    second.Edit(cut, tail++);
                    first.Advance(clocks);
                    second.Advance(clocks);
                    Check(Capture(first) == Capture(second),
                          "Restored fetch, memory and pixel phases continue through the same clock "
                          "slices");
                }
                Restore(first, saved);
                Check(first.ram.data() == mapped,
                      "Restoring shared RAM preserves its live mapping");
                first.Advance(1);
                ++count;
            }
        }
    return count;
}

uint64_t Instructions() {
    uint64_t count = 0;
    for (unsigned profile : {0u, 2u})
        for (unsigned mode = 0; mode < 4; ++mode)
            for (unsigned opcode = 0; opcode < 256; ++opcode) {
                SuperFxStateFixture first(profile, 0), second(profile, 0);
                first.Instruction(opcode, mode);
                second.Instruction(opcode, mode);
                for (unsigned cut = 0; cut < 24; ++cut) {
                    const auto saved = Capture(first);
                    Restore(second, saved);
                    first.Edit(cut, 3);
                    second.Edit(cut, 3);
                    first.Advance(17);
                    second.Advance(17);
                    Check(Capture(first) == Capture(second),
                          "Every opcode and ALT mode retains its instruction operands across "
                          "restore");
                    Restore(first, saved);
                    first.Advance(1);
                    ++count;
                }
            }
    return count;
}

uint32_t Word(std::span<const uint8_t> bytes, size_t offset) {
    uint32_t value = 0;
    for (unsigned n = 0; n < 4; ++n)
        value |= uint32_t(bytes[offset + n]) << (8 * n);
    return value;
}

void Put(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, unsigned size) {
    for (unsigned n = 0; n < size; ++n)
        bytes.at(offset + n) = uint8_t(value >> (8 * n));
}

// Offsets describe the canonical wire fields, independent of native structure padding.
constexpr size_t WorkOffset = 653, StepCountOffset = 720, StepOffset = 724;

void InvalidFields() {
    struct Mutation {
        size_t offset;
        uint64_t value;
        unsigned size;
    };

    constexpr std::array mutations{
        Mutation{590, 1, 2},        // Reserved status bit.
        Mutation{592, 1, 2},        // Unaligned cache base.
        Mutation{596, 128, 1},      // Program bank.
        Mutation{598, 2, 1},        // RAM bank.
        Mutation{599, 1, 1},        // Reserved configuration bit.
        Mutation{601, 64, 1},       // Screen mode.
        Mutation{602, 2, 1},        // Clock select.
        Mutation{607, 16, 4},       // Source register index.
        Mutation{611, 16, 4},       // Destination register index.
        Mutation{615, 2, 1},        // Noncanonical boolean.
        Mutation{618, 7, 4},        // ROM countdown.
        Mutation{622, 0x800000, 4}, // ROM address.
        Mutation{627, 7, 4},        // RAM countdown.
        Mutation{631, 0x20000, 4},  // RAM address.
        Mutation{636, 1, 4},        // Unreturned clock.
        Mutation{640, uint64_t(-1), 8},
        Mutation{648, 3, 1}, // Memory port enumeration.
        Mutation{WorkOffset, 255, 1},
        Mutation{WorkOffset, 4, 1},  // Synchronous execute phase.
        Mutation{WorkOffset, 16, 1}, // Synchronous pixel update.
        Mutation{655, 4, 4},         // Latched ALT mode.
        Mutation{667, 2, 1},         // Branch condition boolean.
        Mutation{669, 1, 4},         // Latched RAM bank alignment.
        Mutation{673, 0x50000, 4},   // Pending instruction address.
        Mutation{682, 3, 4},         // Pixel depth.
        Mutation{686, 9, 4},         // Pixel plane.
        Mutation{694, 128, 1},       // Fetch bank.
        Mutation{695, 2, 1},         // Fetch boolean.
        Mutation{697, 17, 4},        // Fill position.
        Mutation{701, 3, 1},         // Fetch phase.
        Mutation{702, 2, 4},         // Flush row index.
        Mutation{710, 3, 4},         // Flush depth.
        Mutation{714, 9, 4},         // Flush plane.
        Mutation{719, 2, 1},         // Partial-plane read boolean.
        Mutation{StepCountOffset, 0xffffffff, 4},
    };
    for (unsigned scenario : {0u, 2u, 4u, 10u, 14u, 17u}) {
        SuperFxStateFixture fixture(0, scenario);
        for (unsigned tick = 0; tick < 45; ++tick) {
            fixture.Drive(tick);
            fixture.Advance(1);
        }
        const auto saved = Capture(fixture);
        Check(saved.size() == StepOffset + Word(saved, StepCountOffset) * 10 + fixture.ram.size(),
              "Super FX state uses canonical fixed-width clock-step records");
        for (const auto& mutation : mutations) {
            auto bad = saved;
            Put(bad, mutation.offset, mutation.value, mutation.size);
            Reject(fixture, bad);
        }
        auto bad = saved;
        bad[649] ^= 1;
        Reject(fixture, bad);
        bad = saved;
        bad[650] ^= 1;
        Reject(fixture, bad);
        if (Word(saved, StepCountOffset)) {
            for (unsigned byte : {unsigned(StepOffset), unsigned(StepOffset + 4)}) {
                bad = saved;
                bad[byte] = byte == StepOffset ? 0 : 255;
                Reject(fixture, bad);
            }
            bad = saved;
            bad[StepOffset + (Word(saved, StepCountOffset) - 1) * 10 + 4] = 1;
            Reject(fixture, bad);
        }
    }
}

void NestedMemoryAndBindings() {
    SuperFxStateFixture first(0, 17), second(0, 17);
    for (unsigned tick = 0; tick < 300; ++tick) {
        first.Drive(tick);
        first.Advance(1);
    }
    const auto saved = Capture(first);
    Check(Word(saved, StepCountOffset) > 16,
          "Repeated CPU buffer updates produce a long interrupted memory-step stack");
    auto bad = saved;
    bad[StepOffset + 4] = 4;
    Reject(first, bad);
    const auto rom = second.rom;
    auto* const memory = second.ram.data();
    Restore(second, saved);
    first.chip.SetPal(true);
    second.chip.SetPal(true);
    Restore(second, Capture(first));
    first.chip.WriteCpu(0x303a, 0x18);
    second.chip.WriteCpu(0x303a, 0x18);
    first.Advance(1000);
    second.Advance(1000);
    Check(Capture(first) == Capture(second) && second.rom == rom && second.ram.data() == memory,
          "Long memory-step chains resume in order and retain target ROM and RAM bindings");
    second.rom[0x1234] = 0x6a;
    second.chip.WriteCpu(0x3030, 0);
    Check(second.chip.ReadCpu(0x009234) == 0x6a,
          "Restored CPU mapping still reads the target cartridge allocation");
}

void StopIrqAndReset() {
    SuperFxStateFixture first(0, 0), second(0, 0);
    for (unsigned n = 0; n < 16; ++n)
        first.chip.WriteCpu(0x3100 + n, n == 0 ? 0 : 1);
    first.Reg(15, 0);
    first.Advance(10);
    Check(first.chip.CpuIrqPending(), "STOP publishes the interrupt before a stopped checkpoint");
    first.chip.WriteCpu(0x3031, 0);
    Restore(second, Capture(first));
    Check(second.chip.CpuIrqPending() && !(second.chip.ReadCpu(0x3031) & 0x80) &&
              !second.chip.CpuIrqPending(),
          "Independent IRQ output survives a cleared writable status bit and acknowledge");
    first.chip.SetPal(true);
    second.chip.SetPal(true);
    const auto saved = Capture(first);
    first.chip.Reset();
    second.chip.Reset();
    Check(!first.chip.CpuIrqPending(), "Reset clears the saved processor interrupt");
    Restore(first, saved);
    Restore(second, saved);
    Check(Capture(first) == Capture(second), "Reset retains the regional oscillator for restore");
}

void StorageSizes() {
    for (size_t rom : {size_t{0}, size_t{1}, size_t{0x8000}, size_t{0x100000}})
        for (size_t ram : {size_t{0}, size_t{1}, size_t{32769}, size_t{0x20000}}) {
            SuperFxStateFixture first(1, 0, rom, ram), second(1, 0, rom, ram);
            first.Advance(13);
            const auto saved = Capture(first);
            Restore(second, saved);
            first.Advance(17);
            second.Advance(17);
            Check(Capture(first) == Capture(second),
                  "Empty, mirrored and full cartridge storage retains portable processor state");
        }
}

void AtomicRejection() {
    for (unsigned scenario : {0u, 2u, 4u, 10u, 14u, 17u}) {
        SuperFxStateFixture fixture(0, scenario), control(0, scenario);
        for (unsigned tick = 0; tick < 45; ++tick) {
            fixture.Drive(tick);
            control.Drive(tick);
            fixture.Advance(1);
            control.Advance(1);
        }
        const auto saved = Capture(fixture);
        for (size_t length = 0; length < saved.size(); length += saved.size() / 19)
            Reject(fixture, std::span(saved).first(length));
        Reject(fixture, std::span(saved).first(saved.size() - 1));
        auto bad = saved;
        bad.push_back(0);
        Reject(fixture, bad);
        for (unsigned byte : {0u, 8u, 16u}) {
            bad = saved;
            bad[byte] ^= 1;
            Reject(fixture, bad);
        }
        fixture.chip.WriteCpu(0x303a, 0x18);
        control.chip.WriteCpu(0x303a, 0x18);
        fixture.Advance(71);
        control.Advance(71);
        Check(Capture(fixture) == Capture(control),
              "Late rejection preserves the original pending execution for future clocks");
    }
}

void Portable(const char* path) {
    std::vector<uint8_t> bytes;
    for (unsigned profile = 0; profile < 8; ++profile)
        for (unsigned scenario : {0u, 2u, 4u, 8u, 10u, 12u, 14u, 17u}) {
            SuperFxStateFixture fixture(profile, scenario);
            for (unsigned tick = 0; tick < 83; ++tick) {
                fixture.Drive(tick);
                fixture.Advance(1);
            }
            const auto saved = Capture(fixture);
            bytes.insert(bytes.end(), saved.begin(), saved.end());
        }
    std::ofstream output(snes::core::FilePath(path), std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    Check(bool(output), "Write portable Super FX fixture");
    std::printf("Portable Super FX fixture: %zu bytes\n", bytes.size());
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto cuts = Cuts() + Instructions();
        AtomicRejection();
        InvalidFields();
        NestedMemoryAndBindings();
        StopIrqAndReset();
        StorageSizes();
        if (argc == 2)
            Portable(argv[1]);
        std::printf("Super FX portable continuation: %llu cuts passed\n", (unsigned long long)cuts);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
