#include "snes/core/IrqController.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void Timing::SerializeState(detail::StateArchive& archive) {
    archive.Expect(static_cast<uint8_t>(region_));
    archive.Expect(cpuRevision_);
    archive.Expect(renderCycle_);
    archive.Number(interlace_);
    archive.Number(interlaceInput_);
    archive.Number(vdisp_);
    const auto horizontal = archive.Number(hcounter_);
    const auto vertical = archive.Number(vcounter_);
    archive.Number(field_);
    const auto hperiod = archive.Number(hperiod_);
    const auto vperiod = archive.Number(vperiod_);
    const auto previousH = archive.Number(previousHPeriod_);
    const auto previousV = archive.Number(previousVPeriod_);
    const auto validH = [](uint16_t period) {
        return period == kDotsPerLineShort || period == kDotsPerLine || period == kDotsPerLineLong;
    };
    const auto base = region_ == Region::NTSC ? kScanlinesNTSC : kScanlinesPAL;
    archive.Validate(validH(hperiod) && validH(previousH) && vperiod >= base &&
                         vperiod <= base + 1 && previousV >= base && previousV <= base + 1 &&
                         horizontal < hperiod && vertical < vperiod,
                     "Invalid machine timing counters");
    // Register writes can move a pending deadline behind the current counter.
    archive.Bounded(nextEventH_, uint16_t{2}, hperiod);
    const auto elapsed = archive.Number(masterClocksElapsed_);
    archive.Number(frameCount_);
    archive.Bounded(dramRefreshPosition_, uint16_t(cpuRevision_ == 1 ? 530 : 531),
                    uint16_t(cpuRevision_ == 1 ? 530 : 538));
    archive.Bounded(hdmaSetupPosition_, uint16_t{12}, uint16_t{20});
    archive.Number(hdmaSetupFired_);
    const auto half = archive.Number(halfClockPending_);
    archive.Validate((horizontal & 1) == half && (elapsed & 1) == half,
                     "Invalid machine timing half clock");
    archive.Number(dramRefreshFired_);
    archive.Number(hblankFired_);
    archive.Number(hdmaFired_);
    archive.Number(renderCycleFired_);
    archive.Number(nmiFired_);
}

void IrqController::SerializeState(detail::StateArchive& archive) {
    archive.Object(*this, [](auto& fields, auto& irq) {
        fields.Number(irq.nmiEnable_);
        fields.Number(irq.nmiLine_);
        fields.Number(irq.nmiValid_);
        fields.Number(irq.nmiHold_);
        fields.Number(irq.nmiTransition_);
        fields.Number(irq.hirqEnable_);
        fields.Number(irq.virqEnable_);
        fields.Number(irq.irqEnable_);
        fields.Validate(irq.irqEnable_ == (irq.hirqEnable_ || irq.virqEnable_),
                        "Invalid interrupt controller enables");
        fields.Number(irq.irqLine_);
        fields.Number(irq.irqValid_);
        fields.Number(irq.irqHold_);
        fields.Number(irq.irqTransition_);
        fields.Bounded(irq.htimeRaw_, uint16_t{0}, uint16_t{511});
        fields.Number(irq.htimeMc_);
        fields.Validate(irq.htimeMc_ == (irq.htimeRaw_ + 1) * 4, "Invalid interrupt timer target");
        fields.Bounded(irq.vtime_, uint16_t{0}, uint16_t{511});
        fields.Number(irq.irqLock_);
    });
}
} // namespace snes::core
