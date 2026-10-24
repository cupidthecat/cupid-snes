#include "ConsoleConfig.hpp"
#include "snes/core/debug/MemoryInspection.hpp"
#include "state/EmulatorBoards.hpp"

#include <cstdio>
#include <set>
#include <stdexcept>

using namespace snes::core;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class Function> void Reject(Function run) {
    bool rejected = false;
    try {
        run();
    } catch (const std::exception&) {
        rejected = true;
    }
    Check(rejected, "Invalid inspection range rejects");
}

struct Input final : IInputProvider {
    unsigned polls = 0, fields = 0;

    InputState Poll(uint64_t) override {
        ++polls;
        return {.a = true};
    }

    void AdvanceFrame() noexcept override {
        ++fields;
    }
};

void Boards() {
    using namespace snes::tests::emulator_state;
    for (unsigned n = 0; n < unsigned(Board::Count); ++n) {
        const auto board = Board(n);
        for (const auto region : {Region::NTSC, Region::PAL}) {
            auto config = snes::tests::DeterministicConsoleConfig();
            config.regionOverride = region;
            config.cartridgeFirmware = Firmware(board);
            auto machine = std::make_unique<Emulator>(config);
            Input input;
            machine->AttachInputProvider(&input);
            Load(*machine, board);
            machine->StepFrame();
            const auto before = machine->CaptureState();
            const auto clock = machine->CurrentMasterCycles(), frame = machine->CurrentFrame();
            const auto polls = input.polls, fields = input.fields;
            std::set<std::string> keys;
            const auto regions = MemoryInspector::Regions(*machine);
            Check(regions.size() >= 7, "All machines expose console storage and cartridge ROM");
            for (const auto& memory : regions) {
                Check(keys.insert(memory.key).second && !memory.label.empty() && memory.size,
                      "Region identifiers are unique and memory sizes are nonzero");
                const auto bytes = MemoryInspector::Read(*machine, memory.key, 0, memory.size);
                Check(bytes.size() == memory.size, "A physical region can be read in full");
                const auto last = MemoryInspector::Read(*machine, memory.key, memory.size - 1, 1);
                Check(last.front() == bytes.back(), "Partial reads retain physical byte ordering");
                Check(MemoryInspector::Read(*machine, memory.key, memory.size, 0).empty(),
                      "An empty read at the end is valid");
                Reject([&] { MemoryInspector::Read(*machine, memory.key, memory.size, 1); });
                Reject([&] { MemoryInspector::Read(*machine, memory.key, 1, SIZE_MAX); });
            }
            Reject([&] { MemoryInspector::Read(*machine, "missing", 0, 1); });
            size_t available = 0;
            for (uint32_t bank = 0; bank < 256; ++bank)
                for (uint32_t offset : {0u, 0x1234u, 0x2180u, 0x3000u, 0x4016u, 0x4210u, 0x4800u,
                                        0x6000u, 0x7fffu, 0x8000u, 0xfffcu, 0xffffu})
                    available +=
                        MemoryInspector::PeekCpu(*machine, (bank << 16) | offset).has_value();
            Check(available >= 128, "Every machine exposes mapped work RAM without bus reads");
            for (uint32_t port : {0x2138u, 0x2139u, 0x213bu, 0x2180u, 0x4016u, 0x4210u})
                Check(!MemoryInspector::PeekCpu(*machine, port),
                      "Console I/O ports remain unavailable to mapped peeks");
            Check(machine->CurrentMasterCycles() == clock && machine->CurrentFrame() == frame &&
                      input.polls == polls && input.fields == fields,
                  "Inspection never advances clocks, fields or host input");
            Check(machine->CaptureState() == before,
                  "Inspection retains every serialized machine byte");
            for (uint32_t bank = 0; bank < 256; ++bank)
                for (uint32_t offset :
                     {0u, 0x1234u, 0x3000u, 0x4800u, 0x6000u, 0x7fffu, 0x8000u, 0xfffcu, 0xffffu}) {
                    const auto address = (bank << 16) | offset;
                    if (const auto value = MemoryInspector::PeekCpu(*machine, address)) {
                        const auto expected = machine->GetBus().ReadCpu(address);
                        if (*value != expected) {
                            std::fprintf(
                                stderr,
                                "Mapped byte mismatch board=%u address=%06X peek=%02X bus=%02X\n",
                                n, unsigned(address), unsigned(*value), unsigned(expected));
                            throw std::runtime_error(
                                "Available peeks match the mapped bus decoder");
                        }
                    }
                }
            if (board == Board::Sa1 || board == Board::SlotSa1)
                Check(keys.contains("sa1-iram"), "SA-1 internal RAM is exposed");
            if (board == Board::SuperFx)
                Check(keys.contains("superfx-cache"),
                      "Graphics cache is exposed without CPU register reads");
            if (board == Board::Handheld || board == Board::Handheld2)
                for (const auto* key :
                     {"handheld-rom", "handheld-save", "handheld-wram", "handheld-hram",
                      "handheld-vram", "handheld-oam", "handheld-wave"})
                    Check(keys.contains(key), "Handheld storage retains distinct physical regions");
        }
    }
}

void Bytes() {
    using namespace snes::tests::emulator_state;
    for (uint32_t vramSize : {65536u, 131072u}) {
        auto config = snes::tests::DeterministicConsoleConfig();
        config.ppu.vramBytes = vramSize;
        auto machine = std::make_unique<Emulator>(config);
        Load(*machine, Board::Lo);
        machine->GetPpu().VramData()[0] = 0x1234;
        machine->GetPpu().VramData()[vramSize / 2 - 1] = 0xabcd;
        const auto bytes = MemoryInspector::Read(*machine, "vram", 0, vramSize);
        Check(bytes[0] == 0x34 && bytes[1] == 0x12 && bytes[vramSize - 2] == 0xcd &&
                  bytes.back() == 0xab,
              "Both VRAM sizes use little-endian physical bytes including the final word");
        machine->GetPpu().CgramData()[1] = 0x3b4c;
        Check(MemoryInspector::Read(*machine, "cgram", 2, 2) == std::vector<uint8_t>({0x4c, 0x3b}),
              "Color memory is independent of host word byte order");
        machine->GetBus().WramData()[0x1234] = 0x5a;
        Check(MemoryInspector::Read(*machine, "wram", 0x1234, 1).front() == 0x5a,
              "Work RAM uses physical offsets rather than serial data ports");
        for (uint32_t address : {0x001234u, 0x801234u, 0x7e1234u})
            Check(MemoryInspector::PeekCpu(*machine, address) == 0x5a,
                  "Mapped peeks respect work RAM mirrors");
        const auto original = MemoryInspector::PeekCpu(*machine, 0x008000);
        Check(original && original == machine->GetBus().ReadCpu(0x808000),
              "Mapped ROM peeks follow cartridge aliases");
        unsigned reads = 0;
        machine->GetBus().Map(
            0, 0, 0x8000, 0x8000,
            [&](uint32_t, uint8_t) {
                ++reads;
                return uint8_t(0xab);
            },
            [](uint32_t, uint8_t) {});
        Check(!MemoryInspector::PeekCpu(*machine, 0x008000) && reads == 0,
              "Unknown mapped handlers are unavailable and are never invoked by peeking");
        Reject([&] { MemoryInspector::PeekCpu(*machine, 0x1000000); });
    }
    auto machine = std::make_unique<Emulator>();
    Check(MemoryInspector::Regions(*machine).empty(), "An idle machine has no inspectable regions");
    Check(!MemoryInspector::PeekCpu(*machine, 0x8000), "An idle machine has no mapped CPU bytes");
    Reject([&] { MemoryInspector::Read(*machine, "wram", 0, 1); });
}

void ConsumingRead() {
    using namespace snes::tests::emulator_state;
    auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    Load(*machine, Board::Sdd1);
    auto& bus = machine->GetBus();
    bus.Write(0x4302, 0);
    bus.Write(0x4303, 0);
    bus.Write(0x4304, 0xc0);
    bus.Write(0x4305, 8);
    bus.Write(0x4306, 0);
    bus.Write(0x4800, 1);
    bus.Write(0x4801, 1);
    const auto before = machine->CaptureState();
    for (unsigned repeat = 0; repeat < 4; ++repeat)
        Check(!MemoryInspector::PeekCpu(*machine, 0xc00000),
              "An active decompression stream cannot be peeked as ROM");
    Check(machine->CaptureState() == before,
          "Unavailable peeks never consume a decompression stream");
}
} // namespace

int main() {
    try {
        Boards();
        Bytes();
        ConsumingRead();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
