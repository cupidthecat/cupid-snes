#pragma once

#include "input/InputJournal.hpp"
#include "snes/core/Emulator.hpp"
#include "storage/InputMovie.hpp"
#include <deque>
#include <future>
#include <optional>

namespace snes::frontend {
class Playback {
  public:
    enum class Mode { Live, Recording, Playing, Finished, Failed };

    struct RewindSettings {
        unsigned interval = 0, states = 80;
        size_t bytes = size_t(256) * 1024 * 1024;
        bool operator==(const RewindSettings&) const = default;
    };

    struct HistoryInfo {
        size_t states = 0, bytes = 0;
        uint64_t oldestFrame = 0, newestFrame = 0;
        bool preparing = false;
        std::string error;
    };

    using Restorer =
        std::function<void(core::Emulator::PreparedStateRestore, const core::StateOperation&)>;

    // The machine and source outlive this module. All calls except detached
    // checkpoint completion belong to the machine's owning thread.
    Playback(core::Emulator& machine, core::IInputProvider* source, Restorer restore = {});
    ~Playback();
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;

    class Work {
      public:
        Work(Work&& other) noexcept;
        ~Work();
        Work(const Work&) = delete;
        Work& operator=(const Work&) = delete;

      private:
        friend class Playback;
        explicit Work(Playback& owner);
        Playback* owner_;
        bool previous_;
    };

    // Include checkpoint synchronization in the same clock-query timeline.
    Work Emulating();
    core::FrameStepResult StepFrame(const core::FrameStepOptions& options = {},
                                    bool collectHistory = true);
    Mode Status() const noexcept;
    bool Recording() const noexcept;
    bool Replaying() const noexcept;
    const char* Failure() const noexcept;
    size_t Position() const noexcept;
    static std::array<uint8_t, 32> CheatIdentity(core::CheatSystem system,
                                                 std::span<const core::CheatCode> codes);
    void Record(std::array<uint8_t, 32> cheats, const core::StateOperation& operation = {});
    void FinishRecording(const core::StateOperation& operation = {});
    void DiscardRecording() noexcept;
    const InputMovie* PendingMovie() const noexcept;
    void DiscardPending() noexcept;
    void Play(std::shared_ptr<const InputMovie> movie, std::array<uint8_t, 32> cheats,
              const core::StateOperation& operation = {});
    void StopPlayback() noexcept;
    static void ValidateRewind(const RewindSettings& settings);
    void ConfigureRewind(const RewindSettings& settings);
    HistoryInfo History();
    bool Rewind(const core::StateOperation& operation = {});
    // Reset, device changes and manual restores begin a new history.
    void TimelineChanged();

  private:
    struct Entry {
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        core::Emulator::StatePosition position;
        size_t cursor = 0;
    };

    struct Pending {
        std::future<std::vector<uint8_t>> bytes;
        core::Emulator::StatePosition position;
        size_t cursor = 0;
    };

    void ClearHistory() noexcept;
    void Collect(bool wait = false);
    void Retain(Entry entry);
    void SeedHistory(Entry entry);
    void CaptureHistory();
    void FinishPlayback(const core::StateOperation& operation = {});
    uint64_t ReadHandheldClock();
    core::BroadcastTime ReadBroadcastClock();

    core::Emulator& machine_;
    core::IInputProvider* source_;
    InputJournal journal_;
    Restorer restore_;
    std::function<uint64_t()> handheldClock_;
    std::function<core::BroadcastTime()> broadcastClock_;
    bool emulating_ = false;
    Mode mode_ = Mode::Live;
    const char* movieError_ = nullptr;
    InputMovie::Descriptor descriptor_;
    std::shared_ptr<const std::vector<uint8_t>> initial_;
    std::shared_ptr<const InputMovie> movie_;
    std::optional<InputMovie> completed_;
    RewindSettings settings_;
    std::deque<Entry> history_;
    std::optional<Pending> pending_;
    size_t historyBytes_ = 0;
    uint64_t lastCaptureFrame_ = 0;
    std::string historyError_;
};
} // namespace snes::frontend
