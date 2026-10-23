#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace snes::frontend {

// Schedule completed pictures from elapsed console clocks. Audio queue feedback
// follows device clock drift gradually instead of following individual callbacks.
class FramePacer {
  public:
    // Keep callback headroom when preparation briefly exceeds one picture interval.
    static constexpr uint32_t kTargetAudioQueueMilliseconds = 60;

    void Reset() noexcept {
        active_ = false;
        queueError_ = 0;
        fractionalNanoseconds_ = 0;
        periodNanoseconds_ = 0;
    }

    uint64_t Schedule(uint64_t now, uint64_t masterClocks, uint32_t clockHz,
                      double queuedMilliseconds) {
        if (clockHz == 0)
            throw std::invalid_argument("Frame clock frequency must be positive");
        if (!active_) {
            active_ = true;
            deadline_ = now;
            return deadline_;
        }

        // The input queue drains in device-sized blocks. Average those steps
        // before correcting playback speed, with at most a 0.5% adjustment.
        queueError_ += (queuedMilliseconds - kTargetAudioQueueMilliseconds - queueError_) / 64.0;
        const double adjustment = 1.0 + std::clamp(queueError_ / 2000.0, -0.005, 0.005);
        const double period =
            double(masterClocks) * 1'000'000'000.0 / clockHz * adjustment + fractionalNanoseconds_;
        const auto whole = static_cast<uint64_t>(period);
        fractionalNanoseconds_ = period - double(whole);
        periodNanoseconds_ = whole;
        deadline_ += whole;

        // Keep the clock phase when computation runs slightly past its deadline.
        // A full interval of lateness starts a fresh cadence, preventing a backlog
        // of overdue pictures after a slow frame or suspended window.
        if (deadline_ < now && now - deadline_ >= whole)
            deadline_ = now;
        return deadline_;
    }

    void Presented(uint64_t now) noexcept {
        // Renderer completion and timer wakeups can arrive partway through a
        // picture interval. Retain the console clock across those delays. A full
        // interval of lateness starts the next deadline at this picture.
        if (active_ && now > deadline_ && now - deadline_ >= periodNanoseconds_)
            deadline_ = now;
    }

  private:
    uint64_t deadline_ = 0;
    uint64_t periodNanoseconds_ = 0;
    double queueError_ = 0;
    double fractionalNanoseconds_ = 0;
    bool active_ = false;
};

} // namespace snes::frontend
