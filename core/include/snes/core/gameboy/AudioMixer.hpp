// Align the adapter and console streams before stereo clipping.
#pragma once

#include "snes/core/audio/StreamMixer.hpp"

namespace snes::core::gameboy {

class AudioMixer {
public:
  using Output = audio::StreamMixer::Output;
  void SetOutput(Output output);
  void Reset(uint32_t handheldRate);
  // Every control write restarts the handheld stream, including unchanged rates.
  void SetHandheldRate(uint32_t rate);
  void WriteHandheld(int16_t left, int16_t right);
  void WriteConsole(int16_t left, int16_t right);
  void SetExpansionRate(uint32_t rate);
  void WriteExpansion(float left, float right);

private:
  audio::StreamMixer mixer_;
};

} // namespace snes::core::gameboy
