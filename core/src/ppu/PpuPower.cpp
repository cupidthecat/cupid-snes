#include "snes/core/PowerOnState.hpp"
#include "snes/core/Ppu.hpp"

#include <algorithm>

namespace snes::core {

void Ppu::InitializeScanlineState(bool retainMemory) {
    if (!retainMemory) {
        std::fill_n(vram_.get(), VramStorageWords, uint16_t{});
        cgram_.fill(0);
        oam_.fill(0);
        for (unsigned object = 0; object < 128; ++object)
            oam_[object * 4 + 1] = 0xff;
        objects_.fill({});
    }
    io_ = IO{};
    io_.vramIncrementMode = false;
    io_.vramIncrementSize = 0;
    latch_ = Latch{};
    latch_.ppu1.mdr = latch_.ppu2.mdr = 0;
}

void Ppu::InitializePowerOnState(PowerOnState& state, bool retainVram) {
    if (ScanlineModel()) {
        InitializeScanlineState(retainVram);
        return;
    }
    if (!retainVram)
        state.Fill({reinterpret_cast<uint8_t*>(vram_.get()), VramStorageWords * sizeof(uint16_t)});
    oam_.fill(0);
    objects_.fill({});
    io_ = IO{};
    latch_ = Latch{};

    const auto bit = [&](uint64_t fallback = 0) { return (state.Value(fallback) & 1) != 0; };
    latch_.ppu1.mdr = uint8_t(state.Value(0xff));
    latch_.ppu2.mdr = uint8_t(state.Value(0xff));
    latch_.vram = uint16_t(state.Value());
    latch_.oam = uint8_t(state.Value());
    latch_.cgram = uint8_t(state.Value());
    latch_.ppu1.bgofs = uint8_t(state.Value());
    latch_.ppu2.bgofs = uint8_t(state.Value() & 7);
    latch_.mode7 = uint8_t(state.Value());

    io_.oamBaseAddress = uint16_t(state.Value() & 0x3fe);
    io_.oamAddress = uint16_t(state.Value() & 0x3ff);
    io_.oamPriority = bit();
    io_.mode7.hoffset = uint16_t(state.Value());
    io_.mode7.voffset = uint16_t(state.Value());
    io_.vramIncrementMode = bit(1);
    io_.vramMapping = uint8_t(state.Value() & 3);
    io_.vramAddress = uint16_t(state.Value());
    io_.mode7.repeat = uint8_t(state.Value() & 3);
    io_.mode7.vflip = bit();
    io_.mode7.hflip = bit();
    io_.mode7.a = uint16_t(state.Value());
    io_.mode7.b = uint16_t(state.Value());
    io_.mode7.c = uint16_t(state.Value());
    io_.mode7.d = uint16_t(state.Value());
    io_.mode7.x = uint16_t(state.Value());
    io_.mode7.y = uint16_t(state.Value());
    io_.cgramAddress = uint8_t(state.Value());
    io_.cgramAddressLatch = bit();
    io_.extbg = bit();
    io_.pseudoHires = bit();

    io_.mosaic.size = uint8_t(1 + (state.Value() & 15));
    for (auto* bg : {&io_.bg1, &io_.bg2, &io_.bg3, &io_.bg4}) {
        bg->tiledataAddress = uint16_t((state.Value() & 15) << 12);
        bg->screenAddress = uint16_t((state.Value() & 0xfc) << 8);
        bg->screenSize = uint8_t(state.Value() & 3);
        bg->tileSize = bit();
        bg->aboveEnable = bit();
        bg->belowEnable = bit();
        bg->hoffset = uint16_t(state.Value());
        bg->voffset = uint16_t(state.Value());
        bg->mosaicEnable = bit();
    }
    io_.obj.aboveEnable = bit();
    io_.obj.belowEnable = bit();
    io_.obj.interlace = bit();
    io_.obj.baseSize = uint8_t(state.Value() & 7);
    io_.obj.nameselect = uint8_t(state.Value() & 3);
    io_.obj.tiledataAddress = uint16_t((state.Value() & 7) << 13);

    // Window control flags use the full entropy value.
    for (auto* window :
         {&io_.bg1.window, &io_.bg2.window, &io_.bg3.window, &io_.bg4.window, &io_.obj.window}) {
        window->oneEnable = state.Value() != 0;
        window->oneInvert = state.Value() != 0;
        window->twoEnable = state.Value() != 0;
        window->twoInvert = state.Value() != 0;
        window->mask = uint8_t(state.Value() & 3);
        window->aboveEnable = state.Value() != 0;
        window->belowEnable = state.Value() != 0;
    }
    io_.col.window.oneEnable = state.Value() != 0;
    io_.col.window.oneInvert = state.Value() != 0;
    io_.col.window.twoEnable = state.Value() != 0;
    io_.col.window.twoInvert = state.Value() != 0;
    io_.col.window.mask = uint8_t(state.Value() & 3);
    io_.col.window.aboveMask = uint8_t(state.Value() & 3);
    io_.col.window.belowMask = uint8_t(state.Value() & 3);
    io_.window.oneLeft = uint8_t(state.Value());
    io_.window.oneRight = uint8_t(state.Value());
    io_.window.twoLeft = uint8_t(state.Value());
    io_.window.twoRight = uint8_t(state.Value());

    state.Fill({reinterpret_cast<uint8_t*>(cgram_.data()), cgram_.size() * sizeof(uint16_t)});
    for (auto& color : cgram_)
        color &= 0x7fff;
    io_.col.blendMode = bit();
    io_.col.directColor = bit();
    io_.col.mathMode = bit();
    io_.col.halve = bit();
    for (auto layer : {BG1, BG2, BG3, BG4})
        io_.col.enable[layer] = bit();
    io_.col.enable[OBJ1] = io_.col.enable[OBJ2] = bit();
    io_.col.enable[COL] = bit();
    const auto blue = state.Value() & 31;
    const auto green = state.Value() & 31;
    const auto red = state.Value() & 31;
    io_.col.fixedColor = uint16_t((blue << 10) | (green << 5) | red);
}

} // namespace snes::core
