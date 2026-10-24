#include "Playback.hpp"
#include "snes/core/detail/Sha256.hpp"
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
namespace {
uint64_t Pack(core::BroadcastTime time) {
    return uint64_t(time.year) | (uint64_t(time.month) << 16) | (uint64_t(time.day) << 24) |
           (uint64_t(time.weekday) << 32) | (uint64_t(time.hour) << 40) |
           (uint64_t(time.minute) << 48) | (uint64_t(time.second) << 56);
}

core::BroadcastTime Unpack(uint64_t value) {
    return {uint16_t(value),      uint8_t(value >> 16), uint8_t(value >> 24), uint8_t(value >> 32),
            uint8_t(value >> 40), uint8_t(value >> 48), uint8_t(value >> 56)};
}
} // namespace

Playback::Playback(core::Emulator& machine, core::IInputProvider* source, Restorer restore)
    : machine_(machine), source_(source), journal_(source), restore_(std::move(restore)),
      handheldClock_(machine.GetGameBoyTimeSource()) {
    std::function<uint64_t()> handheld = [this] { return ReadHandheldClock(); };
    std::function<core::BroadcastTime()> broadcast = [this] { return ReadBroadcastClock(); };
    machine_.SetGameBoyTimeSource(std::move(handheld));
    broadcastClock_ = machine_.ExchangeBroadcastTimeSource(std::move(broadcast));
    machine_.AttachInputProvider(&journal_);
}

Playback::~Playback() {
    machine_.AttachInputProvider(source_);
    machine_.ExchangeBroadcastTimeSource(std::move(broadcastClock_));
    try {
        machine_.SetGameBoyTimeSource(std::move(handheldClock_));
    } catch (...) {
    }
}

Playback::Work::Work(Playback& owner) : owner_(&owner), previous_(owner.emulating_) {
    owner.emulating_ = true;
}

Playback::Work::Work(Work&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), previous_(other.previous_) {
}

Playback::Work::~Work() {
    if (owner_)
        owner_->emulating_ = previous_;
}

Playback::Work Playback::Emulating() {
    return Work(*this);
}

uint64_t Playback::ReadHandheldClock() {
    const auto source = [&] {
        return handheldClock_ ? handheldClock_() : uint64_t(std::time(nullptr));
    };
    return emulating_ ? journal_.PollClock(InputJournal::Query::HandheldClock,
                                           machine_.CurrentFrame(), source)
                      : source();
}

core::BroadcastTime Playback::ReadBroadcastClock() {
    const auto source = [&] { return broadcastClock_ ? broadcastClock_() : core::BroadcastTime{}; };
    return emulating_
               ? Unpack(journal_.PollClock(InputJournal::Query::BroadcastClock,
                                           machine_.CurrentFrame(), [&] { return Pack(source()); }))
               : source();
}

Playback::Mode Playback::Status() const noexcept {
    return Failure() && mode_ != Mode::Live ? Mode::Failed : mode_;
}

bool Playback::Recording() const noexcept {
    return mode_ == Mode::Recording;
}

bool Playback::Replaying() const noexcept {
    return mode_ == Mode::Playing || mode_ == Mode::Finished;
}

const char* Playback::Failure() const noexcept {
    return journal_.Failure() ? journal_.Failure() : movieError_;
}

size_t Playback::Position() const noexcept {
    return journal_.Position();
}

std::array<uint8_t, 32> Playback::CheatIdentity(core::CheatSystem system,
                                                std::span<const core::CheatCode> codes) {
    core::detail::Sha256Hasher hash;
    const uint8_t kind = uint8_t(system);
    hash.Update(std::span(&kind, 1));
    for (const auto& code : codes) {
        const std::array<uint8_t, 7> bytes{uint8_t(code.address),
                                           uint8_t(code.address >> 8),
                                           uint8_t(code.address >> 16),
                                           uint8_t(code.address >> 24),
                                           code.value,
                                           uint8_t(code.compare.has_value()),
                                           code.compare.value_or(0)};
        hash.Update(bytes);
    }
    return hash.Finish();
}

void Playback::Record(std::array<uint8_t, 32> cheats, const core::StateOperation& operation) {
    if (mode_ != Mode::Live || completed_)
        throw std::runtime_error("Stop the current movie and save its recording first");
    auto work = Emulating();
    auto state = std::make_shared<const std::vector<uint8_t>>(machine_.CaptureState(operation));
    InputJournal next(source_);
    next.Record(InputMovie::maximumSamples);
    ClearHistory();
    descriptor_ = {machine_.CurrentFrame(), machine_.CurrentFrame(), machine_.CurrentMasterCycles(),
                   machine_.CurrentMasterCycles(), cheats};
    initial_ = std::move(state);
    journal_ = std::move(next);
    movieError_ = nullptr;
    mode_ = Mode::Recording;
    if (settings_.interval)
        SeedHistory({initial_, {descriptor_.firstFrame, descriptor_.firstClock}, 0});
}

void Playback::FinishRecording(const core::StateOperation& operation) {
    if (!Recording())
        throw std::runtime_error("No movie recording is active");
    auto work = Emulating();
    auto end = machine_.PrepareStateCapture(operation);
    const auto position = end.Position();
    descriptor_.lastFrame = position.frame;
    descriptor_.lastClock = position.masterCycles;
    completed_.emplace(initial_, journal_.FinishRecording(), descriptor_);
    initial_.reset();
    mode_ = Mode::Live;
    ClearHistory();
}

const InputMovie* Playback::PendingMovie() const noexcept {
    return completed_ ? &*completed_ : nullptr;
}

void Playback::DiscardRecording() noexcept {
    if (!Recording())
        return;
    journal_.Stop();
    initial_.reset();
    mode_ = Mode::Live;
    movieError_ = nullptr;
    ClearHistory();
}

void Playback::DiscardPending() noexcept {
    completed_.reset();
}

void Playback::Play(std::shared_ptr<const InputMovie> movie, std::array<uint8_t, 32> cheats,
                    const core::StateOperation& operation) {
    if (!movie || mode_ != Mode::Live || completed_)
        throw std::runtime_error("Stop the current movie and save its recording first");
    movie->Validate(operation);
    if (movie->Info().cheats != cheats)
        throw std::runtime_error("Movie playback requires the recorded cheat settings");
    auto prepared = machine_.PrepareStateRestore(movie->InitialState(), operation);
    const auto position = prepared.Position();
    if (position.frame != movie->Info().firstFrame ||
        position.masterCycles != movie->Info().firstClock)
        throw std::runtime_error("Movie start position does not match its checkpoint");
    InputJournal next(source_);
    next.Play(movie->Inputs(), {.advanceSource = true, .liveAfterEnd = false}, operation);
    auto initial = movie->ShareInitialState();
    if (restore_)
        restore_(std::move(prepared), operation);
    else
        prepared.Commit();
    ClearHistory();
    journal_ = std::move(next);
    descriptor_ = movie->Info();
    movie_ = std::move(movie);
    movieError_ = nullptr;
    mode_ = Mode::Playing;
    if (settings_.interval)
        SeedHistory({std::move(initial), position, 0});
}

void Playback::StopPlayback() noexcept {
    if (!Replaying())
        return;
    journal_.Stop();
    movie_.reset();
    movieError_ = nullptr;
    mode_ = Mode::Live;
    ClearHistory();
}

void Playback::FinishPlayback(const core::StateOperation& operation) {
    auto end = machine_.PrepareStateCapture(operation);
    const auto position = end.Position();
    if (position.frame != descriptor_.lastFrame || position.masterCycles != descriptor_.lastClock ||
        journal_.Position() != movie_->Inputs()->size() || journal_.Failure()) {
        movieError_ = "Movie endpoint does not match its recording";
        throw std::runtime_error(Failure());
    }
    mode_ = Mode::Finished;
}

core::FrameStepResult Playback::StepFrame(const core::FrameStepOptions& options,
                                          bool collectHistory) {
    if (Status() == Mode::Failed)
        throw std::runtime_error(Failure());
    if (mode_ == Mode::Finished)
        return {machine_.CurrentFrame(), machine_.CurrentMasterCycles(), {}};
    auto work = Emulating();
    if (mode_ == Mode::Playing && machine_.CurrentFrame() >= descriptor_.lastFrame) {
        FinishPlayback();
        return {machine_.CurrentFrame(), machine_.CurrentMasterCycles(), {}};
    }
    const auto result = machine_.StepFrame(options);
    if (journal_.Failure())
        throw std::runtime_error(journal_.Failure());
    if (collectHistory)
        CaptureHistory();
    if (journal_.Failure())
        throw std::runtime_error(journal_.Failure());
    if (mode_ == Mode::Playing && machine_.CurrentFrame() >= descriptor_.lastFrame)
        FinishPlayback();
    return result;
}

void Playback::ClearHistory() noexcept {
    pending_.reset();
    history_.clear();
    historyBytes_ = 0;
    lastCaptureFrame_ = machine_.CurrentFrame();
    historyError_.clear();
}

void Playback::ValidateRewind(const RewindSettings& settings) {
    if (settings.interval > 600 || !settings.states || settings.states > 320 || !settings.bytes ||
        settings.bytes > size_t(1024) * 1024 * 1024)
        throw std::invalid_argument("Choose supported rewind limits");
}

void Playback::ConfigureRewind(const RewindSettings& settings) {
    ValidateRewind(settings);
    if (settings_ == settings)
        return;
    ClearHistory();
    settings_ = settings;
}

void Playback::Retain(Entry entry) {
    if (entry.bytes->size() > settings_.bytes)
        throw std::runtime_error("The machine checkpoint exceeds the rewind memory limit");
    while (!history_.empty() && (history_.size() >= settings_.states ||
                                 entry.bytes->size() > settings_.bytes - historyBytes_)) {
        historyBytes_ -= history_.front().bytes->size();
        history_.pop_front();
    }
    const auto bytes = entry.bytes->size();
    history_.push_back(std::move(entry));
    historyBytes_ += bytes;
    lastCaptureFrame_ = history_.back().position.frame;
}

void Playback::SeedHistory(Entry entry) {
    try {
        Retain(std::move(entry));
    } catch (const std::exception& error) {
        historyError_ = error.what();
        settings_.interval = 0;
    }
}

void Playback::Collect(bool wait) {
    if (!pending_ ||
        (!wait && pending_->bytes.wait_for(std::chrono::seconds(0)) != std::future_status::ready))
        return;
    auto pending = std::move(*pending_);
    pending_.reset();
    try {
        auto bytes = std::make_shared<const std::vector<uint8_t>>(pending.bytes.get());
        SeedHistory({std::move(bytes), pending.position, pending.cursor});
    } catch (const std::exception& error) {
        historyError_ = error.what();
        settings_.interval = 0;
    }
}

void Playback::CaptureHistory() {
    Collect();
    if (!settings_.interval || pending_)
        return;
    const auto frame = machine_.CurrentFrame();
    if (frame < lastCaptureFrame_) {
        ClearHistory();
        return;
    }
    if (frame - lastCaptureFrame_ < settings_.interval)
        return;
    try {
        auto capture = machine_.PrepareStateCapture();
        if (capture.Size() > settings_.bytes)
            throw std::runtime_error("The machine checkpoint exceeds the rewind memory limit");
        const auto position = capture.Position();
        const auto cursor = journal_.Position();
        auto future = std::async(std::launch::async,
                                 [capture = std::move(capture)] { return capture.Finish(); });
        pending_.emplace(Pending{std::move(future), position, cursor});
        lastCaptureFrame_ = position.frame;
    } catch (const std::exception& error) {
        historyError_ = error.what();
        settings_.interval = 0;
    }
}

Playback::HistoryInfo Playback::History() {
    Collect();
    return {history_.size(),
            historyBytes_,
            history_.empty() ? 0 : history_.front().position.frame,
            history_.empty() ? 0 : history_.back().position.frame,
            bool(pending_),
            historyError_};
}

bool Playback::Rewind(const core::StateOperation& operation) {
    Collect(true);
    auto candidate = history_.size();
    while (candidate &&
           history_[candidate - 1].position.masterCycles >= machine_.CurrentMasterCycles())
        --candidate;
    if (!candidate)
        return false;
    const auto& entry = history_[candidate - 1];
    const bool recording = Recording(), replaying = Replaying();
    if ((recording && entry.cursor > journal_.Position()) ||
        (replaying && entry.cursor > movie_->Inputs()->size()))
        throw std::runtime_error("Rewind cursor no longer belongs to the active movie");
    auto prepared = machine_.PrepareStateRestore(*entry.bytes, operation);
    prepared.Commit();
    if (recording || replaying)
        journal_.Seek(entry.cursor);
    if (replaying)
        mode_ = Mode::Playing;
    movieError_ = nullptr;
    lastCaptureFrame_ = entry.position.frame;
    while (history_.size() >= candidate) {
        historyBytes_ -= history_.back().bytes->size();
        history_.pop_back();
    }
    return true;
}

void Playback::TimelineChanged() {
    if (Recording() || Replaying())
        throw std::runtime_error("Stop the movie before changing its timeline");
    ClearHistory();
}
} // namespace snes::frontend
