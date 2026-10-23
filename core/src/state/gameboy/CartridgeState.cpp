#include "snes/core/detail/Sha256.hpp"
#include "snes/core/detail/StateArchive.hpp"
#include "snes/core/detail/StateDigest.hpp"
#include "snes/core/gameboy/Cartridge.hpp"

namespace snes::core::gameboy {
void Cartridge::SerializeState(detail::StateArchive& archive) {
    // ROM contents cannot change between successful loads.
    if (!stateIdentity_)
        stateIdentity_ =
            detail::StateDigest(rom_, archive.Progress(),
                                archive.Reading() ? StatePhase::Validate : StatePhase::Capture);
    for (auto byte : *stateIdentity_)
        archive.Expect(byte);
    archive.Expect(uint64_t(rom_.size()));
    archive.Expect(uint64_t(ram_.size()));
    archive.Expect(static_cast<uint32_t>(mapper_));
    archive.Expect(multicart_);
    archive.Expect(extendedMbc3_);
    archive.Expect(battery_);
    archive.Expect(clock_);
    archive.Expect(hasRumble_);
    archive.Fixed(std::span<uint8_t>(ram_));
    archive.Object(multicartRegisters_, [](auto& fields, auto& m) {
        fields.Bounded(m.romLow, uint8_t{0}, uint8_t{31});
        fields.Bounded(m.romMid, uint8_t{0}, uint8_t{3});
        fields.Bounded(m.romHigh, uint8_t{0}, uint8_t{3});
        fields.Bounded(m.romMask, uint8_t{0}, uint8_t{15});
        fields.Bounded(m.ramLow, uint8_t{0}, uint8_t{3});
        fields.Bounded(m.ramHigh, uint8_t{0}, uint8_t{3});
        fields.Bounded(m.ramMask, uint8_t{0}, uint8_t{3});
        fields.Number(m.locked);
        fields.Number(m.multiplex);
        fields.Number(m.mode);
        fields.Number(m.modeDisabled);
    });
    motion_.SerializeState(archive);
    infrared_.SerializeState(archive);
    camera_.SerializeState(archive);
    huc3Clock_.SerializeState(archive);
    tpp1_.SerializeState(archive);
    archive.Array(rtc_);
    archive.Array(latched_);
    archive.Number(lastTime_);
    archive.Bounded(rtcClocks_, uint64_t{0}, uint64_t{UINT32_MAX / 16 - 1});
    archive.Bounded(rtcClockRate_, 32u, UINT32_MAX);
    archive.Number(lowBank_);
    archive.Bounded(highBank_, uint8_t{0}, uint8_t{3});
    archive.Bounded(ramBank_, uint8_t{0}, uint8_t{15});
    archive.Bounded(huc3Mode_, uint8_t{0}, uint8_t{15});
    archive.Number(ramEnabled_);
    archive.Number(mode_);
    archive.Number(rtcMapped_);
    archive.Number(rumble_);
    archive.Number(motionEnabled_);
    archive.Number(bankRegistersWritten_);
    archive.Number(cameraRegisters_);
}
} // namespace snes::core::gameboy
