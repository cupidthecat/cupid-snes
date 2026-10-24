#include "snes/core/Dsp.hpp"
#include "snes/core/Smp.hpp"
#include "snes/core/audio/StreamMixer.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace snes::core;
using snes::core::audio::StreamMixer;
using snes::core::detail::StateArchive;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class Operation> void Reject(Operation operation, const char* message) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Check(rejected, message);
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

void Configure(Dsp& dsp, uint8_t* ram, unsigned profile) {
    for (unsigned address = 0; address < 65536; ++address)
        ram[address] = uint8_t(address * 37 + (address >> 8) * 13 + profile * 19);
    dsp.SetRam(ram);
    for (unsigned address = 0; address < Dsp::RegisterCount; ++address)
        dsp.Write(uint8_t(address), 0);
    dsp.Write(Dsp::kDir, 4);
    dsp.Write(Dsp::kMVolL, 127);
    dsp.Write(Dsp::kMVolR, 97);
    dsp.Write(Dsp::kEVolL, 53);
    dsp.Write(Dsp::kEVolR, uint8_t(-41));
    dsp.Write(Dsp::kEfb, 85);
    dsp.Write(Dsp::kEon, 0xff);
    dsp.Write(Dsp::kEsa, 0x60);
    dsp.Write(Dsp::kEdl, uint8_t(profile ? 15 : 1));
    dsp.Write(Dsp::kNon, uint8_t(profile * 0x53));
    dsp.Write(Dsp::kPmon, 0xfe);
    const int coefficients[]{64, 32, 16, 8, -8, 4, -4, 2};
    for (unsigned tap = 0; tap < 8; ++tap)
        dsp.Write(uint8_t(15 + tap * 16), uint8_t(coefficients[tap]));
    for (unsigned voice = 0; voice < Dsp::VoiceCount; ++voice) {
        const unsigned start = 0x1000 + voice * 0x200;
        for (unsigned entry = 0; entry < 2; ++entry) {
            ram[0x400 + voice * 4 + entry * 2] = uint8_t(start);
            ram[0x401 + voice * 4 + entry * 2] = uint8_t(start >> 8);
        }
        for (unsigned block = 0; block < 4; ++block) {
            ram[start + block * 9] =
                uint8_t(0x90 | ((voice + block) % 4) * 4 | (block == 3 ? 3 : 0));
            for (unsigned byte = 1; byte < 9; ++byte)
                ram[start + block * 9 + byte] = uint8_t(voice * 23 + block * 31 + byte * 17);
        }
        const unsigned base = voice * 16;
        dsp.Write(uint8_t(base), uint8_t(93 - voice * 9));
        dsp.Write(uint8_t(base + 1), uint8_t(voice & 1 ? -71 : 71));
        const unsigned pitch = 0x811 + voice * 0x721;
        dsp.Write(uint8_t(base + 2), uint8_t(pitch));
        dsp.Write(uint8_t(base + 3), uint8_t(pitch >> 8));
        dsp.Write(uint8_t(base + 4), uint8_t(voice));
        dsp.Write(uint8_t(base + 5), uint8_t(voice & 1 ? 0x8f : 0));
        dsp.Write(uint8_t(base + 6), uint8_t(0x53 + voice));
        dsp.Write(uint8_t(base + 7), uint8_t(voice & 2 ? 0xff : 0x7f));
    }
    dsp.Write(Dsp::kFlg, 0x1f);
    dsp.Write(Dsp::kKon, 0xff);
}

struct DspFixture {
    std::array<uint8_t, 65536> ram{};
    Dsp dsp;
    std::array<int16_t, 4096> samples{};

    explicit DspFixture(unsigned profile = 0) {
        Configure(dsp, ram.data(), profile);
        dsp.SetOutput(samples.data(), int(samples.size() / 2));
    }

    void SerializeState(StateArchive& archive) {
        archive.Array(ram);
        dsp.SerializeState(archive);
    }

    void Continue() {
        for (unsigned clock = 0; clock < 32 * 128 + 71; ++clock) {
            if (clock % 41 == 0)
                dsp.Write(Dsp::kEndx, 0xff);
            if (clock % 113 == 0)
                ram[0x1001 + (clock / 113) % 8] ^= 0x35;
            if (clock == 200)
                dsp.Write(Dsp::kKoff, 0x53);
            if (clock == 500)
                dsp.Write(Dsp::kKon, 0x29);
            if (clock % 317 == 0)
                dsp.Write(Dsp::kEdl, uint8_t((clock / 317) % 16));
            if (clock % 619 == 0)
                dsp.Write(Dsp::kFlg, uint8_t(clock % 3 == 0 ? 0x5f : 0x1f));
            dsp.Tick();
        }
    }
};

void EveryDspPhase() {
    for (unsigned profile = 0; profile < 4; ++profile)
        for (unsigned phase = 0; phase < 32; ++phase) {
            auto original = std::make_unique<DspFixture>(profile);
            auto restored = std::make_unique<DspFixture>(profile ^ 3);
            for (unsigned clock = 0; clock < 32 * 256 + phase; ++clock)
                original->dsp.Tick();
            Check(original->dsp.Phase() == phase, "The DSP checkpoint covers each pipeline phase");
            const auto saved = Capture(*original);
            restored->samples.fill(0x6666);
            const auto before = Capture(*restored);
            StateArchive archive(saved);
            restored->SerializeState(archive);
            Check(Capture(*restored) == before && restored->samples[0] == 0x6666,
                  "Reading DSP state leaves RAM, pipeline and attached output untouched");
            archive.Commit();
            const auto count = size_t(restored->dsp.SamplesWritten()) * 2;
            Check(Capture(*restored) == saved && restored->samples[count] == 0x6666,
                  "Restore replaces pending PCM without writing beyond its saved prefix");
            original->Continue();
            restored->Continue();
            Check(
                Capture(*original) == Capture(*restored) && original->ram == restored->ram &&
                    std::equal(original->samples.begin(),
                               original->samples.begin() + original->dsp.SamplesWritten() * 2,
                               restored->samples.begin()),
                "DSP continuation retains BRR, envelopes, modulation, noise, FIR echo and register "
                "latches");
            Check(std::any_of(original->samples.begin(),
                              original->samples.begin() + original->dsp.SamplesWritten() * 2,
                              [](int16_t value) { return value != 0; }),
                  "The checkpoint fixture generates audible samples");
        }
}

void InvalidDspState() {
    auto source = std::make_unique<DspFixture>();
    auto target = std::make_unique<DspFixture>(3);
    for (unsigned clock = 0; clock < 401; ++clock)
        source->dsp.Tick();
    target->samples.fill(0x3333);
    const auto valid = Capture(*source), untouched = Capture(*target);
    auto invalid = valid;
    invalid[invalid.size() - 4] = 32;
    StateArchive phase(invalid);
    Reject([&] { target->SerializeState(phase); }, "An out-of-range DSP phase is rejected");
    Reject([&] { phase.Commit(); }, "A rejected DSP phase cannot publish earlier PCM or RAM");
    Check(Capture(*target) == untouched && target->samples[0] == 0x3333,
          "Invalid DSP state preserves the old pipeline and output binding");
    std::array<int16_t, 2> small{0x1111, 0x2222};
    target->dsp.SetOutput(small.data(), 1);
    Reject([&] { Restore(*target, valid); }, "Pending DSP output must fit the attached buffer");
    Check(small[0] == 0x1111 && small[1] == 0x2222 && target->dsp.SamplesWritten() == 0,
          "An undersized output rejects before changing the caller's buffer");
    Dsp unattached;
    Check(!Capture(unattached).empty(), "A silent DSP can be captured without an output buffer");
}

struct SoundMachine {
    Smp smp;
    Dsp dsp;
    std::array<int16_t, 4096> samples{};

    SoundMachine(Smp::DspMode mode, unsigned waits) {
        smp.SetDspMode(mode);
        smp.SetDsp(dsp);
        Configure(dsp, smp.Ram(), 1);
        dsp.SetOutput(samples.data(), int(samples.size() / 2));
        smp.IoState().iplRomEnable = false;
        smp.IoState().internalWaitStates = uint8_t(waits);
        smp.IoState().externalWaitStates = uint8_t(3 - waits);
        smp.r.pc = 0x200;
        const uint8_t code[]{0x8f, 7,    0xf2, 0xe4, 0x20, 0xc4, 0xf3, 0xab, 0x20,
                             0x8f, 0x7c, 0xf2, 0xe4, 0xf3, 0xc4, 0x30, 0x2f, uint8_t(-18)};
        std::copy(std::begin(code), std::end(code), smp.Ram() + 0x200);
        smp.Ram()[0x20] = 0x70;
        smp.Ram()[0x30] = 0;
    }

    void SerializeState(StateArchive& archive) {
        smp.SerializeState(archive);
        dsp.SerializeState(archive);
    }
};

void CoupledSoundContinuation() {
    for (auto mode : {Smp::DspMode::PerCycle, Smp::DspMode::Batch32})
        for (unsigned waits = 0; waits < 4; ++waits)
            for (unsigned cut : {1u, 17u, 65u, 149u, 301u, 1703u}) {
                auto original = std::make_unique<SoundMachine>(mode, waits);
                auto restored = std::make_unique<SoundMachine>(mode, 3 - waits);
                original->smp.RunUntilHardwareClock(cut);
                original->smp.SynchronizeState();
                Restore(*restored, Capture(*original));
                const auto target = original->smp.HardwareClockCount() + 12000;
                for (auto* machine : {original.get(), restored.get()}) {
                    machine->smp.SynchronizeCpu(25000, 21477272);
                    machine->smp.RunUntilHardwareClock(target);
                    machine->smp.SynchronizeState();
                }
                Check(Capture(*original) == Capture(*restored),
                      "Coupled sound restore preserves both DSP modes, stretched access and clock "
                      "debt");
                Check(original->dsp.SamplesWritten() > 100,
                      "The resumed sound machine produces a sustained PCM sequence");
            }
}

struct MixerFixture {
    StreamMixer mixer;
    std::vector<std::array<float, 2>> samples;

    explicit MixerFixture(std::array<uint32_t, 3> rates) {
        mixer.SetOutput([this](float left, float right) { samples.push_back({left, right}); });
        for (unsigned index = 0; index < rates.size(); ++index)
            mixer.SetRate(StreamMixer::Input(index), rates[index]);
    }

    void Feed(unsigned begin, unsigned end) {
        for (unsigned index = begin; index < end; ++index)
            for (unsigned input = 0; input < 3; ++input)
                if (index % (3 + input * 2) == 0) {
                    const double left = double(int((index * 17 + input * 31) % 513) - 256) / 192;
                    const double right = double(int((index * 23 + input * 11) % 513) - 256) / 257;
                    mixer.Write(StreamMixer::Input(input), left, right);
                }
    }
};

void MixerContinuation() {
    constexpr std::array<std::array<uint32_t, 3>, 7> rates{{{32040, 32768, 44100},
                                                            {32000, 0, 44100},
                                                            {32040, 0, 0},
                                                            {32040, 32768, 0},
                                                            {0, 0, 44100},
                                                            {0, 0, 0},
                                                            {32040, 2097152, 44100}}};
    for (const auto& clocks : rates) {
        MixerFixture original(clocks), restored({44100, 32040, 32768});
        original.Feed(0, 5000);
        restored.Feed(0, 17);
        const auto saved = Capture(original.mixer), before = Capture(restored.mixer);
        const auto emitted = restored.samples.size();
        StateArchive archive(saved);
        restored.mixer.SerializeState(archive);
        Check(Capture(restored.mixer) == before && restored.samples.size() == emitted,
              "Reading mixer state leaves queues and the attached output unchanged");
        archive.Commit();
        Check(Capture(restored.mixer) == saved && restored.samples.size() == emitted,
              "Restore replaces rates, interpolation history and wrapped queues without emitting "
              "audio");
        original.samples.clear();
        restored.samples.clear();
        original.Feed(5000, 12000);
        restored.Feed(5000, 12000);
        Check(original.samples.size() == restored.samples.size() &&
                  Capture(original.mixer) == Capture(restored.mixer),
              "Resampling retains the number and phase of every independently clocked stream");
        for (size_t frame = 0; frame < original.samples.size(); ++frame)
            for (unsigned side = 0; side < 2; ++side)
                Check(std::bit_cast<uint32_t>(original.samples[frame][side]) ==
                          std::bit_cast<uint32_t>(restored.samples[frame][side]),
                      "Restored interpolation and final clipping produce identical PCM bits");
        if (std::any_of(clocks.begin(), clocks.end(), [](uint32_t rate) { return rate != 0; }))
            Check(!original.samples.empty(),
                  "Enabled streams continue through the retained output callback");
    }
}

void InvalidMixerState() {
    MixerFixture source({32040, 32768, 44100}), target({44100, 0, 0});
    source.Feed(0, 5000);
    target.Feed(0, 21);
    const auto valid = Capture(source.mixer), before = Capture(target.mixer);
    const auto outputs = target.samples.size();
    auto invalid = valid;
    // The first queue follows one rate and four stereo history frames.
    for (size_t index = 4 + 4 * 2 * 8; index < 4 + 4 * 2 * 8 + 4; ++index)
        invalid.at(index) = 0;
    StateArchive queue(invalid);
    Reject([&] { target.mixer.SerializeState(queue); },
           "An active stream requires its complete ring buffer");
    Reject([&] { queue.Commit(); }, "Invalid queue metadata cannot replace earlier stream state");
    invalid = valid;
    invalid.back() = 0x7f;
    invalid[invalid.size() - 2] = 0xf0;
    StateArchive phase(invalid);
    Reject([&] { target.mixer.SerializeState(phase); }, "A non-finite stream phase is rejected");
    Reject([&] { phase.Commit(); }, "A rejected final phase preserves every previous stream");
    Check(Capture(target.mixer) == before && target.samples.size() == outputs,
          "Malformed mixer state preserves queued audio and its callback");
}
} // namespace

int main() {
    try {
        EveryDspPhase();
        InvalidDspState();
        CoupledSoundContinuation();
        MixerContinuation();
        InvalidMixerState();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::puts("Audio state preserves DSP phases, pending PCM, sound clocks and resampling queues");
    return 0;
}
