#include "ConsoleConfig.hpp"
#include "cartridge/StateFixture.hpp"
#include "session/Playback.hpp"
#include <bit>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace snes::core;
using namespace snes::frontend;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class Function> void Reject(Function function, const char* message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

struct Host final : IInputProvider {
    uint64_t polls = 0, fields = 0;
    bool unrelated = false;

    InputState Poll(uint64_t frame) override {
        return PollController(0, frame);
    }

    InputState PollController(int player, uint64_t frame) override {
        ++polls;
        InputState result;
        result.a = unrelated || ((polls + frame + unsigned(player)) & 1);
        result.start = (polls + frame) % 3 == 0;
        return result;
    }

    MouseState PollMouse(int port, uint64_t frame) override {
        ++polls;
        return {int32_t(polls * 7 + unsigned(port)), int32_t(frame) - 20, true, false};
    }

    LightGunState PollLightGun(int port, int gun, uint64_t frame) override {
        ++polls;
        return {int32_t((polls * 13) % 256),
                int32_t(frame % 224),
                true,
                false,
                bool(port),
                bool(gun),
                false,
                false};
    }

    void AdvanceFrame() noexcept override {
        ++fields;
    }
};

struct Output final : IVideoOutput, IAudioOutput {
    uint64_t video = 14695981039346656037ull, pcm = video, pictures = 0, samples = 0;

    static void Hash(uint64_t& hash, uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte)
            hash = (hash ^ uint8_t(value >> (byte * 8))) * 1099511628211ull;
    }

    void Present(const VideoFrame& frame) override {
        ++pictures;
        Hash(video, frame.width);
        Hash(video, frame.height);
        for (auto pixel : frame.pixels)
            Hash(video, pixel);
    }

    void Submit(const AudioBuffer& buffer) override {
        samples += buffer.interleavedStereo.size();
        Hash(pcm, buffer.sampleRate);
        for (auto value : buffer.interleavedStereo)
            Hash(pcm, std::bit_cast<uint32_t>(value));
    }

    bool operator==(const Output& other) const {
        return video == other.video && pcm == other.pcm && pictures == other.pictures &&
               samples == other.samples;
    }
};

std::vector<uint8_t> Image(bool dma = false, bool quiet = false) {
    auto image = snes::tests::cartridge_state::Image(0x20, 2, 32768);
    if (quiet) {
        image[0] = 0xdb;
        return image;
    }
    std::vector<uint8_t> program{0x78};
    const auto write = [&](uint16_t address, uint8_t value) {
        program.insert(program.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
    };
    write(0x4200, 1);
    if (dma) {
        for (unsigned channel = 0; channel < 8; ++channel)
            for (unsigned offset = 0; offset < 7; ++offset)
                write(uint16_t(0x4300 + channel * 16 + offset), offset == 1   ? 0x22
                                                                : offset == 4 ? 0x7e
                                                                              : 0);
        write(0x420b, 255);
    }
    const auto loop = uint16_t(0x8000 + program.size());
    write(0x4016, 1);
    write(0x4016, 0);
    program.insert(program.end(), {0xad, 0x16, 0x40, 0x8f, 0x00, 0x00, 0x70, 0xad, 0x17, 0x40, 0x4c,
                                   uint8_t(loop), uint8_t(loop >> 8)});
    std::copy(program.begin(), program.end(), image.begin());
    return image;
}

std::array<uint8_t, 32> Cheats() {
    return Playback::CheatIdentity(CheatSystem::Console, {});
}

void WaitHistory(Playback& playback) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (playback.History().preparing) {
        Check(std::chrono::steady_clock::now() < deadline, "Rewind worker completes");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(playback.History().error.empty(), "History capture retains a valid checkpoint");
}

void Replay(bool pal, ControllerDevice device) {
    auto config = snes::tests::DeterministicConsoleConfig();
    config.regionOverride = pal ? Region::PAL : Region::NTSC;
    auto source = std::make_unique<Emulator>(config);
    auto target = std::make_unique<Emulator>(config);
    const auto image = Image(device == ControllerDevice::Gamepad);
    for (auto* machine : {source.get(), target.get()}) {
        Check(machine->LoadCartridge(image), "Load recording guest");
        machine->GetAutoJoypad().Ports().Configure(
            0, device == ControllerDevice::Multitap ? device : ControllerDevice::Gamepad,
            {0, 1, 2, 3});
        machine->GetAutoJoypad().Ports().Configure(1, device,
                                                   device == ControllerDevice::Multitap
                                                       ? std::array<int, 4>{4, 5, 6, 7}
                                                       : std::array<int, 4>{1, 2, 3, 4});
    }
    Host live, unrelated;
    unrelated.unrelated = true;
    Playback recorder(*source, &live), player(*target, &unrelated);
    recorder.Record(Cheats());
    Output expected, observed;
    source->AttachVideoOutput(&expected);
    source->AttachAudioOutput(&expected);
    for (unsigned frame = 0; frame < 4; ++frame)
        recorder.StepFrame();
    recorder.FinishRecording();
    const auto complete = source->CaptureState();
    auto movie = std::make_shared<const InputMovie>(*recorder.PendingMovie());
    Check(movie->Info().lastFrame == source->CurrentFrame() &&
              movie->Info().lastClock == source->CurrentMasterCycles(),
          "The movie retains its coherent endpoint");
    const auto prior = target->CaptureState();
    auto wrong = Cheats();
    wrong[0] ^= 1;
    Reject([&] { player.Play(movie, wrong); },
           "Different cheat policies reject before restoration");
    Check(target->CaptureState() == prior && player.Status() == Playback::Mode::Live,
          "Rejected movie preparation retains the live machine");
    auto descriptor = movie->Info();
    ++descriptor.firstClock;
    auto bad = std::make_shared<const InputMovie>(
        std::vector<uint8_t>(movie->InitialState().begin(), movie->InitialState().end()),
        movie->Inputs(), descriptor);
    Reject([&] { player.Play(bad, Cheats()); }, "False initial clock metadata rejects");
    Check(target->CaptureState() == prior, "Malformed movie coordinates never replace execution");
    target->AttachVideoOutput(&observed);
    target->AttachAudioOutput(&observed);
    player.Play(movie, Cheats());
    for (unsigned step = 0; player.Status() != Playback::Mode::Finished && step < 100; ++step)
        player.StepFrame();
    Check(player.Status() == Playback::Mode::Finished && target->CaptureState() == complete &&
              observed == expected && !unrelated.polls &&
              player.Position() == movie->Inputs()->size(),
          "Movie replay retains every field, machine byte, picture and PCM sample without live "
          "input");
    player.StopPlayback();
    Check(player.Status() == Playback::Mode::Live, "Stopping playback restores live input");
    if (!pal && device == ControllerDevice::Gamepad) {
        auto falseEnd = movie->Info();
        falseEnd.lastClock += 2;
        auto invalid = std::make_shared<const InputMovie>(movie->ShareInitialState(),
                                                          movie->Inputs(), falseEnd);
        player.Play(invalid, Cheats());
        Reject(
            [&] {
                for (unsigned step = 0; step < 100; ++step)
                    player.StepFrame();
            },
            "A false endpoint stops playback");
        Check(player.Status() == Playback::Mode::Failed && player.Replaying() && player.Failure() &&
                  std::string_view(player.Failure()).find("endpoint") != std::string_view::npos &&
                  target->CaptureState() == complete,
              "Endpoint mismatch stops at the recorded machine and remains recoverable");
        player.StopPlayback();
    }
}

void ClockBindings() {
    auto config = snes::tests::DeterministicConsoleConfig();
    config.expansionDevice = Emulator::ExpansionDevice::Satellaview;
    auto machine = std::make_unique<Emulator>(config);
    Check(machine->LoadCartridge(Image(false, true)), "Load clock binding guest");
    unsigned handheldCalls = 0, broadcastCalls = 0;
    machine->SetGameBoyTimeSource([&] { return 123456ull + ++handheldCalls; });
    machine->SetBroadcastTimeSource([&] {
        ++broadcastCalls;
        return BroadcastTime{uint16_t(2000 + broadcastCalls), 12, 31, 7, 23, 59, 58};
    });
    {
        Playback playback(*machine, nullptr);
        playback.Record(Cheats());
        uint64_t time;
        BroadcastTime calendar;
        {
            auto work = playback.Emulating();
            time = machine->GetGameBoyTimeSource()();
            calendar = machine->GetBroadcastTimeSource()();
        }
        playback.FinishRecording();
        auto movie = std::make_shared<const InputMovie>(*playback.PendingMovie());
        playback.DiscardPending();
        const auto handheldBefore = handheldCalls, broadcastBefore = broadcastCalls;
        playback.Play(movie, Cheats());
        {
            auto work = playback.Emulating();
            const auto actualTime = machine->GetGameBoyTimeSource()();
            const auto actualCalendar = machine->GetBroadcastTimeSource()();
            Check(actualTime == time && actualCalendar.year == calendar.year &&
                      actualCalendar.month == 12 && actualCalendar.day == 31 &&
                      actualCalendar.weekday == 7 && actualCalendar.hour == 23 &&
                      actualCalendar.minute == 59 && actualCalendar.second == 58,
                  "Both clock domains reproduce their complete recorded values");
        }
        playback.StepFrame();
        Check(playback.Status() == Playback::Mode::Finished && handheldCalls == handheldBefore &&
                  broadcastCalls == broadcastBefore,
              "Clock replay never calls the live providers and can finish without another field");
    }
    const auto handheldBefore = handheldCalls, broadcastBefore = broadcastCalls;
    machine->GetGameBoyTimeSource()();
    machine->GetBroadcastTimeSource()();
    Check(handheldCalls == handheldBefore + 1 && broadcastCalls == broadcastBefore + 1,
          "Destroying playback restores both original clock bindings");
}

void RewindAndBranch() {
    auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    Check(machine->LoadCartridge(Image()), "Load rewind guest");
    Host host;
    Playback playback(*machine, &host);
    playback.ConfigureRewind({1, 3, size_t(32) * 1024 * 1024});
    playback.StepFrame({}, false);
    Check(playback.History().states == 0 && !playback.History().preparing,
          "Explicit paused advances do not capture rewind history");
    std::vector<std::vector<uint8_t>> states;
    for (unsigned field = 0; field < 4; ++field) {
        playback.StepFrame();
        WaitHistory(playback);
        states.push_back(machine->CaptureState());
    }
    Check(playback.History().states == 3 && playback.History().bytes <= 32 * 1024 * 1024,
          "History evicts old checkpoints at its count and byte limits");
    const auto polls = host.polls, fields = host.fields;
    Check(playback.Rewind() && machine->CaptureState() == states[2] && host.polls == polls &&
              host.fields == fields,
          "Rewind restores the previous complete checkpoint without polling or advancing input");
    const auto beforeCancel = machine->CaptureState();
    const auto count = playback.History().states;
    Reject([&] { playback.Rewind(StateOperation([](const StateProgress&) { return false; })); },
           "Cancelled rewind stops before commit");
    Check(machine->CaptureState() == beforeCancel && playback.History().states == count,
          "Cancelled rewind retains the current machine and history");
    playback.ConfigureRewind({1, 10, size_t(64) * 1024 * 1024});
    playback.Record(Cheats());
    for (unsigned field = 0; field < 4; ++field) {
        playback.StepFrame();
        WaitHistory(playback);
    }
    const auto oldCursor = playback.Position();
    Check(playback.Rewind() && playback.Position() < oldCursor,
          "Recording rewind truncates input at the restored checkpoint");
    host.unrelated = true;
    for (unsigned field = 0; field < 2; ++field)
        playback.StepFrame({}, false);
    playback.FinishRecording();
    const auto expected = machine->CaptureState();
    auto movie = std::make_shared<const InputMovie>(*playback.PendingMovie());
    playback.DiscardPending();
    playback.Play(movie, Cheats());
    for (unsigned step = 0; playback.Status() != Playback::Mode::Finished && step < 100; ++step)
        playback.StepFrame({}, false);
    Check(playback.Status() == Playback::Mode::Finished && machine->CaptureState() == expected,
          "A rerecorded branch replays the replacement future exactly");
    playback.StopPlayback();
    const auto oneState = machine->CaptureState().size();
    const auto budget = oneState * 5 / 2;
    playback.ConfigureRewind({1, 10, budget});
    for (unsigned field = 0; field < 4; ++field) {
        playback.StepFrame();
        WaitHistory(playback);
    }
    Check(playback.History().states == 2 && playback.History().bytes <= budget,
          "The byte budget evicts history before its checkpoint-count limit");
    playback.ConfigureRewind({1, 10, 1});
    playback.StepFrame();
    Check(!playback.History().error.empty() && !playback.History().preparing &&
              playback.Status() == Playback::Mode::Live,
          "An oversized checkpoint disables history without stopping live play");
    playback.StepFrame();
    playback.ConfigureRewind({1, 10, size_t(64) * 1024 * 1024});
    playback.StepFrame();
    WaitHistory(playback);
    Check(playback.History().states == 1, "Changing rewind limits rearms capture after failure");
}

void EmptyMovie() {
    auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    Check(machine->LoadCartridge(Image(false, true)), "Load empty movie guest");
    Host host;
    Playback playback(*machine, &host);
    playback.Record(Cheats());
    playback.FinishRecording();
    auto movie = std::make_shared<const InputMovie>(*playback.PendingMovie());
    const auto expected = machine->CaptureState();
    const auto fields = host.fields, polls = host.polls;
    playback.DiscardPending();
    playback.Play(movie, Cheats());
    playback.StepFrame();
    Check(playback.Status() == Playback::Mode::Finished && movie->Inputs()->empty() &&
              machine->CaptureState() == expected && host.fields == fields && host.polls == polls,
          "A zero-field movie finishes without advancing or polling the machine");
}

void RuntimeClocks(bool pal, bool handheld, bool secondModel = false) {
    auto config = snes::tests::DeterministicConsoleConfig();
    config.regionOverride = pal ? Region::PAL : Region::NTSC;
    config.expansionDevice = Emulator::ExpansionDevice::Satellaview;
    auto source = std::make_unique<Emulator>(config), target = std::make_unique<Emulator>(config);
    uint64_t sourceCalls = 0, targetCalls = 0;
    source->SetGameBoyTimeSource([&] { return 1700000000ull + ++sourceCalls; });
    target->SetGameBoyTimeSource([&] { return 1900000000ull + ++targetCalls; });
    source->SetBroadcastTimeSource(
        [&] { return BroadcastTime{2026, 10, 23, 6, 12, 34, uint8_t(++sourceCalls % 60)}; });
    target->SetBroadcastTimeSource(
        [&] { return BroadcastTime{2040, 1, 1, 1, 0, 0, uint8_t(++targetCalls % 60)}; });
    for (auto* machine : {source.get(), target.get()}) {
        if (handheld) {
            auto bios = snes::tests::cartridge_state::Image(
                0x20, 0xe3, 0x80000, secondModel ? "Super GAMEBOY2" : "Super GAMEBOY");
            const std::array<uint8_t, 6> code{0xa9, 0x81, 0x8d, 3, 0x60, 0xdb};
            std::copy(code.begin(), code.end(), bios.begin());
            Check(machine->LoadSuperGameBoy(bios, snes::tests::handheld::Image(0x10),
                                            snes::tests::handheld::Boot()),
                  "Load an actual running handheld clock");
        } else {
            auto image = Image();
            const std::array<uint8_t, 10> code{0xad, 0x92, 0x21, 0x8f, 0, 0, 0x70, 0x4c, 0, 0x80};
            std::copy(code.begin(), code.end(), image.begin());
            Check(machine->LoadCartridge(image), "Load guest receiver clock reads");
        }
    }
    Playback recorder(*source, nullptr), player(*target, nullptr);
    const auto cheats = Playback::CheatIdentity(source->CheatsSystem(), {});
    recorder.Record(cheats);
    Output expected, actual;
    source->AttachVideoOutput(&expected);
    source->AttachAudioOutput(&expected);
    for (unsigned frame = 0; frame < 12; ++frame) {
        recorder.StepFrame();
        if (handheld && frame == 5)
            source->SaveSlotBattery(0);
    }
    recorder.FinishRecording();
    const auto complete = source->CaptureState();
    auto movie = std::make_shared<const InputMovie>(*recorder.PendingMovie());
    const auto query =
        handheld ? InputJournal::Query::HandheldClock : InputJournal::Query::BroadcastClock;
    Check(std::any_of(movie->Inputs()->begin(), movie->Inputs()->end(),
                      [&](const auto& sample) { return sample.query == query; }),
          "Real device execution records host clock queries");
    const auto beforeCalls = targetCalls;
    player.Play(movie, cheats);
    target->AttachVideoOutput(&actual);
    target->AttachAudioOutput(&actual);
    for (unsigned step = 0; player.Status() != Playback::Mode::Finished && step < 100; ++step)
        player.StepFrame();
    Check(player.Status() == Playback::Mode::Finished && targetCalls == beforeCalls &&
              target->CaptureState() == complete && expected == actual,
          "Real clocked device replay retains machine, picture and sound without host sampling");
}

void DisconnectedClockLifetime() {
    auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    Check(machine->LoadCartridge(Image()), "Load disconnected clock guest");
    unsigned calls = 0;
    machine->ExchangeBroadcastTimeSource([&] {
        ++calls;
        return BroadcastTime{2031, 2, 3, 4, 5, 6, 7};
    });
    {
        Playback playback(*machine, nullptr);
        machine->SetExpansionDevice(Emulator::ExpansionDevice::Satellaview);
        Check(machine->GetBroadcastTimeSource()().year == 2031 && calls == 1,
              "Connecting a receiver retains the installed host binding");
        machine->SetExpansionDevice(Emulator::ExpansionDevice::None);
    }
    machine->SetExpansionDevice(Emulator::ExpansionDevice::Satellaview);
    Check(machine->GetBroadcastTimeSource()().year == 2031 && calls == 2,
          "Clock bindings restore even when disconnected before playback destruction");
}
} // namespace

int main() {
    try {
        for (bool pal : {false, true})
            for (auto device :
                 {ControllerDevice::Gamepad, ControllerDevice::Multitap, ControllerDevice::Mouse,
                  ControllerDevice::SuperScope, ControllerDevice::Justifier,
                  ControllerDevice::Justifiers, ControllerDevice::MacsRifle})
                Replay(pal, device);
        ClockBindings();
        RewindAndBranch();
        EmptyMovie();
        DisconnectedClockLifetime();
        for (bool pal : {false, true}) {
            RuntimeClocks(pal, false);
            for (bool secondModel : {false, true})
                RuntimeClocks(pal, true, secondModel);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::puts("Playback checks passed");
    return 0;
}
