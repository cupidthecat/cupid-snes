#include "ConsoleConfig.hpp"
#include "state/EmulatorBoards.hpp"
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>

using namespace snes::core;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

struct Input final : IInputProvider {
    uint64_t polls = 0, fields = 0;

    InputState Poll(uint64_t) override {
        ++polls;
        InputState value;
        value.a = true;
        return value;
    }

    void AdvanceFrame() noexcept override {
        ++fields;
    }
};

void Profiles() {
    using namespace snes::tests::emulator_state;
    for (unsigned index = 0; index < unsigned(Board::Count); ++index)
        for (auto region : {Region::NTSC, Region::PAL}) {
            const auto board = Board(index);
            auto config = snes::tests::DeterministicConsoleConfig();
            config.cartridgeFirmware = Firmware(board);
            config.regionOverride = region;
            auto machine = std::make_unique<Emulator>(config);
            Input input;
            machine->AttachInputProvider(&input);
            machine->SetGameBoyTimeSource([] { return uint64_t(123456); });
            machine->SetBroadcastTimeSource([] { return BroadcastTime{}; });
            Load(*machine, board);
            machine->StepFrame();
            auto capture = machine->PrepareStateCapture();
            const auto position = capture.Position();
            Check(position.frame == machine->CurrentFrame() &&
                      position.masterCycles == machine->CurrentMasterCycles(),
                  "Prepared capture retains its exact position before worker completion");
            const auto expected = machine->CaptureState();
            Check(capture.Size() == expected.size(), "Prepared capture reports its complete size");
            machine->StepFrame();
            const auto frame = machine->CurrentFrame(), clock = machine->CurrentMasterCycles();
            const auto polls = input.polls, fields = input.fields;
            auto result = std::async(std::launch::async, [&] { return capture.Finish(); });
            const auto bytes = result.get();
            Check(bytes == expected && machine->CurrentFrame() == frame &&
                      machine->CurrentMasterCycles() == clock && input.polls == polls &&
                      input.fields == fields,
                  "Worker completion retains the captured bytes without touching resumed hardware");
            auto restore = machine->PrepareStateRestore(bytes);
            Check(restore.Position() == position,
                  "Restore preparation exposes the validated position before changing the machine");
            restore.Commit();
            Check(machine->CaptureState() == expected,
                  "Completed captures restore every supported cartridge profile");
        }
}

void OwnershipAndCancellation() {
    auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    auto image = snes::tests::cartridge_state::Image(0x20, 2, 32768);
    image[0] = 0xdb;
    Check(machine->LoadCartridge(image), "Load detached capture guest");
    machine->StepFrame();
    const auto expected = machine->CaptureState();
    auto capture = machine->PrepareStateCapture();
    machine.reset();
    bool cancelled = false;
    try {
        capture.Finish(StateOperation([](const StateProgress&) { return false; }));
    } catch (const StateCancelled&) {
        cancelled = true;
    }
    Check(cancelled && capture.Size() == expected.size(),
          "Cancelling checksum completion retains the detached payload for retry");
    auto moved = std::move(capture);
    Check(capture.Size() == 0, "Moving capture transfers sole payload ownership");
    auto future =
        std::async(std::launch::async, [capture = std::move(moved)] { return capture.Finish(); });
    Check(future.get() == expected,
          "Capture completion survives owner destruction, cancellation and transfer to a worker");
}
} // namespace

int main() {
    try {
        Profiles();
        OwnershipAndCancellation();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::puts("Deferred state capture checks passed");
    return 0;
}
