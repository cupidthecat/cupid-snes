// Stereo sound rendering at the adapter's native sample rate.
#pragma once

#include "snes/core/gameboy/SoundChannels.hpp"

#include <array>
#include <cstdint>

namespace snes::core::detail {
class StateArchive;
}

namespace snes::core {
class MemoryInspector;
}

namespace snes::core::gameboy {

class Audio final : private SoundChannels::Output {
    friend class ::snes::core::MemoryInspector;

  public:
    void SerializeState(detail::StateArchive& archive);

    struct Output {
        virtual ~Output() = default;
        virtual void Sample(uint64_t clock, int16_t left, int16_t right) = 0;
    };

    explicit Audio(Output& output) : output_(output) {
    }

    // Processor resets can retain the sample clock and analog output history.
    void Reset(bool secondModel = false, bool preserveOutput = false);
    uint8_t Read(uint16_t address);
    void Write(uint16_t address, uint8_t value, uint16_t programCounter = 0);
    void Advance(unsigned clocks);
    // Complete an elapsed bus timing batch, including any sample due in it.
    void AdvanceTo(uint64_t clock);
    void DividerEdge(bool rising, uint64_t clock);
    void Stop(bool stopped) noexcept;

    unsigned SampleRate() const noexcept {
        return sampleRate_;
    }

  private:
    struct Filter {
        std::array<std::array<int64_t, 128>, 2> changes{};
        std::array<int64_t, 2> value{};
        std::array<int, 2> input{};
        SoundChannels::Signal signal{};
        uint8_t code = 0;
        double charge = 0;
    };

    Output& output_;
    std::array<Filter, 4> filters_{};
    std::array<double, 2> capacitor_{};
    uint64_t clock_ = 0, renderClock_ = 0, samples_ = 0, phaseClock_ = 0;
    unsigned sampleCycles_ = 0, sampleRate_ = 33558, clockRate_ = 4295454;
    uint32_t phaseStep_ = 4194275, phase_ = 0;
    unsigned resumeClocks_ = 0, renderedClocks_ = 0, batchClocks_ = 128;
    bool stopped_ = false;
    uint8_t volume_ = 0, routing_ = 0;
    double highpass_ = 1;
    SoundChannels channels_{*this};

    void Dac(uint64_t clock, unsigned channel, SoundChannels::Signal signal,
             int phaseOffset) override;
    void Mixer(uint64_t clock, uint8_t volume, uint8_t routing) override;
    void Power(uint64_t clock, bool enabled) override;
    void Change(uint64_t clock, unsigned channel, unsigned level, int phaseOffset);
    void AccumulateClocks(uint64_t clock);
    void AdvancePhase(uint64_t clock);
    void FlushSound(uint64_t clock, bool deferTrigger = false);
    void CommitSamples();
    void Render();
};

} // namespace snes::core::gameboy
