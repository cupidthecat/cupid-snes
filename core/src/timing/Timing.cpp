// snes emulator
// core/src/timing/Timing.cpp
// Master clock, scanline, frame, and video-region timing.

// Timing.cpp — SNES dot / scanline / frame timing implementation

#include "snes/core/Timing.hpp"

namespace snes::core {

// Construction & Reset

Timing::Timing(Region region)
    : region_(region)
{
    Reset();
}

void Timing::Reset() {
    hcounter_  = 0;
    vcounter_  = 0;
    field_     = false;  // even field first
    interlace_ = false;
    interlaceInput_ = false;
    vdisp_     = 225;

    vperiod_ = (region_ == Region::NTSC) ? kScanlinesNTSC : kScanlinesPAL;
    hperiod_ = kDotsPerLine;
    previousHPeriod_ = hperiod_;
    previousVPeriod_ = vperiod_;

    masterClocksElapsed_ = 0;
    frameCount_ = 0;
    dramRefreshPosition_ = kDramRefreshPos;
    hdmaSetupPosition_ = 12;
    hdmaSetupFired_ = false;
    halfClockPending_ = false;

    dramRefreshFired_ = false;
    hblankFired_      = false;
    hdmaFired_        = false;
    renderCycleFired_ = false;
    nmiFired_         = false;
    updateNextEvent();
}

void Timing::SetRegion(Region r) noexcept {
    region_ = r;
    // vperiod_ will be recalculated at the next frame boundary (tickScanline
    // when vcounter wraps).  For immediate effect after a region change
    // mid-frame, we also update it here.
    vperiod_ = (region_ == Region::NTSC) ? kScanlinesNTSC : kScanlinesPAL;
    previousVPeriod_ = vperiod_;
}

// Core tick

void Timing::Tick(uint32_t clocks) {
    // Interrupt evaluation retains its two-clock grid. Scanline events only
    // need dispatch when the counter reaches their next scheduled position.

    if (halfClockPending_ && clocks != 0) {
        halfClockPending_ = false;
        tickOnce(1);
        --clocks;
    }
    while (clocks >= 2) {
        tickOnce();
        clocks -= 2;
    }
    // Preserve a half tick so splitting an interval into odd batches does not
    // change event positions or lose clocks from the horizontal counter.
    if (clocks == 1) {
        ++masterClocksElapsed_;
        ++hcounter_;
        halfClockPending_ = true;
    }
}

void Timing::tickOnce(uint8_t clocks) {
    masterClocksElapsed_ += clocks;
    hcounter_ += clocks;
    if (hcounter_ >= nextEventH_)
        dispatchScanlineEvents();

    // Poll NMI and IRQ every four master clocks.
    // Fire after wrap so hcounter_ is valid (0..hperiod-1).
    // Bit 1 set after increment → fires every other tick → every 4 clocks.
    if ((hcounter_ & 2) && onIrqPoll) {
        onIrqPoll(hcounter_, vcounter_, vdisp_, hperiod_);
    }

    // Controller edges share the free-running CPU clock divider.
    if ((masterClocksElapsed_ & 127) == 0 && onJoypadPoll) {
        onJoypadPoll(hcounter_, vcounter_, vdisp_);
    }
}

void Timing::dispatchScanlineEvents() {
    // Per-dot event checks (only fire once per scanline per event)

    if (!hdmaSetupFired_ && vcounter_ == 0 && hcounter_ >= hdmaSetupPosition_) {
        hdmaSetupFired_ = true;
        if (onHdmaSetup) onHdmaSetup();
    }

    // NMI assertion: fires at H=kNmiHPos on the first VBlank scanline
    if (!nmiFired_ && vcounter_ == vdisp_ && hcounter_ >= kNmiHPos) {
        nmiFired_ = true;
        if (onNmiPoint) onNmiPoint();
    }

    // PPU render sampling point (visible lines 1..vdisp-1).
    if (!renderCycleFired_
        && vcounter_ > 0
        && vcounter_ < vdisp_
        && hcounter_ >= kRenderCycle) {
        renderCycleFired_ = true;
        if (onRenderCycle) onRenderCycle(vcounter_);
    }

    // DRAM refresh
    if (!dramRefreshFired_ && hcounter_ >= dramRefreshPosition_) {
        dramRefreshFired_ = true;
        if (onDramRefresh) onDramRefresh();
    }

    // HBlank start (visible scanlines only)
    if (!hblankFired_ && hcounter_ >= kHBlankStart && vcounter_ < vdisp_) {
        hblankFired_ = true;
        if (onHBlank) onHBlank(vcounter_);
    }

    // HDMA transfer point (visible scanlines)
    if (!hdmaFired_ && hcounter_ >= kHdmaPosition && vcounter_ < vdisp_) {
        hdmaFired_ = true;
        if (onHdmaTransfer) onHdmaTransfer(vcounter_);
    }

    // Scanline wrap
    if (hcounter_ >= hperiod_) {
        previousHPeriod_ = hperiod_;
        hcounter_ -= hperiod_;
        tickScanline();
    }

    updateNextEvent();
}

void Timing::updateNextEvent() {
    nextEventH_ = hperiod_;
    const auto include = [this](bool pending, uint16_t position) {
        if (pending && position < nextEventH_)
            nextEventH_ = position;
    };
    include(!hdmaSetupFired_ && vcounter_ == 0, hdmaSetupPosition_);
    include(!nmiFired_ && vcounter_ == vdisp_, kNmiHPos);
    include(!renderCycleFired_ && vcounter_ > 0 && vcounter_ < vdisp_, kRenderCycle);
    include(!dramRefreshFired_, dramRefreshPosition_);
    include(!hblankFired_ && vcounter_ < vdisp_, kHBlankStart);
    include(!hdmaFired_ && vcounter_ < vdisp_, kHdmaPosition);
}

// Scanline transition

void Timing::tickScanline() {
    // Advance V counter
    vcounter_++;

    // Sample interlace at scanline 128 for the remaining field timing.
    if (vcounter_ == 128) {
        interlace_ = interlaceInput_;
        // Recalculate vperiod based on interlace + field.
        // In interlace mode on alternating fields the frame gains one line.
        vperiod_ = (region_ == Region::NTSC) ? kScanlinesNTSC : kScanlinesPAL;
        if (interlace_ && !field_) {
            vperiod_ += 1;  // even field in interlace has +1 scanline
        }
    }

    // Frame wrap
    if (vcounter_ >= vperiod_) {
        previousVPeriod_ = vperiod_;
        vcounter_ = 0;
        field_ = !field_;
        frameCount_++;
        hdmaSetupPosition_ = uint16_t(12 + (masterClocksElapsed_ & 7));
        hdmaSetupFired_ = false;

        // Recalculate vperiod for the new frame.
        vperiod_ = (region_ == Region::NTSC) ? kScanlinesNTSC : kScanlinesPAL;

        // Fire frame-begin callback
        if (onFrameBegin) onFrameBegin();
    }

    // Update H period for the new scanline (short/long line logic).
    updateHPeriod();
    dramRefreshPosition_ = uint16_t(538 - (masterClocksElapsed_ & 7));

    // Reset per-scanline event flags.
    dramRefreshFired_ = false;
    hblankFired_      = false;
    hdmaFired_        = false;
    renderCycleFired_ = false;
    nmiFired_         = false;

    // Fire scanline callback
    if (onScanline) onScanline(vcounter_);

    // VBlank start
    if (vcounter_ == vdisp_) {
        if (onVBlankBegin) onVBlankBegin();
    }
}

// H period calculation

void Timing::updateHPeriod() {
    hperiod_ = kDotsPerLine;  // 1364

    // NTSC non-interlace, odd field, scanline 240: short line (1360).
    if (region_ == Region::NTSC && !interlace_ && field_ && vcounter_ == 240) {
        hperiod_ = kDotsPerLineShort;
    }

    // PAL interlace, odd field, scanline 311: long line (1368).
    if (region_ == Region::PAL && interlace_ && field_ && vcounter_ == 311) {
        hperiod_ = kDotsPerLineLong;
    }
}

// Utilities

uint16_t Timing::HDot() const noexcept {
    uint16_t clocks = hcounter_;
    // Normal and long lines stretch the dots starting at clocks 1292 and 1310.
    // The short NTSC line has uniform four-clock dots.
    if (hperiod_ != kDotsPerLineShort) {
        if (hcounter_ > 1292)
            clocks -= 2;
        if (hcounter_ > 1310)
            clocks -= 2;
    }
    return clocks / 4;
}

uint16_t Timing::HPeriodForScanline(uint16_t scanline) const {
    uint16_t period = kDotsPerLine;

    if (region_ == Region::NTSC && !interlace_ && field_ && scanline == 240) {
        period = kDotsPerLineShort;
    }
    if (region_ == Region::PAL && interlace_ && field_ && scanline == 311) {
        period = kDotsPerLineLong;
    }

    return period;
}

uint32_t Timing::MasterClocksThisFrame() const {
    uint32_t total = 0;
    uint16_t lines = vperiod_;
    for (uint16_t i = 0; i < lines; ++i) {
        total += HPeriodForScanline(i);
    }
    return total;
}

} // namespace snes::core
