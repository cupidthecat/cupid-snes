// Regression coverage for SDL audio buffering.

#include "SdlAudioOutput.hpp"

#include <cstdio>
#include <exception>

using snes::core::AudioBuffer;
using snes::frontend::SdlAudioOutput;

namespace {
int failures = 0;
void Expect(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

AudioBuffer Tone(int frames) {
  AudioBuffer buffer;
  buffer.sampleRate = snes::core::AudioTiming::SampleRate;
  for (int i = 0; i < frames; ++i) {
    const float sample = (i % 32 < 16) ? 0.25f : -0.25f;
    buffer.interleavedStereo.push_back(sample);
    buffer.interleavedStereo.push_back(-sample);
  }
  return buffer;
}

bool WaitForDrain(SdlAudioOutput &audio) {
  const auto deadline = SDL_GetTicks() + 2000;
  do {
    if (!audio.HasPendingOutput() && audio.QueuedBytes() < 32 * 2 * sizeof(float))
      return true;
    SDL_Delay(1);
  } while (SDL_GetTicks() < deadline);
  std::fprintf(stderr, "Audio did not drain: queued=%d, pending=%d, playing=%d\n",
               audio.QueuedBytes(), audio.HasPendingOutput(), audio.IsPlaying());
  return false;
}

void QueuePreservesInput() {
  SdlAudioOutput audio;
  Expect(audio.NeedsSamples(), "an empty queue requests an emulated frame");
  constexpr int primeFrames = 1923;
  audio.Submit(Tone(primeFrames - 1));
  Expect(audio.NeedsSamples(), "a queue below 60 ms requests more input");
  audio.Submit(Tone(1));
  Expect(!audio.NeedsSamples(), "60 ms of queued input stops frame production");
  audio.Submit(Tone(1920));
  Expect(audio.QueuedBytes() == (primeFrames + 1920) * 2 * sizeof(float),
         "queue size uses input bytes even when the device resamples");
  audio.Submit(Tone(532));
  Expect(audio.QueuedBytes() == (primeFrames + 1920 + 532) * 2 * sizeof(float),
         "a full queue preserves the next audio block");
}

void PlaybackPrimesAndRecovers() {
  SdlAudioOutput audio;
  audio.Resume();
  audio.Submit(Tone(320)); // 10 ms is too little to start playback.
  const int initial = audio.QueuedBytes();
  SDL_Delay(80);
  Expect(audio.QueuedBytes() == initial, "startup waits for a buffer before playing");
  audio.Submit(Tone(1920));
  Expect(WaitForDrain(audio), "a primed stream drains down to resampler padding");
  int tail = audio.QueuedBytes();
  audio.Submit(Tone(320));
  SDL_Delay(80);
  Expect(audio.QueuedBytes() == tail + 320 * 2 * sizeof(float),
         "an underrun refills the buffer before restarting");
  audio.Submit(Tone(1920));
  Expect(WaitForDrain(audio), "playback resumes after underrun recovery");
  audio.Pause();
  tail = audio.QueuedBytes();
  audio.Submit(Tone(2240));
  SDL_Delay(80);
  Expect(audio.QueuedBytes() == tail + 2240 * 2 * sizeof(float),
         "submitting a full buffer does not override an explicit pause");
  audio.Resume();
  Expect(WaitForDrain(audio), "explicit resume plays a queued buffer");
}

void GameChangesDiscardAudio() {
  SdlAudioOutput audio;
  audio.Submit(Tone(2400));
  Expect(audio.QueuedBytes() > 0, "the previous game has queued audio");
  audio.Clear();
  Expect(!audio.IsPlaying() && audio.QueuedBytes() == 0 && !audio.HasPendingOutput(),
         "changing games discards queued input and resampler output");
  audio.Submit(Tone(2400));
  const auto queued = audio.QueuedBytes();
  SDL_Delay(30);
  Expect(audio.QueuedBytes() == queued, "new game audio waits for an explicit resume");
  audio.Resume();
  Expect(WaitForDrain(audio), "new game audio primes and plays after resume");
}

void MutedPlaybackKeepsPacing() {
  SdlAudioOutput::Config config;
  config.deviceName = "A missing playback device";
  SdlAudioOutput audio(config);
  auto buffer = Tone(2400);
  const auto original = buffer.interleavedStereo;
  audio.SetVolume(175, true);
  audio.Resume();
  audio.Submit(buffer);
  Expect(WaitForDrain(audio), "muted playback still drains and paces emulation");
  Expect(buffer.interleavedStereo == original,
         "host volume and mute preserve generated audio samples");
  audio.SetVolume(175);
  audio.Submit(buffer);
  Expect(WaitForDrain(audio), "unmuting preserves the running audio stream");
}
} // namespace

int main(int argc, char **argv) {
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  SDL_SetHint(SDL_HINT_AUDIO_FREQUENCY, argc > 1 ? argv[1] : "48000");
  if (!SDL_Init(SDL_INIT_AUDIO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  try {
    QueuePreservesInput();
    PlaybackPrimesAndRecovers();
    GameChangesDiscardAudio();
    MutedPlaybackKeepsPacing();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    ++failures;
  }
  SDL_Quit();
  return failures ? 1 : 0;
}
