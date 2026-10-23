#include "snes/core/gameboy/AudioMixer.hpp"

#include <utility>

namespace snes::core::gameboy {
namespace {
using Input = audio::StreamMixer::Input;
}

void AudioMixer::SetOutput(Output output) {
    mixer_.SetOutput(std::move(output));
}

void AudioMixer::Reset(uint32_t handheldRate) {
    mixer_.Reset();
    mixer_.SetRate(Input::Console, handheldRate ? AudioTiming::SampleRate : 0);
    mixer_.SetRate(Input::Handheld, handheldRate);
}

void AudioMixer::SetHandheldRate(uint32_t rate) {
    mixer_.SetRate(Input::Handheld, rate);
}

void AudioMixer::WriteHandheld(int16_t left, int16_t right) {
    mixer_.Write(Input::Handheld, double(left) / 32768, double(right) / 32768);
}

void AudioMixer::WriteConsole(int16_t left, int16_t right) {
    mixer_.Write(Input::Console, double(left) / 32768, double(right) / 32768);
}

void AudioMixer::SetConsoleRate(uint32_t rate) {
    mixer_.SetRate(Input::Console, rate);
}

void AudioMixer::SetExpansionRate(uint32_t rate) {
    mixer_.SetRate(Input::Expansion, rate);
}

void AudioMixer::WriteExpansion(float left, float right) {
    mixer_.Write(Input::Expansion, left, right);
}

} // namespace snes::core::gameboy
