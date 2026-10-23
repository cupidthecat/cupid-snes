#include "snes/core/Timing.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "snes/core/detail/StateArchive.hpp"
#include "snes/core/gameboy/Adapter.hpp"
#include "snes/core/gameboy/Machine.hpp"
#include "snes/core/gameboy/Peripherals.hpp"

namespace snes::core::gameboy {
void Machine::SerializeState(detail::StateArchive& archive) {
    for (auto byte : detail::Sha256(boot_))
        archive.Expect(byte);
    cpu_.SerializeState(archive);
    timer_.SerializeState(archive);
    cartridge_.SerializeState(archive);
    startup_->SerializeState(archive);
    archive.Array(ram_);
    archive.Array(highRam_);
    const auto cpu = archive.Number(cpuClock_);
    const auto device = archive.Number(deviceClock_);
    const auto dma = archive.Number(dmaClock_);
    archive.Validate(cpu == device && dma == device,
                     "Handheld machine state requires a completed instruction or bus operation");
    archive.Number(dmaSource_);
    const auto position = archive.Number(dmaPosition_);
    archive.Validate(position <= 0xa1 || position == 0xff, "Invalid handheld DMA position");
    archive.Number(dmaClocks_);
    archive.Number(dmaRegister_);
    archive.Number(flags_);
    archive.Number(enable_);
    archive.Validate((archive.Number(joypad_) & 0xc0) == 0xc0, "Invalid handheld joypad bits");
    archive.Number(dataBus_);
    archive.Bounded(decay_, uint8_t{0}, uint8_t{12});
    archive.Number(bootDisabled_);
    archive.Number(dmaRestarting_);
}

void Peripherals::SerializeState(detail::StateArchive& archive) {
    video_.SerializeState(archive);
    audio_.SerializeState(archive);
    archive.Number(clocks_);
}

void Adapter::SerializeState(detail::StateArchive& archive) {
    archive.Expect(secondModel_);
    archive.Expect(loaded_);
    archive.Expect(masterHz_);
    archive.Bounded(clockCredit_, -int64_t{128} * Timing::kMasterClockHz,
                    std::numeric_limits<int64_t>::max());
    mixer_.SerializeState(archive);
    host_.SerializeState(archive);
    devices_.SerializeState(archive);
    machine_.SerializeState(archive);
}
} // namespace snes::core::gameboy
