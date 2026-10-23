// SDL3 audio queue and device management.

#include "SdlAudioOutput.hpp"

#include "FramePacer.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace snes::frontend {

// Construction / Destruction

SdlAudioOutput::SdlAudioOutput() : SdlAudioOutput(Config{}) {
}

SdlAudioOutput::SdlAudioOutput(const Config& config) {
    if (config.sampleRate <= 0 || config.sampleRate > 192000 || config.channels != 2) {
        throw std::invalid_argument("Audio requires stereo input at 1 to 192000 Hz");
    }
    sampleRate_ = config.sampleRate;
    targetQueuedBytes_ = ((sampleRate_ * FramePacer::kTargetAudioQueueMilliseconds + 999) / 1000) *
                         2 * sizeof(float);
    SelectDevice(config.deviceName);
}

void SdlAudioOutput::SelectDevice(const std::string& name) {
    // Describe the format we will push: stereo float at the SNES sample rate.
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = sampleRate_;

    // SDL_OpenAudioDeviceStream opens a device, creates a stream, and binds
    // them together.  Passing nullptr for the callback means we will push
    // data ourselves via SDL_PutAudioStreamData (queue-based model).
    // The stream stays paused until Resume() is requested and input is ready.
    auto device = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!name.empty()) {
        int count = 0;
        auto* devices = SDL_GetAudioPlaybackDevices(&count);
        for (int index = 0; devices && index < count; ++index)
            if (const char* deviceName = SDL_GetAudioDeviceName(devices[index]);
                deviceName && name == deviceName) {
                device = devices[index];
                break;
            }
        SDL_free(devices);
    }
    auto* next = SDL_OpenAudioDeviceStream(device, &spec,
                                           nullptr,  // no callback - push model
                                           nullptr); // no userdata

    if (!next) {
        throw std::runtime_error(std::string("SDL_OpenAudioDeviceStream failed: ") +
                                 SDL_GetError());
    }
    if (!SDL_SetAudioStreamGain(next, muted_ ? 0.0f : volume_ / 100.0f)) {
        const std::string error = SDL_GetError();
        SDL_DestroyAudioStream(next);
        throw std::runtime_error(error);
    }
    if (stream_)
        SDL_DestroyAudioStream(stream_);
    stream_ = next;
    playbackRequested_ = playing_ = false;
}

std::vector<std::string> SdlAudioOutput::DeviceNames() {
    std::vector<std::string> names;
    int count = 0;
    auto* devices = SDL_GetAudioPlaybackDevices(&count);
    for (int index = 0; devices && index < count; ++index)
        if (const char* name = SDL_GetAudioDeviceName(devices[index]))
            names.emplace_back(name);
    SDL_free(devices);
    return names;
}

SdlAudioOutput::~SdlAudioOutput() {
    if (stream_) {
        // Destroying the stream also closes the associated audio device.
        SDL_DestroyAudioStream(stream_);
    }
}

// IAudioOutput::Submit

void SdlAudioOutput::Submit(const snes::core::AudioBuffer& buffer) {
    if (!stream_)
        return;
    if (buffer.interleavedStereo.empty())
        return;

    if (buffer.sampleRate != sampleRate_ || buffer.interleavedStereo.size() % 2 != 0) {
        throw std::invalid_argument("Audio buffer does not match the stream's stereo input format");
    }
    if (buffer.interleavedStereo.size() > std::numeric_limits<int>::max() / sizeof(float)) {
        throw std::length_error("Audio buffer is too large");
    }
    // After a stall, collect a fresh buffer before restarting the device.
    const bool pendingOutput = HasPendingOutput();
    // SDL may retain a few input frames for the resampler even after output drains.
    if (playing_ && !pendingOutput) {
        if (!SDL_PauseAudioStreamDevice(stream_)) {
            throw std::runtime_error(std::string("Audio pause failed: ") + SDL_GetError());
        }
        playing_ = false;
    }

    // Push interleaved float samples into SDL's internal queue.
    // SDL resamples automatically when the playback device uses a different rate.
    const int bytes = static_cast<int>(buffer.interleavedStereo.size() * sizeof(float));
    if (!SDL_PutAudioStreamData(stream_, buffer.interleavedStereo.data(), bytes)) {
        throw std::runtime_error(std::string("Audio submit failed: ") + SDL_GetError());
    }
    StartIfReady();
}

// Playback control

void SdlAudioOutput::Resume() {
    playbackRequested_ = true;
    StartIfReady();
}

void SdlAudioOutput::Pause() {
    if (stream_ && !SDL_PauseAudioStreamDevice(stream_)) {
        throw std::runtime_error(std::string("Audio pause failed: ") + SDL_GetError());
    }
    playbackRequested_ = false;
    playing_ = false;
}

void SdlAudioOutput::Clear() {
    Pause();
    if (stream_ && !SDL_ClearAudioStream(stream_))
        throw std::runtime_error(std::string("Audio clear failed: ") + SDL_GetError());
}

void SdlAudioOutput::SetVolume(int percent, bool muted) {
    if (percent < 0 || percent > 200)
        throw std::invalid_argument("Audio volume must be between 0 and 200 percent");
    if (stream_ && !SDL_SetAudioStreamGain(stream_, muted ? 0.0f : percent / 100.0f))
        throw std::runtime_error(std::string("Cannot set audio volume: ") + SDL_GetError());
    volume_ = percent;
    muted_ = muted;
}

int SdlAudioOutput::QueuedBytes() const {
    const int bytes = stream_ ? SDL_GetAudioStreamQueued(stream_) : 0;
    if (bytes < 0) {
        throw std::runtime_error(std::string("Audio queue query failed: ") + SDL_GetError());
    }
    return bytes;
}

bool SdlAudioOutput::HasPendingOutput() const {
    if (!stream_)
        return false;
    const int available = SDL_GetAudioStreamAvailable(stream_);
    if (available < 0)
        throw std::runtime_error(std::string("Audio availability query failed: ") + SDL_GetError());
    return available != 0;
}

bool SdlAudioOutput::NeedsSamples() const {
    return QueuedBytes() < targetQueuedBytes_;
}

double SdlAudioOutput::QueuedMilliseconds() const {
    return 1000.0 * QueuedBytes() / (sampleRate_ * 2 * sizeof(float));
}

void SdlAudioOutput::StartIfReady() {
    if (stream_ && playbackRequested_ && !playing_ && !NeedsSamples()) {
        if (!SDL_ResumeAudioStreamDevice(stream_)) {
            throw std::runtime_error(std::string("Audio resume failed: ") + SDL_GetError());
        }
        playing_ = true;
    }
}

} // namespace snes::frontend
