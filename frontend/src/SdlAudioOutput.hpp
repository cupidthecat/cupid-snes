// SDL3 audio output interface.

#pragma once
// Opens an SDL3 audio stream at the native sample rate in stereo float and implements
// IAudioOutput::Submit() by pushing interleaved samples into SDL's
// internal queue.  SDL3's audio stream handles buffering, resampling
// (if the hardware rate differs), and draining automatically.
//
// Usage:
//     SdlAudioOutput audio;            // opens default playback device
//     audio.Resume();                   // start once the buffer is ready
//     audio.Submit(buffer);             // push samples each frame
//     // ... on shutdown, destructor cleans up

#include "snes/core/Platform.hpp"

#include <SDL3/SDL.h>
#include <cstdint>

namespace snes::frontend {

class SdlAudioOutput final : public snes::core::IAudioOutput {
public:
  struct Config {
    int sampleRate = snes::core::AudioTiming::SampleRate;
    int channels = 2; // stereo
  };

  explicit SdlAudioOutput();
  explicit SdlAudioOutput(const Config &config);
  ~SdlAudioOutput() override;

  // Non-copyable / non-movable (owns SDL resources)
  SdlAudioOutput(const SdlAudioOutput &) = delete;
  SdlAudioOutput &operator=(const SdlAudioOutput &) = delete;

  // IAudioOutput
  void Submit(const snes::core::AudioBuffer &buffer) override;

  // Playback control

  /// Request playback after at least 60 ms of input has been queued.
  void Resume();

  /// Pause the audio device.
  void Pause();

  // Stop playback and discard host audio before a pause, reset or game change.
  void Clear();

  /// Returns true if the audio stream was created successfully.
  bool IsValid() const noexcept {
    return stream_ != nullptr;
  }

  /// Returns queued bytes in the input format (stereo float).
  int QueuedBytes() const;

  /// Whether the stream can currently supply samples to the device.
  bool HasPendingOutput() const;

  /// The frame loop should wait while enough audio is already queued.
  bool NeedsSamples() const;

  bool IsPlaying() const noexcept {
    return playing_;
  }
  double QueuedMilliseconds() const;

private:
  void StartIfReady();

  SDL_AudioStream *stream_ = nullptr;
  int sampleRate_ = snes::core::AudioTiming::SampleRate;
  int targetQueuedBytes_ = 0;
  bool playbackRequested_ = false;
  bool playing_ = false;
};

} // namespace snes::frontend
