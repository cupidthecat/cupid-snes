#include "snes/core/detail/StateArchive.hpp"
#include "snes/core/gameboy/Audio.hpp"
#include "snes/core/gameboy/AudioMixer.hpp"
#include "snes/core/gameboy/SoundChannels.hpp"

namespace snes::core::gameboy {
void SoundChannels::SerializeState(detail::StateArchive &archive) {
  archive.Expect(secondModel_);
  archive.Array(registers_);
  archive.Array(wave_);
  unsigned index = 0;
  archive.Array(channels_, [&](auto &fields, auto &channel) {
    fields.Bounded(channel.timer, 0, 8200);
    fields.Bounded(channel.length, 0u, index == 2 ? 256u : 64u);
    fields.Bounded(channel.frequency, uint16_t{0}, uint16_t{2047});
    fields.Bounded(channel.position, uint8_t{0}, index == 2 ? uint8_t{31} : uint8_t{7});
    fields.Bounded(channel.volume, uint8_t{0}, uint8_t{15});
    fields.Bounded(channel.envelope, uint8_t{0}, uint8_t{7});
    fields.Bounded(channel.amplitude, uint8_t{0}, uint8_t{15});
    fields.Number(channel.enabled);
    fields.Number(channel.envelopeRunning);
    fields.Number(channel.high);
    fields.Number(channel.squareStarted);
    fields.Number(channel.envelopeClock);
    fields.Number(channel.envelopeLimit);
    fields.Number(channel.squareReloaded);
    fields.Number(channel.squareTicked);
    ++index;
  });
  archive.Object(emitted_, [](auto &fields, auto &levels) {
    for (auto &level : levels)
      fields.Bounded(level, uint8_t{0}, uint8_t{15});
  });
  const auto clocks = archive.Number(clocks_);
  const auto audio = archive.Number(audioClock_);
  archive.Validate(audio <= clocks && clocks - audio < 4, "Invalid handheld sound clock phase");
  archive.Number(noiseStart_);
  archive.Number(waveRead_);
  archive.Number(sweepStop_);
  archive.Number(dividerEvent_);
  archive.Number(secondaryEvent_);
  archive.Number(sweepTrigger_);
  archive.Bounded(noise_, uint16_t{0}, uint16_t{0x7fff});
  archive.Bounded(noiseCounter_, uint16_t{0}, uint16_t{0x3fff});
  archive.Bounded(sweepShadow_, uint16_t{0}, uint16_t{2047});
  archive.Bounded(noiseCountdown_, 0, 32);
  archive.Bounded(noiseReloadDelta_, 0, 2);
  archive.Bounded(phase_, uint8_t{0}, uint8_t{7});
  const auto shift = archive.Number(waveShift_);
  archive.Validate(shift <= 2 || shift == 4, "Invalid handheld wave volume shift");
  archive.Number(waveLatch_);
  archive.Bounded(emittedDacs_, uint8_t{0}, uint8_t{15});
  archive.Bounded(emittedChannels_, uint8_t{0}, uint8_t{15});
  archive.Bounded(sweepCounter_, uint8_t{0}, uint8_t{8});
  archive.Number(noiseAlignment_);
  archive.Bounded(startupEdges_, uint8_t{0}, uint8_t{2});
  archive.Number(powered_);
  archive.Number(dividerHigh_);
  archive.Number(sweepEnabled_);
  archive.Number(sweepNegated_);
  archive.Number(noisePending_);
  archive.Number(waveAccess_);
  archive.Number(noiseReloaded_);
  archive.Number(sweepStarting_);
  archive.Number(stopped_);
  archive.Validate(!archive.Number(deferTrigger_),
                   "Handheld sound state requires a completed operation");
}
void Audio::SerializeState(detail::StateArchive &archive) {
  archive.Array(filters_, [](auto &fields, auto &filter) {
    // Leave ample signal headroom while keeping invalid histories out of integer overflow.
    constexpr auto limit = int64_t{1} << 31;
    for (auto &side : filter.changes)
      for (auto &delta : side)
        fields.Bounded(delta, -limit, limit);
    for (auto &value : filter.value)
      fields.Bounded(value, -limit, limit);
    for (auto &input : filter.input)
      fields.Bounded(input, -136, 120);
    fields.Bounded(filter.signal.level, uint8_t{0}, uint8_t{15});
    fields.Number(filter.signal.dacEnabled);
    fields.Number(filter.signal.channelEnabled);
    fields.Bounded(filter.code, uint8_t{0}, uint8_t{16});
    fields.Bounded(filter.charge, 0.0, 1.0);
  });
  for (auto &capacitor : capacitor_)
    archive.Bounded(capacitor, -double(uint32_t{1} << 28), double(uint32_t{1} << 28));
  const auto clock = archive.Number(clock_);
  const auto render = archive.Number(renderClock_);
  archive.Number(samples_);
  const auto phaseClock = archive.Number(phaseClock_);
  archive.Validate(render <= clock && clock - render < 4 &&
                       (phaseClock <= render || phaseClock - render <= 4),
                   "Invalid handheld audio clock phase");
  archive.Bounded(sampleCycles_, 0u, 4295454u + 4 * 33558);
  const auto sampleRate = archive.Number(sampleRate_);
  const auto clockRate = archive.Number(clockRate_);
  archive.Validate((sampleRate == 33558 && clockRate == 4295454) ||
                       (sampleRate == 32768 && clockRate == 4194304),
                   "Invalid handheld audio rates");
  archive.Number(phaseStep_);
  archive.Number(phase_);
  const auto resume = archive.Number(resumeClocks_);
  archive.Validate(resume == 0 || resume == 4, "Invalid handheld sound resume phase");
  archive.Number(renderedClocks_);
  archive.Expect(batchClocks_);
  archive.Number(stopped_);
  archive.Number(volume_);
  archive.Number(routing_);
  archive.Bounded(highpass_, 0.0, 1.0);
  channels_.SerializeState(archive);
}
void AudioMixer::SerializeState(detail::StateArchive &archive) {
  mixer_.SerializeState(archive);
}
} // namespace snes::core::gameboy
