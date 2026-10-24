#include "snes/core/detail/StateArchive.hpp"
#include "snes/core/gameboy/Host.hpp"
#include "snes/core/gameboy/Oam.hpp"
#include "snes/core/gameboy/Video.hpp"

namespace snes::core::gameboy {
void Host::SerializeState(detail::StateArchive& archive) {
    archive.Array(video_);
    archive.Array(packets_, [](auto& fields, auto& packet) { fields.Array(packet); });
    archive.Array(receiving_);
    archive.Array(command_);
    archive.Array(controllers_);
    archive.Bounded(readAddress_, uint16_t{0}, uint16_t{511});
    archive.Bounded(readBank_, uint8_t{0}, uint8_t{3});
    archive.Bounded(writeBank_, uint8_t{0}, uint8_t{3});
    archive.Number(x_);
    archive.Number(y_);
    archive.Bounded(packetHead_, uint8_t{0}, uint8_t{63});
    archive.Bounded(packetCount_, uint8_t{0}, uint8_t{64});
    archive.Bounded(receivedBits_, uint8_t{0}, uint8_t{128});
    archive.Number(control_);
    archive.Bounded(multiplayer_, uint8_t{0}, uint8_t{3});
    archive.Bounded(controller_, uint8_t{0}, uint8_t{3});
    const auto divider = archive.Number(divider_);
    archive.Validate(divider == 4 || divider == 5 || divider == 7 || divider == 9,
                     "Invalid handheld adapter divider");
    archive.Number(controllerLocked_);
    archive.Number(receivingPacket_);
    archive.Number(released_);
}

void Oam::SerializeState(detail::StateArchive& archive) {
    archive.Expect(secondModel_);
    archive.Array(bytes_);
}

void Video::SerializeState(detail::StateArchive& archive) {
    archive.Expect(secondModel_);
    archive.Array(vram_);
    oam_.SerializeState(archive);
    archive.Array(registers_);
    archive.Object(fifo_, [](auto& fields, auto& pixels) {
        for (auto& pixel : pixels)
            fields.Bounded(pixel, uint8_t{0}, uint8_t{3});
    });
    archive.Array(objects_, [](auto& fields, auto& object) {
        fields.Bounded(object.index, uint8_t{0}, uint8_t{39});
        fields.Number(object.x);
        fields.Number(object.y);
        fields.Number(object.flags);
        fields.Bounded(object.address, uint16_t{0}, uint16_t{4080});
        fields.Number(object.low);
        fields.Number(object.high);
        for (auto& pixel : object.pixels)
            fields.Bounded(pixel, uint8_t{0}, uint8_t{3});
        fields.Number(object.fetched);
    });
    archive.Bounded(dot_, uint16_t{0}, uint16_t{455});
    archive.Bounded(transferClocks_, uint16_t{0}, uint16_t{456});
    archive.Bounded(hblankDot_, uint16_t{0}, uint16_t{455});
    archive.Bounded(line_, uint8_t{0}, uint8_t{153});
    archive.Bounded(ly_, uint8_t{0}, uint8_t{153});
    archive.Bounded(mode_, uint8_t{0}, uint8_t{3});
    const auto irq = archive.Number(irqMode_);
    archive.Validate(irq <= 5 || irq == 0xff, "Invalid handheld LCD interrupt mode");
    archive.Bounded(blocked_, uint8_t{0}, uint8_t{15});
    archive.Bounded(comparisonLine_, -1, 153);
    archive.Bounded(fifoHead_, uint8_t{0}, uint8_t{7});
    archive.Bounded(fifoSize_, uint8_t{0}, uint8_t{8});
    archive.Bounded(fetchPhase_, uint8_t{0}, uint8_t{QueueWrite});
    archive.Number(fetchX_);
    archive.Number(tile_);
    archive.Number(low_);
    archive.Number(high_);
    archive.Bounded(x_, uint8_t{0}, uint8_t{160});
    archive.Bounded(position_, int16_t{-16}, int16_t{160});
    archive.Bounded(mapAddress_, uint16_t{0}, uint16_t{8191});
    archive.Bounded(dataAddress_, uint16_t{0}, uint16_t{8191});
    archive.Bounded(scrollFine_, uint8_t{0}, uint8_t{7});
    archive.Bounded(objectCount_, uint8_t{0}, uint8_t{10});
    archive.Bounded(objectStall_, uint8_t{0}, uint8_t{6});
    archive.Bounded(fetchingObject_, uint8_t{0}, uint8_t{9});
    archive.Number(windowLine_);
    archive.Bounded(windowPhase_, uint8_t{0}, uint8_t{3});
    archive.Number(objectPending_);
    archive.Number(objectAborted_);
    archive.Number(windowStarting_);
    archive.Number(insertBackground_);
    archive.Number(warm_);
    archive.Number(coincidence_);
    archive.Number(irqLine_);
    archive.Number(stopped_);
    archive.Number(windowReached_);
    archive.Number(windowActive_);
    archive.Number(lateWindow_);
    archive.Number(windowWrite_);
    archive.Number(windowCheck_);
    archive.Number(windowReset_);
    archive.Number(sleeping_);
    archive.Number(vramRelease_);
    archive.Number(dmaSource_);
    const auto dma = archive.Number(dmaPosition_);
    archive.Validate(dma <= 0xa1 || dma == 0xff, "Invalid handheld LCD DMA position");
    archive.Number(scanY_);
    archive.Number(scanX_);
}
} // namespace snes::core::gameboy
