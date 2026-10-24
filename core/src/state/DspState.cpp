#include "snes/core/Dsp.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void Dsp::SerializeState(detail::StateArchive &archive) {
  archive.Array(regs_);
  for (auto &voice : voices_)
    archive.Object(voice, [](auto &fields, auto &next) {
      for (auto &sample : next.buf)
        fields.Bounded(sample, -32768, 32767);
      const auto position = fields.Bounded(next.bufPos, 0, BrrBufSize - 4);
      fields.Validate(position % 4 == 0, "Invalid DSP sample buffer position");
      fields.Bounded(next.interpPos, 0, 0x7fff);
      fields.Bounded(next.brrAddr, 0, 0xffff);
      const auto offset = fields.Bounded(next.brrOffset, 1, BrrBlockSize - 2);
      fields.Validate((offset & 1) != 0, "Invalid DSP sample block position");
      fields.Bounded(next.konDelay, 0, 5);
      fields.Enumeration(next.envMode, Release, Sustain);
      fields.Bounded(next.env, 0, 0x7ff);
      fields.Bounded(next.hiddenEnv, -32, 0xbff);
      fields.Bounded(next.output, -32768, 32767);
      fields.Bounded(next.envSnapshot, uint8_t{0}, uint8_t{127});
    });
  archive.Number(newKon_);
  archive.Number(kon_);
  archive.Bounded(everyOther_, 0, 1);
  archive.Bounded(noise_, 0, 0x7fff);
  archive.Bounded(counter_, 0, 30719);
  for (auto &frame : echoHist_)
    for (auto &sample : frame)
      archive.Bounded(sample, -16384, 16383);
  archive.Bounded(echoHistIdx_, 0, EchoHistSize - 1);
  const auto offset = archive.Bounded(echoOffset_, 0, 15 * 0x800 - 4);
  const auto length = archive.Bounded(echoLength_, 0, 15 * 0x800);
  archive.Validate(offset % 4 == 0 && length % 0x800 == 0 &&
                       (length ? offset < length : offset == 0),
                   "Invalid DSP echo buffer position");
  archive.Object(pipe_, [](auto &fields, auto &next) {
    fields.Number(next.pitchMod);
    fields.Number(next.noiseEnable);
    fields.Number(next.echoEnable);
    fields.Number(next.directoryPage);
    fields.Number(next.keyOff);
    fields.Number(next.sourceNumber);
    fields.Number(next.echoPage);
    fields.Number(next.echoFlags);
    fields.Number(next.directoryAddress);
    fields.Number(next.nextBrrAddress);
    fields.Number(next.echoAddress);
    fields.Bounded(next.adsr0, 0, 255);
    fields.Bounded(next.pitch, 0, 0x7fff);
    fields.Bounded(next.brrHeader, 0, 255);
    fields.Bounded(next.brrByte, 0, 255);
    fields.Bounded(next.output, -32768, 32767);
    fields.Bounded(next.looped, 0, 128);
    for (auto &sample : next.mainMix)
      fields.Bounded(sample, -32768, 32767);
    for (auto &sample : next.echoMix)
      fields.Bounded(sample, -32768, 32767);
    for (auto &sample : next.filteredEcho)
      fields.Bounded(sample, -6 * 32768, 6 * 32768);
  });
  archive.Number(endxBuf_);
  archive.Number(envxBuf_);
  archive.Number(outxBuf_);
  const auto samples = archive.Bounded(samplesWritten_, 0, std::max(outBufSize_, 0));
  archive.Validate(samples == 0 || outBuf_, "DSP state requires an attached sample buffer");
  archive.Fixed(std::span<int16_t>(outBuf_, size_t(samples) * 2));
  archive.Bounded(phase_, 0u, 31u);
}
} // namespace snes::core
