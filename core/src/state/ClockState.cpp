#include "snes/core/EpsonRtc.hpp"
#include "snes/core/Srtc.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void Srtc::SerializeState(detail::StateArchive& archive) {
    archive.Expect(masterFrequency_);
    // Interrupted nibble writes can retain carries beyond ordinary calendar ranges.
    archive.Array(calendar_);
    archive.Bounded(balance_, int64_t{0}, int64_t(Timing::kMasterClockHz - 1));
    archive.Bounded(index_, -1, 13);
    archive.Enumeration(mode_, Mode::Idle, Mode::Write);
}

void EpsonRtc::SerializeState(detail::StateArchive& archive) {
    archive.Expect(masterFrequency_);
    for (auto& digit : digits_)
        archive.Bounded(digit, uint8_t{0}, uint8_t{15});
    archive.Bounded(ticks_, 0u, (1u << 21) - 1);
    archive.Bounded(seconds_, 0u, 1439u);
    archive.Bounded(balance_, int64_t{0}, int64_t(Timing::kMasterClockHz - 1));
    archive.Bounded(selected_, uint8_t{0}, uint8_t{3});
    archive.Bounded(position_, uint8_t{0}, uint8_t{15});
    archive.Bounded(last_, uint8_t{0}, uint8_t{15});
    archive.Bounded(wait_, uint8_t{0}, uint8_t{8});
    archive.Number(ready_);
    archive.Number(resync_);
    archive.Number(irq_);
    archive.Number(heldTick_);
    archive.Enumeration(phase_, Phase::Mode, Phase::Write);
}
} // namespace snes::core
