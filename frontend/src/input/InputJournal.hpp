#pragma once

#include "snes/core/Platform.hpp"
#include "snes/core/StateOperation.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace snes::frontend {

class InputJournal final : public core::IInputProvider {
  public:
    enum class Query : uint8_t {
        Primary,
        Controller,
        Mouse,
        LightGun,
        HandheldClock,
        BroadcastClock
    };

    struct Sample {
        uint64_t frame = 0;
        Query query = Query::Primary;
        int index = 0, gun = 0;
        uint32_t buttons = 0;
        int32_t x = 0, y = 0;
        bool operator==(const Sample&) const = default;
    };

    using Recording = std::vector<Sample>;
    enum class State { Live, Recording, Playback, Finished, Failed };

    struct PlaybackOptions {
        size_t cursor = 0;
        bool advanceSource = true;
        bool liveAfterEnd = true;
    };

    explicit InputJournal(core::IInputProvider* source = nullptr) : source_(source) {
    }

    InputJournal(const InputJournal&) = delete;
    InputJournal& operator=(const InputJournal&) = delete;
    InputJournal(InputJournal&&) noexcept = default;
    InputJournal& operator=(InputJournal&&) noexcept = default;
    // Call between emulation steps. Starting a new operation discards the old one.
    void Record(size_t maximumSamples = std::numeric_limits<size_t>::max());
    // Stops recording and transfers immutable ownership without copying samples.
    // A failed recording cannot be exported as a complete input sequence.
    std::shared_ptr<const Recording> FinishRecording();
    void Play(std::shared_ptr<const Recording> recording, const PlaybackOptions& options,
              const core::StateOperation& operation = {});

    void Play(std::shared_ptr<const Recording> recording) {
        Play(std::move(recording), PlaybackOptions{});
    }

    static void Validate(const Recording& recording, const core::StateOperation& operation = {});
    void Stop() noexcept;

    State Status() const noexcept {
        return state_;
    }

    const char* Failure() const noexcept {
        return failure_;
    }

    // A checkpoint's cursor follows all queries consumed while capturing it,
    // including input already latched for its partial next field.
    size_t Position() const noexcept;
    // Restore the cursor only after the matching machine checkpoint commits.
    // Recording discards the old future; playback retains the immutable sequence.
    void Seek(size_t cursor);
    uint64_t PollClock(Query query, uint64_t frame, const std::function<uint64_t()>& source);

    core::InputState Poll(uint64_t frame) override;
    core::InputState PollController(int player, uint64_t frame) override;
    core::MouseState PollMouse(int port, uint64_t frame) override;
    core::LightGunState PollLightGun(int port, int gun, uint64_t frame) override;
    void AdvanceFrame() noexcept override;

  private:
    static bool Valid(const Sample& sample) noexcept;
    void Append(Sample sample) noexcept;
    const Sample* Read(Query query, int index, int gun, uint64_t frame) noexcept;
    bool UsePlayback() const noexcept;
    void Fail(const char* message) noexcept;

    core::IInputProvider* source_;
    std::shared_ptr<Recording> recording_;
    std::shared_ptr<const Recording> playback_;
    State state_ = State::Live;
    PlaybackOptions options_;
    size_t position_ = 0, maximumSamples_ = 0;
    const char* failure_ = nullptr;
};

} // namespace snes::frontend
