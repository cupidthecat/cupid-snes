#include "snes/core/EpsonRtc.hpp"
#include "snes/core/Spc7110.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void Spc7110::SerializeState(detail::StateArchive& archive) {
    archive.Expect(uint64_t(rom_.size()));
    archive.Expect(uint64_t(data_.size()));
    archive.Expect(uint64_t(save_.size()));
    archive.Expect(masterFrequency_);
    archive.Expect(bool(rtc_));
    archive.Array(registers_);
    decoder_.SerializeState(archive);
    if (rtc_)
        rtc_->SerializeState(archive);
    archive.Bounded(balance_, int64_t{0}, int64_t(Timing::kMasterClockHz) * 40 - 1);
    const auto phase = archive.Enumeration(phase_, Phase::Begin, Phase::Idle);
    archive.Validate(phase != Phase::CheckMultiply && phase != Phase::CheckDivide,
                     "SPC7110 state retains an unfinished operation edge");
    archive.Number(decodePending_);
    archive.Number(multiplyPending_);
    archive.Number(dividePending_);
    archive.Bounded(mode_, uint8_t{0}, uint8_t{3});
    archive.Bounded(tileOffset_, uint8_t{0}, uint8_t{31});
    archive.Bounded(streamAddress_, 0u, 0x7fffffu);
    archive.Number(decodedRow_);
    archive.Array(tile_);
}
} // namespace snes::core
