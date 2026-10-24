#include "InputJournal.hpp"
#include <array>
#include <bit>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
namespace {
constexpr std::array<bool core::InputState::*, 12> buttons{
    &core::InputState::up,    &core::InputState::down,  &core::InputState::left,
    &core::InputState::right, &core::InputState::a,     &core::InputState::b,
    &core::InputState::x,     &core::InputState::y,     &core::InputState::l,
    &core::InputState::r,     &core::InputState::start, &core::InputState::select};

uint32_t Pack(const core::InputState& input) noexcept {
    uint32_t value = 0;
    for (size_t index = 0; index < buttons.size(); ++index)
        value |= uint32_t(input.*buttons[index]) << index;
    return value;
}

core::InputState Unpack(uint32_t value) noexcept {
    core::InputState input;
    for (size_t index = 0; index < buttons.size(); ++index)
        input.*buttons[index] = (value >> index) & 1;
    return input;
}
} // namespace

bool InputJournal::Valid(const Sample& sample) noexcept {
    switch (sample.query) {
    case Query::Primary:
        return sample.index == 0 && sample.gun == 0 && sample.buttons < 4096 && sample.x == 0 &&
               sample.y == 0;
    case Query::Controller:
        return sample.index >= 0 && sample.index < 8 && sample.gun == 0 && sample.buttons < 4096 &&
               sample.x == 0 && sample.y == 0;
    case Query::Mouse:
        return sample.index >= 0 && sample.index < 2 && sample.gun == 0 && sample.buttons < 4;
    case Query::LightGun:
        return sample.index >= 0 && sample.index < 2 && sample.gun >= 0 && sample.gun < 2 &&
               sample.buttons < 64;
    case Query::HandheldClock:
    case Query::BroadcastClock:
        return sample.index == 0 && sample.gun == 0 && sample.buttons == 0;
    }
    return false;
}

void InputJournal::Record(size_t maximumSamples) {
    auto recording = std::make_shared<Recording>();
    Stop();
    recording_ = std::move(recording);
    maximumSamples_ = maximumSamples;
    state_ = State::Recording;
}

std::shared_ptr<const InputJournal::Recording> InputJournal::FinishRecording() {
    if (state_ == State::Failed && recording_)
        throw std::runtime_error(failure_);
    if (state_ != State::Recording)
        throw std::runtime_error("No input recording is active");
    state_ = State::Live;
    return std::exchange(recording_, {});
}

void InputJournal::Play(std::shared_ptr<const Recording> recording, const PlaybackOptions& options,
                        const core::StateOperation& operation) {
    if (!recording || options.cursor > recording->size())
        throw std::invalid_argument("Invalid input recording or playback cursor");
    Validate(*recording, operation);
    Stop();
    playback_ = std::move(recording);
    options_ = options;
    position_ = options.cursor;
    state_ = position_ == playback_->size() ? State::Finished : State::Playback;
}

void InputJournal::Validate(const Recording& recording, const core::StateOperation& operation) {
    uint64_t frame = 0;
    for (size_t index = 0; index < recording.size(); ++index) {
        if (index % 2048 == 0)
            operation.Report(core::StatePhase::Validate, index, recording.size(), "Input queries");
        const auto& sample = recording[index];
        if (!Valid(sample) || sample.frame < frame)
            throw std::invalid_argument("Invalid input recording query");
        frame = sample.frame;
    }
    operation.Report(core::StatePhase::Validate, recording.size(), recording.size(),
                     "Input queries");
}

void InputJournal::Stop() noexcept {
    state_ = State::Live;
    recording_.reset();
    playback_.reset();
    position_ = 0;
    failure_ = nullptr;
}

size_t InputJournal::Position() const noexcept {
    return recording_ ? recording_->size() : position_;
}

void InputJournal::Seek(size_t cursor) {
    if (recording_) {
        if (cursor > recording_->size())
            throw std::out_of_range("Recording cursor is outside the retained input");
        recording_->resize(cursor);
        state_ = State::Recording;
    } else if (playback_) {
        if (cursor > playback_->size())
            throw std::out_of_range("Playback cursor is outside the recorded input");
        position_ = cursor;
        state_ = cursor == playback_->size() ? State::Finished : State::Playback;
    } else
        throw std::runtime_error("No input timeline is active");
    failure_ = nullptr;
}

uint64_t InputJournal::PollClock(Query query, uint64_t frame,
                                 const std::function<uint64_t()>& source) {
    if (query != Query::HandheldClock && query != Query::BroadcastClock)
        throw std::invalid_argument("Choose a supported movie clock query");
    if (UsePlayback()) {
        const auto* sample = Read(query, 0, 0, frame);
        return sample ? uint64_t(uint32_t(sample->x)) | (uint64_t(uint32_t(sample->y)) << 32) : 0;
    }
    const auto value = source ? source() : 0;
    Append({frame, query, 0, 0, 0, std::bit_cast<int32_t>(uint32_t(value)),
            std::bit_cast<int32_t>(uint32_t(value >> 32))});
    return value;
}

void InputJournal::Fail(const char* message) noexcept {
    failure_ = message;
    state_ = State::Failed;
}

void InputJournal::Append(Sample sample) noexcept {
    if (state_ != State::Recording)
        return;
    if (!Valid(sample) || (!recording_->empty() && sample.frame < recording_->back().frame)) {
        Fail("Input recording received an invalid query or a backwards field index");
        return;
    }
    if (recording_->size() >= maximumSamples_) {
        Fail("Input recording reached its sample limit");
        return;
    }
    try {
        recording_->push_back(sample);
    } catch (...) {
        Fail("Input recording storage is exhausted");
    }
}

bool InputJournal::UsePlayback() const noexcept {
    return playback_ && !(state_ == State::Finished && options_.liveAfterEnd);
}

const InputJournal::Sample* InputJournal::Read(Query query, int index, int gun,
                                               uint64_t frame) noexcept {
    if (state_ == State::Failed)
        return nullptr;
    if (position_ >= playback_->size()) {
        Fail("Input playback exhausted before the requested query");
        return nullptr;
    }
    const auto& sample = (*playback_)[position_];
    if (sample.query != query || sample.index != index || sample.gun != gun ||
        sample.frame != frame) {
        Fail("Input playback query does not match the recording");
        return nullptr;
    }
    if (++position_ == playback_->size())
        state_ = State::Finished;
    return &sample;
}

core::InputState InputJournal::Poll(uint64_t frame) {
    if (UsePlayback()) {
        const auto* sample = Read(Query::Primary, 0, 0, frame);
        return sample ? Unpack(sample->buttons) : core::InputState{};
    }
    const auto value = source_ ? source_->Poll(frame) : core::InputState{};
    Append({frame, Query::Primary, 0, 0, Pack(value)});
    return value;
}

core::InputState InputJournal::PollController(int player, uint64_t frame) {
    if (UsePlayback()) {
        const auto* sample = Read(Query::Controller, player, 0, frame);
        return sample ? Unpack(sample->buttons) : core::InputState{};
    }
    const auto value = source_ ? source_->PollController(player, frame) : core::InputState{};
    Append({frame, Query::Controller, player, 0, Pack(value)});
    return value;
}

core::MouseState InputJournal::PollMouse(int port, uint64_t frame) {
    if (UsePlayback()) {
        const auto* sample = Read(Query::Mouse, port, 0, frame);
        if (!sample)
            return {};
        return {sample->x, sample->y, bool(sample->buttons & 1), bool(sample->buttons & 2)};
    }
    const auto value = source_ ? source_->PollMouse(port, frame) : core::MouseState{};
    Append({frame, Query::Mouse, port, 0, uint32_t(value.left) | (uint32_t(value.right) << 1),
            value.dx, value.dy});
    return value;
}

core::LightGunState InputJournal::PollLightGun(int port, int gun, uint64_t frame) {
    if (UsePlayback()) {
        const auto* sample = Read(Query::LightGun, port, gun, frame);
        if (!sample)
            return {};
        return {sample->x,
                sample->y,
                bool(sample->buttons & 1),
                bool(sample->buttons & 2),
                bool(sample->buttons & 4),
                bool(sample->buttons & 8),
                bool(sample->buttons & 16),
                bool(sample->buttons & 32)};
    }
    const auto value = source_ ? source_->PollLightGun(port, gun, frame) : core::LightGunState{};
    const auto flags = uint32_t(value.trigger) | (uint32_t(value.cursor) << 1) |
                       (uint32_t(value.turbo) << 2) | (uint32_t(value.pause) << 3) |
                       (uint32_t(value.start) << 4) | (uint32_t(value.offscreen) << 5);
    Append({frame, Query::LightGun, port, gun, flags, value.x, value.y});
    return value;
}

void InputJournal::AdvanceFrame() noexcept {
    if (source_ && (!playback_ || options_.advanceSource))
        source_->AdvanceFrame();
}

} // namespace snes::frontend
