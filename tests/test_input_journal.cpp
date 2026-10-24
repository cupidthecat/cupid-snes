#include "ConsoleConfig.hpp"
#include "input/InputJournal.hpp"
#include "snes/core/detail/Sha256.hpp"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>

using namespace snes::core;
using namespace snes::frontend;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class F> void Reject(F perform, const char* message) {
    try {
        perform();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

InputState Pad(uint32_t mask) {
    return {bool(mask & 1),   bool(mask & 2),   bool(mask & 4),    bool(mask & 8),
            bool(mask & 16),  bool(mask & 32),  bool(mask & 64),   bool(mask & 128),
            bool(mask & 256), bool(mask & 512), bool(mask & 1024), bool(mask & 2048)};
}

bool Equal(const InputState& a, const InputState& b) {
    return std::tie(a.up, a.down, a.left, a.right, a.a, a.b, a.x, a.y, a.l, a.r, a.start,
                    a.select) ==
           std::tie(b.up, b.down, b.left, b.right, b.a, b.b, b.x, b.y, b.l, b.r, b.start, b.select);
}

bool Equal(const MouseState& a, const MouseState& b) {
    return std::tie(a.dx, a.dy, a.left, a.right) == std::tie(b.dx, b.dy, b.left, b.right);
}

bool Equal(const LightGunState& a, const LightGunState& b) {
    return std::tie(a.x, a.y, a.trigger, a.cursor, a.turbo, a.pause, a.start, a.offscreen) ==
           std::tie(b.x, b.y, b.trigger, b.cursor, b.turbo, b.pause, b.start, b.offscreen);
}

struct Host final : IInputProvider {
    uint64_t queries = 0, fields = 0;
    bool alternate = false, varying = false;

    InputState Poll(uint64_t frame) override {
        ++queries;
        return Pad(alternate ? 0 : uint32_t(frame) ^ 0xa55);
    }

    InputState PollController(int player, uint64_t frame) override {
        ++queries;
        return Pad(alternate
                       ? 0xfff
                       : uint32_t(frame * 137 + (player + 1) * 53 + (varying ? queries * 7 : 0)));
    }

    MouseState PollMouse(int port, uint64_t frame) override {
        ++queries;
        return {port ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int32_t>::max(),
                int32_t((frame + (varying ? queries : 0)) % 65536) - 32768, bool(frame & 1),
                bool(frame & 2)};
    }

    LightGunState PollLightGun(int port, int gun, uint64_t frame) override {
        ++queries;
        return {int32_t((frame * 17 + (varying ? queries : 0)) % 320) - 16,
                gun ? -32768 : 240,
                bool(frame & 1),
                bool(frame & 2),
                bool(frame & 4),
                bool(frame & 8),
                bool(frame & 16),
                bool((frame + port + gun) & 32)};
    }

    void AdvanceFrame() noexcept override {
        ++fields;
    }
};

void ClockAndSeek() {
    Host host;
    InputJournal journal(&host);
    unsigned clockCalls = 0;
    uint64_t clock = 0xfedcba9876543210ull;
    const auto readClock = [&] {
        ++clockCalls;
        return clock;
    };
    journal.Record();
    Check(journal.PollClock(InputJournal::Query::HandheldClock, 7, readClock) == clock,
          "Recording retains every bit of the handheld timestamp");
    journal.PollController(0, 7);
    journal.PollClock(InputJournal::Query::BroadcastClock, 8, readClock);
    Reject([&] { journal.Seek(4); }, "Out-of-range recording cursors reject");
    Check(journal.Position() == 3, "Rejected recording seeks retain the original future");
    journal.Seek(1);
    clock = 0x0123456789abcdefull;
    journal.PollClock(InputJournal::Query::BroadcastClock, 9, readClock);
    const auto recording = journal.FinishRecording();
    Check(recording->size() == 2 && recording->back().frame == 9,
          "Recording after rewind replaces only the discarded future");
    const auto calls = clockCalls;
    journal.Play(recording, {.advanceSource = false, .liveAfterEnd = false});
    Check(journal.PollClock(InputJournal::Query::HandheldClock, 7, readClock) ==
                  0xfedcba9876543210ull &&
              journal.PollClock(InputJournal::Query::BroadcastClock, 9, readClock) == clock &&
              clockCalls == calls,
          "Playback reproduces typed clock values without reading the live clock");
    journal.Seek(0);
    Reject(
        [&] {
            journal.Play(recording, {}, StateOperation([](const StateProgress&) { return false; }));
        },
        "Cancelled movie preparation stops before changing the input timeline");
    Check(journal.Status() == InputJournal::State::Playback && journal.Position() == 0,
          "Cancelling validation retains the active recording and cursor");
    Reject([&] { journal.Seek(3); }, "Out-of-range playback cursors reject");
    Check(journal.Status() == InputJournal::State::Playback && journal.Position() == 0,
          "Rejected playback seeks preserve the active cursor");
    journal.PollClock(InputJournal::Query::BroadcastClock, 7, readClock);
    Check(journal.Status() == InputJournal::State::Failed && clockCalls == calls,
          "A mismatched clock domain stops replay without sampling live time");
    journal.Seek(0);
    Check(!journal.Failure() && journal.PollClock(InputJournal::Query::HandheldClock, 7,
                                                  readClock) == 0xfedcba9876543210ull,
          "Restoring a valid playback cursor clears the abandoned failure");
    journal.Stop();
    Reject([&] { journal.Seek(0); }, "Idle input cannot seek a discarded timeline");
    Reject([&] { journal.PollClock(InputJournal::Query::Mouse, 0, readClock); },
           "Input device kinds cannot be used as clock queries");
}

void Contract() {
    Host host;
    InputJournal journal(&host);
    Check(Equal(journal.Poll(7), Pad(7 ^ 0xa55)), "Live input delegates the primary query");
    journal.Record();

    struct Values {
        InputState primary;
        std::array<InputState, 8> pads;
        std::array<MouseState, 4> mice;
        std::array<LightGunState, 4> guns;
    };

    std::vector<Values> expected;
    for (uint64_t frame = 0; frame < 64; ++frame) {
        Values values;
        values.primary = journal.Poll(frame);
        for (int player = 0; player < 8; ++player)
            values.pads[size_t(player)] = journal.PollController(player, frame);
        for (int sample = 0; sample < 4; ++sample)
            values.mice[size_t(sample)] = journal.PollMouse(sample % 2, frame);
        for (int port = 0; port < 2; ++port)
            for (int gun = 0; gun < 2; ++gun)
                values.guns[size_t(port * 2 + gun)] = journal.PollLightGun(port, gun, frame);
        journal.AdvanceFrame();
        expected.push_back(values);
    }
    const auto count = journal.Position();
    const auto recording = journal.FinishRecording();
    Check(recording->size() == 64 * 17 && count == recording->size() && host.fields == 64,
          "Each query is recorded once; field notifications add no queries");
    Check(recording->front().query == InputJournal::Query::Primary &&
              recording->front().buttons == 0xa55 && (*recording)[1].buttons == 53,
          "Primary and player queries retain distinct identities and button bits");
    host.alternate = true;
    const auto queries = host.queries;
    journal.Play(recording);
    for (uint64_t frame = 0; frame < expected.size(); ++frame) {
        const auto& values = expected[size_t(frame)];
        Check(Equal(journal.Poll(frame), values.primary), "Replay all primary button bits");
        for (int player = 0; player < 8; ++player)
            Check(Equal(journal.PollController(player, frame), values.pads[size_t(player)]),
                  "Replay each of eight players independently");
        for (int sample = 0; sample < 4; ++sample)
            Check(Equal(journal.PollMouse(sample % 2, frame), values.mice[size_t(sample)]),
                  "Replay repeated mouse queries and full signed movement");
        for (int port = 0; port < 2; ++port)
            for (int gun = 0; gun < 2; ++gun)
                Check(Equal(journal.PollLightGun(port, gun, frame),
                            values.guns[size_t(port * 2 + gun)]),
                      "Replay every gun coordinate, flag and query identity");
        journal.AdvanceFrame();
    }
    Check(journal.Status() == InputJournal::State::Finished && journal.Position() == count &&
              host.queries == queries && host.fields == 128,
          "Playback consumes no live queries and retains ordinary field notifications");
    Check(Equal(journal.PollController(0, 64), Pad(0xfff)) && host.queries == queries + 1,
          "The first query after movie completion uses live input");
    journal.Play(recording, {.cursor = count - 1, .advanceSource = false, .liveAfterEnd = false});
    Check(Equal(journal.PollLightGun(1, 1, 63), expected.back().guns[3]),
          "Playback starts at an exact consumed-query cursor");
    journal.AdvanceFrame();
    Check(host.fields == 128, "Reconstruction can suppress live field notifications");
    Check(Equal(journal.PollController(0, 64), {}) && journal.Failure(),
          "Strict reconstruction rejects an exhausted log without polling live input");
    journal.Stop();
    Check(!journal.Failure() && Equal(journal.PollController(0, 64), Pad(0xfff)),
          "Stopping playback restores live input");

    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        auto invalid = std::make_shared<InputJournal::Recording>(*recording);
        switch (mutation) {
        case 0:
            (*invalid)[0].query = InputJournal::Query(255);
            break;
        case 1:
            (*invalid)[0].buttons = 4096;
            break;
        case 2:
            (*invalid)[1].index = 8;
            break;
        case 3:
            (*invalid)[0].x = 1;
            break;
        case 4:
            (*invalid)[13].gun = 2;
            break;
        case 5:
            (*invalid)[17].frame = 1000;
            break;
        case 6:
            (*invalid)[9].buttons = 4;
            break;
        }
        journal.Play(recording, {.cursor = 3});
        Reject([&] { journal.Play(invalid); }, "Reject malformed recordings before replacement");
        Check(journal.Status() == InputJournal::State::Playback && journal.Position() == 3,
              "Rejected playback retains its previous owner and cursor");
    }
    Reject([&] { journal.Play({}); }, "Reject a missing recording");
    Reject([&] { journal.Play(recording, {.cursor = count + 1}); }, "Reject a cursor past the log");
    journal.Play(recording, {.cursor = 13});
    journal.PollLightGun(0, 1, 0);
    Check(journal.Failure() && journal.Position() == 13,
          "The gun number is part of a light-gun query's identity");
    journal.Play(recording, {.cursor = 9});
    journal.PollMouse(1, 0);
    Check(journal.Failure() && journal.Position() == 9,
          "The port is part of a mouse query's identity");
    for (unsigned mismatch = 0; mismatch < 4; ++mismatch) {
        journal.Play(recording, {.cursor = 1});
        const auto before = host.queries;
        if (mismatch == 0)
            journal.Poll(0);
        else if (mismatch == 1)
            journal.PollController(1, 0);
        else if (mismatch == 2)
            journal.PollController(0, 1);
        else
            journal.PollLightGun(0, 1, 0);
        Check(journal.Status() == InputJournal::State::Failed && journal.Position() == 1 &&
                  host.queries == before,
              "Query type, device, gun and field mismatch fail without consuming input");
    }
    journal.Record(1);
    journal.PollController(0, 10);
    Check(Equal(journal.PollController(0, 11), Pad(0xfff)) && journal.Failure(),
          "Recording exhaustion retains live gameplay and reports failure");
    Reject([&] { journal.FinishRecording(); }, "A failed recording cannot be exported as complete");
    journal.Record();
    journal.Poll(10);
    journal.Poll(9);
    Check(journal.Failure(), "A reset cannot silently create a backwards recording timeline");
    journal.Stop();
    auto empty = std::make_shared<const InputJournal::Recording>();
    journal.Play(empty);
    Check(journal.Status() == InputJournal::State::Finished && Equal(journal.Poll(0), {}),
          "Empty playback finishes immediately and uses live input");
    InputJournal disconnected;
    disconnected.Record();
    Check(Equal(disconnected.Poll(0), {}) && Equal(disconnected.PollController(7, 0), {}) &&
              Equal(disconnected.PollMouse(1, 0), {}) &&
              Equal(disconnected.PollLightGun(1, 1, 0), {}),
          "Disconnected input records the platform's neutral device values");
    Check(disconnected.FinishRecording()->size() == 4, "Neutral queries retain their identities");
}

std::vector<uint8_t> Image(bool dma) {
    std::vector<uint8_t> image(32768), program{0x78};
    std::fill_n(image.begin() + 0x7fc0, 21, ' ');
    image[0x7fd5] = 0x20;
    image[0x7fd7] = 5;
    image[0x7fd9] = 1;
    image[0x7fdc] = image[0x7fdd] = 0xff;
    image[0x7ffd] = 0x80;
    const auto write = [&](uint16_t address, uint8_t value) {
        program.insert(program.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
    };
    write(0x2100, 0x80);
    write(0x4200, 1);
    if (dma) {
        for (unsigned channel = 0; channel < 8; ++channel) {
            const auto base = uint16_t(0x4300 + channel * 16);
            for (unsigned index = 0; index < 7; ++index)
                write(uint16_t(base + index), index == 1 ? 0x22 : index == 4 ? 0x7e : 0);
        }
        write(0x420b, 255);
    }
    const auto loop = uint16_t(0x8000 + program.size());
    write(0x4016, 1);
    write(0x4016, 0);
    program.insert(program.end(),
                   {0xad, 0x16, 0x40, 0xad, 0x17, 0x40, 0x4c, uint8_t(loop), uint8_t(loop >> 8)});
    std::copy(program.begin(), program.end(), image.begin());
    return image;
}

struct Witness final : IVideoOutput, IAudioOutput {
    uint64_t video = 14695981039346656037ull, pcm = video, frames = 0, samples = 0;

    static void Hash(uint64_t& hash, uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte)
            hash = (hash ^ uint8_t(value >> (byte * 8))) * 1099511628211ull;
    }

    void Present(const VideoFrame& frame) override {
        ++frames;
        Hash(video, frame.width);
        Hash(video, frame.height);
        for (const auto pixel : frame.pixels)
            Hash(video, pixel);
    }

    void Submit(const AudioBuffer& buffer) override {
        samples += buffer.interleavedStereo.size();
        Hash(pcm, buffer.sampleRate);
        for (const auto sample : buffer.interleavedStereo)
            Hash(pcm, std::bit_cast<uint32_t>(sample));
    }

    bool operator==(const Witness& other) const {
        return std::tie(video, pcm, frames, samples) ==
               std::tie(other.video, other.pcm, other.frames, other.samples);
    }
};

void Put(std::vector<uint8_t>& out, uint64_t value, unsigned bytes) {
    for (unsigned byte = 0; byte < bytes; ++byte)
        out.push_back(uint8_t(value >> (byte * 8)));
}

void Continuation(bool pal, ControllerDevice device, std::vector<uint8_t>& fixture) {
    auto config = snes::tests::DeterministicConsoleConfig();
    config.regionOverride = pal ? Region::PAL : Region::NTSC;
    const auto image = Image(device == ControllerDevice::Gamepad);
    const auto load = [&](Emulator& machine) {
        std::string error;
        Check(machine.LoadCartridge(image, &error), "Load journal continuation guest");
        auto& ports = machine.GetAutoJoypad().Ports();
        ports.Configure(0,
                        device == ControllerDevice::Multitap ? device : ControllerDevice::Gamepad,
                        {0, 1, 2, 3});
        ports.Configure(1, device,
                        device == ControllerDevice::Multitap ? std::array<int, 4>{4, 5, 6, 7}
                                                             : std::array<int, 4>{1, 2, 3, 4});
    };
    Host live;
    live.varying = true;
    InputJournal recorder(&live);
    auto source = std::make_unique<Emulator>(config);
    load(*source);
    Witness all;
    source->AttachVideoOutput(&all);
    source->AttachAudioOutput(&all);
    source->AttachInputProvider(&recorder);
    const auto initial = source->CaptureState();
    recorder.Record();
    source->StepFrame();
    const auto middle = source->CaptureState();
    const auto cursor = recorder.Position();
    const auto middleFrame = source->CurrentFrame();
    if (device == ControllerDevice::Gamepad)
        Check(middleFrame > 1, "Recording observes fields completed by long-DMA capture");
    Witness tail;
    source->AttachVideoOutput(&tail);
    source->AttachAudioOutput(&tail);
    for (unsigned field = 0; field < 3; ++field)
        source->StepFrame();
    const auto end = source->CaptureState();
    const auto recording = recorder.FinishRecording();
    Check(cursor > 0 && recording->size() > cursor,
          "Recording includes both sides of the checkpoint");

    Host unrelated;
    unrelated.alternate = true;
    InputJournal playback(&unrelated);
    auto target = std::make_unique<Emulator>(config);
    load(*target);
    target->RestoreState(initial);
    target->AttachInputProvider(&playback);
    Witness observed;
    target->AttachVideoOutput(&observed);
    target->AttachAudioOutput(&observed);
    playback.Play(recording, {.advanceSource = false, .liveAfterEnd = false});
    target->StepFrame();
    Check(target->CaptureState() == middle && observed == all && playback.Position() == cursor,
          "Ordered replay retains every crossed field, complete machine, picture and PCM prefix");
    Check(!playback.Failure() && unrelated.queries == 0 && unrelated.fields == 0,
          "Reconstruction never polls or advances unrelated live input");
    observed = Witness{};
    for (unsigned field = 0; field < 3; ++field)
        target->StepFrame();
    Check(target->CaptureState() == end && observed == tail && !playback.Failure() &&
              playback.Position() == recording->size(),
          "Replay continues exactly after a checkpoint containing partial-field input");

    target->RestoreState(middle);
    observed = Witness{};
    playback.Play(recording, {.cursor = cursor, .advanceSource = false, .liveAfterEnd = false});
    for (unsigned field = 0; field < 3; ++field)
        target->StepFrame();
    Check(target->CaptureState() == end && observed == tail && !playback.Failure() &&
              playback.Position() == recording->size(),
          "Restoring the consumed-query cursor skips input already latched in the checkpoint");
    if (device == ControllerDevice::Multitap)
        for (int player = 0; player < 8; ++player)
            Check(std::any_of(recording->begin(), recording->end(),
                              [&](const auto& sample) {
                                  return sample.query == InputJournal::Query::Controller &&
                                         sample.index == player;
                              }),
                  "The guest samples every multitap player");
    if (device == ControllerDevice::Mouse)
        Check(std::any_of(
                  recording->begin(), recording->end(),
                  [](const auto& sample) { return sample.query == InputJournal::Query::Mouse; }),
              "The guest samples mouse latches");
    if (IsLightGun(device))
        Check(std::any_of(
                  recording->begin(), recording->end(),
                  [](const auto& sample) { return sample.query == InputJournal::Query::LightGun; }),
              "The guest samples gun buttons and aim");
    const auto digest = detail::Sha256(end);
    fixture.insert(fixture.end(), digest.begin(), digest.end());
    std::vector<uint8_t> packed;
    for (const auto& sample : *recording) {
        Put(packed, sample.frame, 8);
        Put(packed, uint8_t(sample.query), 1);
        Put(packed, unsigned(sample.index), 1);
        Put(packed, unsigned(sample.gun), 1);
        Put(packed, sample.buttons, 4);
        Put(packed, uint32_t(sample.x), 4);
        Put(packed, uint32_t(sample.y), 4);
    }
    const auto inputDigest = detail::Sha256(packed);
    fixture.insert(fixture.end(), inputDigest.begin(), inputDigest.end());
    Put(fixture, cursor, 8);
    Put(fixture, recording->size(), 8);
}
} // namespace

int main(int argc, char** argv) {
    try {
        Contract();
        ClockAndSeek();
        std::vector<uint8_t> fixture;
        for (bool pal : {false, true})
            for (auto device :
                 {ControllerDevice::Gamepad, ControllerDevice::Multitap, ControllerDevice::Mouse,
                  ControllerDevice::SuperScope, ControllerDevice::Justifier,
                  ControllerDevice::Justifiers, ControllerDevice::MacsRifle})
                Continuation(pal, device, fixture);
        if (argc > 1) {
            std::ofstream file(argv[1], std::ios::binary);
            file.write(reinterpret_cast<const char*>(fixture.data()),
                       std::streamsize(fixture.size()));
            Check(bool(file), "Write journal portability fixture");
        }
        std::puts(
            "Input journal checks passed: typed queries, failure handling and 14 continuation "
            "profiles");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
