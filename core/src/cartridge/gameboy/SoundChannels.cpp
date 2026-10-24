#include "snes/core/gameboy/SoundChannels.hpp"

#include <algorithm>

namespace snes::core::gameboy {
namespace {
constexpr std::array<unsigned, 4> Base = {0, 5, 10, 15};
constexpr std::array<uint8_t, 23> ReadMask = {0x80, 0x3f, 0,    0xff, 0xbf, 0xff, 0x3f, 0,
                                              0xff, 0xbf, 0x7f, 0xff, 0x9f, 0xff, 0xbf, 0xff,
                                              0xff, 0,    0,    0xbf, 0,    0,    0x70};
} // namespace

void SoundChannels::Reset(bool secondModel, bool notifyOutput) {
    registers_.fill(0);
    wave_.fill(0);
    channels_.fill({});
    emitted_.fill(0);
    clocks_ = audioClock_ = noiseStart_ = waveRead_ = sweepStop_ = dividerEvent_ = 0;
    sweepTrigger_ = secondaryEvent_ = 0;
    sweepStarting_ = false;
    noise_ = 0x7fff;
    noiseCounter_ = noiseAlignment_ = 0;
    noiseCountdown_ = noiseReloadDelta_ = 0;
    noiseReloaded_ = false;
    sweepShadow_ = 0;
    phase_ = waveShift_ = waveLatch_ = emittedDacs_ = sweepCounter_ = 0;
    startupEdges_ = 0;
    emittedChannels_ = 0;
    powered_ = dividerHigh_ = sweepEnabled_ = sweepNegated_ = false;
    noisePending_ = waveAccess_ = false;
    stopped_ = false;
    deferTrigger_ = false;
    secondModel_ = secondModel;
    if (notifyOutput) {
        for (unsigned i = 0; i < 4; ++i)
            output_.Dac(0, i, {}, 0);
        output_.Mixer(0, 0, 0);
    }
}

uint16_t SoundChannels::Frequency(unsigned channel) const noexcept {
    if (channel >= 3)
        return uint16_t((registers_[18] & 7) << (registers_[18] >> 4));
    return channels_[channel].frequency;
}

bool SoundChannels::DacEnabled(unsigned channel) const noexcept {
    return powered_ && (channel == 2 ? (registers_[10] & 0x80) != 0
                                     : (registers_[Base[channel] + 2] & 0xf8) != 0);
}

bool SoundChannels::LengthEnabled(unsigned channel) const noexcept {
    return (registers_[Base[channel] + 4] & 0x40) != 0;
}

void SoundChannels::Suspend(unsigned clocks) noexcept {
    clocks_ += clocks;
    audioClock_ += clocks;
    if (noisePending_)
        noiseStart_ += clocks;
    if (sweepStop_)
        sweepStop_ += clocks;
    if (waveAccess_)
        waveRead_ += clocks;
    if (dividerEvent_)
        dividerEvent_ += clocks;
    if (secondaryEvent_)
        secondaryEvent_ += clocks;
    if (sweepStarting_)
        sweepTrigger_ += clocks;
}

bool SoundChannels::NeedsStep() const noexcept {
    return channels_[2].enabled || sweepStop_ ||
           (sweepStarting_ && audioClock_ - sweepTrigger_ <
                                  ((registers_[0] & 7) ? 4u * (3u + (registers_[0] & 7)) : 4u));
}

SoundChannels::State SoundChannels::GetState() const noexcept {
    State state;
    for (unsigned i = 0; i < 4; ++i) {
        state.amplitudes[i] = channels_[i].enabled ? channels_[i].amplitude : 0;
        state.volumes[i] = i == 2 ? uint8_t(waveShift_ == 4   ? 0
                                            : waveShift_ == 0 ? 15
                                                              : 16 >> waveShift_)
                                  : channels_[i].volume;
        state.frequencies[i] = Frequency(i);
    }
    return state;
}

uint8_t SoundChannels::Read(uint16_t address) const {
    if (address >= 0xff30 && address <= 0xff3f) {
        if (channels_[2].enabled)
            return waveAccess_ && audioClock_ - waveRead_ < 2 ? wave_[channels_[2].position / 2]
                                                              : 0xff;
        return wave_[address & 15];
    }
    if (address < 0xff10 || address > 0xff26)
        return 0xff;
    const unsigned reg = address - 0xff10;
    if (reg == 22) {
        uint8_t value = powered_ ? 0xf0 : 0x70;
        for (unsigned i = 0; i < 4; ++i)
            value |= channels_[i].enabled ? uint8_t(1u << i) : 0;
        return value;
    }
    return registers_[reg] | ReadMask[reg];
}

void SoundChannels::Emit(uint64_t clock, int phaseOffset) {
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t value = channels_[i].amplitude;
        const uint8_t mask = uint8_t(1u << i);
        const bool dac = DacEnabled(i);
        const bool active = channels_[i].enabled;
        if (value == emitted_[i] && dac == ((emittedDacs_ & mask) != 0) &&
            active == ((emittedChannels_ & mask) != 0))
            continue;
        emitted_[i] = value;
        emittedDacs_ = uint8_t((emittedDacs_ & ~mask) | (dac ? mask : 0));
        emittedChannels_ = uint8_t((emittedChannels_ & ~mask) | (active ? mask : 0));
        output_.Dac(clock, i, {value, dac, active}, phaseOffset);
    }
}

void SoundChannels::Disable(unsigned channel, bool clearSignal) {
    channels_[channel].enabled = false;
    if (clearSignal)
        channels_[channel].amplitude = 0;
}

void SoundChannels::Power(bool enabled) {
    if (powered_ == enabled)
        return;
    powered_ = enabled;
    registers_.fill(0);
    for (auto& channel : channels_) {
        const auto length = channel.length;
        channel = {};
        channel.length = length;
    }
    startupEdges_ = enabled && dividerHigh_ ? 2 : 0;
    phase_ = startupEdges_ ? 1 : 0;
    waveShift_ = enabled ? 4 : 0;
    waveLatch_ = 0;
    noise_ = 0x7fff;
    noiseCounter_ = noiseAlignment_ = 0;
    noiseCountdown_ = noiseReloadDelta_ = 0;
    noiseReloaded_ = false;
    sweepShadow_ = sweepCounter_ = 0;
    sweepStop_ = 0;
    sweepTrigger_ = 0;
    sweepStarting_ = false;
    sweepEnabled_ = sweepNegated_ = noisePending_ = waveAccess_ = false;
    Emit(audioClock_);
    output_.Power(audioClock_, enabled);
}

bool SoundChannels::Duty(unsigned channel) const noexcept {
    const auto position = channels_[channel].position;
    switch (registers_[Base[channel] + 1] >> 6) {
    case 0:
        return position == 7;
    case 1:
        return position == 7 || position == 0;
    case 2:
        return position >= 5 || position == 0;
    default:
        return position >= 1 && position <= 6;
    }
}

void SoundChannels::UpdateAmplitude(unsigned channel) {
    auto& state = channels_[channel];
    if (channel < 2) {
        state.amplitude = state.squareStarted && Duty(channel) ? state.volume : 0;
    } else if (channel == 2) {
        const auto nibble = state.position & 1 ? waveLatch_ & 15 : waveLatch_ >> 4;
        state.amplitude = uint8_t(nibble >> waveShift_);
    } else {
        state.amplitude = state.high ? state.volume : 0;
    }
}

void SoundChannels::PulseNoise(bool narrow) {
    const unsigned feedback = (noise_ ^ (noise_ >> 1)) & 1;
    noise_ = uint16_t((noise_ >> 1) | (feedback << 14));
    if (narrow)
        noise_ = uint16_t((noise_ & ~0x40) | (feedback << 6));
    channels_[3].high = !(noise_ & 1);
    if (channels_[3].enabled)
        UpdateAmplitude(3);
}

uint16_t SoundChannels::NoiseTaps() const noexcept {
    uint16_t taps = uint16_t(noiseCounter_ | (noiseCounter_ >> 8));
    if (noiseCounter_ & 0x80)
        taps |= 0xff;
    if (noiseCounter_ & 8)
        taps |= 6;
    if (noiseCounter_ & 0x800)
        taps |= 0x400;
    return taps;
}

void SoundChannels::ClockNoise() {
    ++noiseAlignment_;
    noiseReloaded_ = false;
    const unsigned divider = registers_[18] & 7;
    const int reload = divider ? int(4 * divider) : 2;
    if (noiseCountdown_ == 0)
        noiseCountdown_ = reload;
    if (--noiseCountdown_ != 0)
        return;
    noiseCountdown_ = reload + noiseReloadDelta_;
    noiseReloadDelta_ = 0;
    noiseReloaded_ = true;
    const auto previous = noiseCounter_;
    noiseCounter_ = uint16_t((noiseCounter_ + 1) & 0x3fff);
    const unsigned shift = registers_[18] >> 4;
    if (channels_[3].enabled && shift < 14 && ((~previous & noiseCounter_) & (1u << shift)))
        PulseNoise(registers_[18] & 8);
}

int SoundChannels::SweepResult() {
    const auto control = registers_[0];
    const int delta = sweepShadow_ >> (control & 7);
    if (control & 8) {
        sweepNegated_ = true;
        return int(sweepShadow_) - delta;
    }
    return int(sweepShadow_) + delta;
}

void SoundChannels::Trigger(unsigned channel) {
    auto& state = channels_[channel];
    const bool wasEnabled = state.enabled;
    state.enabled = DacEnabled(channel);
    if (!wasEnabled && channel != 2)
        state.high = false;
    if (state.length == 0) {
        state.length = channel == 2 ? 256 : 64;
        if (LengthEnabled(channel) && (phase_ & 1))
            --state.length;
    }
    state.envelope = registers_[Base[channel] + 2] & 7;
    state.envelopeClock = state.envelopeLimit = false;
    state.envelopeRunning = true;
    if (channel != 2)
        state.volume = registers_[Base[channel] + 2] >> 4;
    if (channel < 2) {
        state.squareTicked = false;
        if (!wasEnabled)
            state.squareStarted = false;
        state.timer = int(4 * (2048 - Frequency(channel)) + (wasEnabled ? 4 : 8));
    } else if (channel == 2) {
        if (wasEnabled && state.timer == 2) {
            const unsigned nextByte = ((state.position + 1) & 31) / 2;
            if (nextByte < 4)
                wave_[0] = wave_[nextByte];
            else {
                const unsigned first = nextByte & ~3u;
                for (unsigned i = 0; i < 4; ++i)
                    wave_[i] = wave_[first + i];
            }
            waveLatch_ = wave_[0];
        }
        state.position = 0;
        state.timer = int(2 * (2048 - Frequency(channel)) + 6);
    } else {
        noise_ = 0x7fff;
        const unsigned divider = registers_[18] & 7;
        noiseCountdown_ = divider ? int(4 * divider + (divider == 1 ? 2 : 0)) : 4;
        noiseReloadDelta_ = divider == 1 ? 2 : 0;
        noiseReloaded_ = false;
    }
    UpdateAmplitude(channel);
    if (channel == 3)
        state.high = false;
    if (channel == 0) {
        sweepShadow_ = registers_[0] & 0x77 ? Frequency(0) : 0;
        sweepStarting_ = true;
        sweepTrigger_ = audioClock_;
        sweepCounter_ = (registers_[0] >> 4) & 7;
        if (sweepCounter_ == 0)
            sweepCounter_ = 8;
        sweepEnabled_ = (registers_[0] & 0x77) != 0;
        sweepNegated_ = false;
        sweepStop_ = 0;
        if ((registers_[0] & 7) && SweepResult() > 2047)
            sweepStop_ = audioClock_ + 12 + 4 * (registers_[0] & 7);
    }
}

void SoundChannels::SetControl(unsigned channel, uint8_t previous, uint8_t value) {
    auto& state = channels_[channel];
    if (!(previous & 0x40) && (value & 0x40) && (phase_ & 1) && state.length != 0) {
        if (--state.length == 0)
            Disable(channel);
    }
    if (!(value & 0x80))
        return;
    if (channel == 3) {
        state.envelopeClock = false;
        state.envelopeRunning = true;
    }
    if (channel == 3 && (noiseAlignment_ & 3)) {
        noisePending_ = true;
        noiseStart_ = audioClock_ + 12;
    } else {
        Trigger(channel);
    }
}

void SoundChannels::Write(uint16_t address, uint8_t value, uint16_t programCounter) {
    if (address >= 0xff30 && address <= 0xff3f) {
        if (!channels_[2].enabled)
            wave_[address & 15] = value;
        else if (waveAccess_ && audioClock_ - waveRead_ < 2)
            wave_[channels_[2].position / 2] = value;
        return;
    }
    if (address < 0xff10 || address > 0xff26)
        return;
    const unsigned reg = address - 0xff10;
    if (reg == 22) {
        Power(value & 0x80);
        return;
    }
    if (reg == 1 || reg == 6 || reg == 11 || reg == 16) {
        const unsigned channel = reg == 1 ? 0 : reg == 6 ? 1 : reg == 11 ? 2 : 3;
        channels_[channel].length = channel == 2 ? 256 - value : 64 - (value & 63);
    }
    if (!powered_ || reg == 5 || reg == 15)
        return;
    const auto previous = registers_[reg];
    registers_[reg] = value;
    switch (reg) {
    case 0:
        registers_[reg] &= 0x7f;
        if ((previous & 8) && !(value & 8) && sweepNegated_)
            Disable(0);
        if (sweepStarting_ && !sweepEnabled_ && (value & 0x70)) {
            if (value & 7)
                channels_[0].frequency = uint16_t(value & 8 ? 1 : 0);
            else if (!(value & 8) && audioClock_ > sweepTrigger_)
                sweepStop_ = audioClock_ + 8;
        }
        break;
    case 1:
    case 6:
        registers_[reg] &= 0xc0;
        break;
    case 2:
    case 7:
    case 17: {
        const unsigned channel = reg == 2 ? 0 : reg == 7 ? 1 : 3;
        auto& state = channels_[channel];
        if (!DacEnabled(channel)) {
            Disable(channel, false);
        } else if (state.enabled) {
            if (state.envelopeClock)
                state.envelope = value & 7;
            unsigned volume = state.volume;
            if (!(previous & 7) && state.envelopeRunning)
                ++volume;
            else if (!(previous & 8))
                volume += 2;
            if ((previous ^ value) & 8)
                volume = 16 - volume;
            state.volume = uint8_t(volume & 15);
            UpdateAmplitude(channel);
            if (!(value & 7) && state.envelopeClock) {
                state.envelopeClock = false;
                state.envelopeRunning &= !state.envelopeLimit;
            }
        }
        break;
    }
    case 4:
    case 9:
    case 14:
    case 19: {
        registers_[reg] &= reg == 19 ? 0x40 : 0x47;
        if (reg != 19) {
            auto& state = channels_[reg / 5];
            auto& frequency = state.frequency;
            if (reg != 14 && state.enabled && state.squareTicked && !(value & 0x80) &&
                (previous & 7) == 7 && (value & 7) != 7 &&
                state.timer == int(4 * (2048 - frequency)))
                state.position = uint8_t((state.position + 7) & 7);
            frequency = uint16_t((frequency & 0xff) | ((value & 7) << 8));
            if (reg != 14 && state.squareReloaded)
                state.timer = int(4 * (2048 - frequency));
        }
        SetControl(reg / 5, previous, value);
        break;
    }
    case 3:
    case 8:
    case 13: {
        auto& state = channels_[reg / 5];
        auto& frequency = state.frequency;
        frequency = uint16_t((frequency & 0x700) | value);
        if (reg != 13 && state.squareReloaded)
            state.timer = int(4 * (2048 - frequency));
        break;
    }
    case 10:
        registers_[reg] &= 0x80;
        if (!DacEnabled(2)) {
            if (channels_[2].enabled) {
                if (channels_[2].timer == 2)
                    waveLatch_ = wave_[programCounter & 15];
                else if (waveAccess_ && audioClock_ - waveRead_ < 2)
                    waveLatch_ = wave_[address & 15];
            }
            Disable(2, false);
        }
        break;
    case 12:
        registers_[reg] &= 0x60;
        waveShift_ = value & 0x60 ? uint8_t(((value >> 5) & 3) - 1) : 4;
        if (channels_[2].enabled)
            UpdateAmplitude(2);
        break;
    case 20:
    case 21:
        output_.Mixer(audioClock_, registers_[20], registers_[21]);
        break;
    case 18:
        if (noiseReloaded_) {
            const unsigned divider = value & 7;
            noiseCountdown_ = divider ? int(4 * divider + 4 - ((noiseAlignment_ + 2) & 3)) : 2;
            noiseReloadDelta_ = 0;
        }
        if (NoiseTaps() & (1u << (value >> 4)))
            PulseNoise(true);
        break;
    default:
        break;
    }
    Emit(audioClock_);
}

void SoundChannels::Lengths() {
    for (unsigned i = 0; i < 4; ++i) {
        auto& state = channels_[i];
        if (LengthEnabled(i) && state.length && --state.length == 0)
            Disable(i);
    }
}

void SoundChannels::Envelopes(bool rising) {
    for (unsigned i : {0u, 1u, 3u}) {
        auto& state = channels_[i];
        const auto control = registers_[Base[i] + 2];
        if (rising) {
            if (!state.enabled || state.envelope != 0)
                continue;
            state.envelope = control & 7;
            if (state.envelope && !state.envelopeClock) {
                state.envelopeClock = true;
                state.envelopeLimit = state.volume == ((control & 8) ? 15 : 0);
            } else if (!state.envelope && state.envelopeClock) {
                state.envelopeClock = false;
                state.envelopeRunning &= !state.envelopeLimit;
            }
        } else if (state.envelopeClock) {
            state.envelopeClock = false;
            state.envelopeRunning &= !state.envelopeLimit;
            if (state.envelopeRunning && (control & 7)) {
                state.volume = uint8_t(int(state.volume) + ((control & 8) ? 1 : -1));
                if (state.enabled)
                    UpdateAmplitude(i);
            }
        }
    }
}

void SoundChannels::Sweep() {
    sweepStarting_ = false;
    if (sweepCounter_ == 0 || --sweepCounter_ != 0)
        return;
    sweepCounter_ = (registers_[0] >> 4) & 7;
    if (sweepCounter_ == 0)
        sweepCounter_ = 8;
    if (!sweepEnabled_ || !(registers_[0] & 0x70))
        return;
    const unsigned shift = registers_[0] & 7;
    if (shift != 0) {
        sweepShadow_ = uint16_t(SweepResult() & 2047);
        channels_[0].frequency = sweepShadow_;
        registers_[3] = uint8_t(sweepShadow_);
        registers_[4] = uint8_t((registers_[4] & 0x40) | (sweepShadow_ >> 8));
    }
    if (SweepResult() > 2047)
        sweepStop_ = audioClock_ + 4 + 4 * shift;
}

void SoundChannels::ClockFrame(int phaseOffset) {
    if (!powered_)
        return;
    if (startupEdges_) {
        if (--startupEdges_)
            return;
        phase_ = 0;
    }
    if (phase_ == 6)
        for (unsigned channel : {0u, 1u, 3u}) {
            auto& state = channels_[channel];
            if (!state.envelopeClock)
                state.envelope = uint8_t((state.envelope - 1) & 7);
        }
    Envelopes(false);
    if ((phase_ & 1) == 0)
        Lengths();
    if (phase_ == 2 || phase_ == 6)
        Sweep();
    phase_ = uint8_t((phase_ + 1) & 7);
    Emit(audioClock_, phaseOffset);
}

void SoundChannels::DividerEdge(bool rising, uint64_t clock) {
    if (noisePending_ && clock > clocks_) {
        dividerHigh_ = rising;
        AdvanceTo(clock - 4);
        if (powered_) {
            if (rising)
                Envelopes(true);
            else
                ClockFrame();
        }
        return;
    }
    const bool immediate = clock <= clocks_;
    dividerHigh_ = rising;
    if (powered_ && !rising && clock > clocks_)
        dividerEvent_ = clock;
    if (powered_ && rising && clock > clocks_)
        secondaryEvent_ = clock;
    AdvanceTo(clock);
    if (immediate && powered_) {
        if (rising)
            Envelopes(true);
        else
            ClockFrame();
    }
}

void SoundChannels::Tick(uint64_t windowBegin, uint64_t windowEnd) {
    if (stopped_) {
        if (noisePending_)
            noiseStart_ += 4;
        if (sweepStop_)
            sweepStop_ += 4;
        if (waveAccess_)
            waveRead_ += 4;
        return;
    }
    if (!powered_)
        return;
    for (unsigned half = 0; half < 2; ++half) {
        const uint64_t end = audioClock_ - 2 + half * 2;
        if (sweepStop_ && end >= sweepStop_) {
            Disable(0);
            sweepStop_ = 0;
            // Keep the filter phase independent of the tick that detects completion.
            Emit(end, -int(end - windowBegin) - int(windowEnd - windowBegin));
        }
        if (dividerEvent_ && end >= dividerEvent_) {
            dividerEvent_ = 0;
            ClockFrame(-4);
        }
        if (secondaryEvent_ && end >= secondaryEvent_) {
            secondaryEvent_ = 0;
            Envelopes(true);
        }
        ClockNoise();
        if (noisePending_ && end >= noiseStart_ && !deferTrigger_) {
            noisePending_ = false;
            Trigger(3);
        }
        Emit(end);
        for (unsigned i = 0; i < 3; ++i) {
            auto& state = channels_[i];
            state.squareReloaded = false;
            if (!state.enabled)
                continue;
            state.timer -= 2;
            while (state.timer <= 0) {
                const auto edge = uint64_t(int64_t(end) + state.timer);
                if (i < 2) {
                    state.squareReloaded = true;
                    state.squareTicked = true;
                    state.timer += int(4 * (2048 - Frequency(i)));
                    state.position = uint8_t((state.position + 1) & 7);
                    state.squareStarted = true;
                } else if (i == 2) {
                    state.timer += int(2 * (2048 - Frequency(2)));
                    state.position = uint8_t((state.position + 1) & 31);
                    waveLatch_ = wave_[state.position / 2];
                    waveRead_ = edge;
                    waveAccess_ = true;
                }
                UpdateAmplitude(i);
                Emit(edge);
            }
        }
    }
    Emit(audioClock_);
}

void SoundChannels::Advance(unsigned clocks) {
    AdvanceTo(clocks_ + clocks);
}

void SoundChannels::AdvanceTo(uint64_t clock, bool deferTrigger) {
    if (!deferTrigger)
        CommitPendingTrigger();
    deferTrigger_ = deferTrigger;
    const auto begin = clocks_;
    clocks_ = std::max(clocks_, clock);
    while (audioClock_ + 4 <= clocks_) {
        audioClock_ += 4;
        Tick(begin, clocks_);
    }
    deferTrigger_ = false;
}

void SoundChannels::CommitPendingTrigger() {
    if (noisePending_ && audioClock_ >= noiseStart_) {
        noisePending_ = false;
        Trigger(3);
        Emit(audioClock_);
    }
}

} // namespace snes::core::gameboy
