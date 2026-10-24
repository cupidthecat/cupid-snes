// File-backed MSU-1 data registers and stereo PCM playback.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace snes::core {
namespace detail {
class StateArchive;
}

class Msu1 {
  public:
    Msu1() = default;
    Msu1(const Msu1&) = delete;
    Msu1& operator=(const Msu1&) = delete;
    Msu1(Msu1&&) noexcept = default;
    Msu1& operator=(Msu1&&) noexcept = default;
    void SerializeState(detail::StateArchive& archive);
    using StereoSample = std::array<int16_t, 2>;
    using OutputFrame = std::array<float, 2>;
    static constexpr uint32_t SampleRate = 44100;

    // A ROM named game.sfc uses game.msu and game-<track>.pcm beside it.
    // An empty data file is valid. Failure closes any previously loaded pack.
    bool Open(const std::filesystem::path& romPath, std::string* error = nullptr);
    void Close();
    void Reset(); // Keep the data file attached, but reset all device registers.

    bool IsOpen() const noexcept {
        return data_.IsOpen();
    }

    static constexpr bool Selects(uint32_t address) noexcept {
        return address <= 0xffffff && (address & 0x400000) == 0 && (address & 0xfff8) == 0x2000;
    }

    uint8_t Read(uint32_t address, uint8_t openBus = 0xff);
    void Write(uint32_t address, uint8_t value);

    // Advance one 44.1 kHz input tick. Muting still consumes the PCM frame.
    // Feed every result, including silence, through the continuous audio stream.
    OutputFrame Clock(bool dspMuted = false);

  private:
    // Each file has a fixed cache. Neither file size nor seek distance changes
    // memory consumption, including data seeks past the signed 32-bit range.
    class Stream {
      public:
        bool Open(const std::filesystem::path& path);
        void Close();
        bool Read(uint64_t offset, std::span<uint8_t> destination);

        bool IsOpen() const noexcept {
            return bool(resource_);
        }

        uint64_t Size() const noexcept {
            return state_.size;
        }

        bool SerializeState(detail::StateArchive& archive, const std::filesystem::path& path,
                            bool reuse);

      private:
        struct Resource;

        struct State {
            std::array<uint8_t, 8192> cache{};
            std::array<uint8_t, 32> identity{};
            uint64_t size = 0, mediaSize = 0, cacheOffset = 0;
            uint32_t cacheSize = 0;
            bool opened = false;
        } state_;

        std::shared_ptr<Resource> resource_;
    };

    struct ResumePoint {
        bool valid = false;
        uint16_t track = 0;
        uint32_t offset = 0;
    };

    void SelectTrack();
    void OpenAudio();

    static constexpr uint8_t AudioError = 0x08;
    static constexpr uint8_t Playing = 0x10;
    static constexpr uint8_t Repeating = 0x20;

    Stream data_;
    Stream pcm_;
    std::filesystem::path basePath_;
    uint32_t dataSeek_ = 0;
    uint64_t dataOffset_ = 0;
    uint16_t track_ = 0;
    uint16_t openedTrack_ = 0;
    uint8_t status_ = 0;
    uint8_t volume_ = 0;
    uint64_t audioPosition_ = 0;
    uint32_t playOffset_ = 0;
    uint32_t loopOffset_ = 0;
    ResumePoint resume_{};
};

} // namespace snes::core
