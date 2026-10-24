#include "snes/core/AutoJoypad.hpp"
#include "snes/core/ControllerPorts.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void ControllerPorts::SerializeState(detail::StateArchive& archive) {
    for (auto& port : ports_)
        archive.Object(port, [](auto& fields, auto& value) {
            fields.Enumeration(value.device, ControllerDevice::None, ControllerDevice::MacsRifle);
            for (auto& player : value.players)
                fields.Bounded(player, -1, 7);
            fields.Array(value.pads);
            for (auto& position : value.positions)
                fields.Bounded(position, 0u, 32u);
            fields.Number(value.mousePacket);
            fields.Bounded(value.sensitivity, uint8_t{0}, uint8_t{2});
            fields.Array(value.guns, [](auto& gunFields, auto& gun) {
                gunFields.Number(gun.x);
                gunFields.Number(gun.y);
                gunFields.Number(gun.trigger);
                gunFields.Number(gun.cursor);
                gunFields.Number(gun.turbo);
                gunFields.Number(gun.pause);
                gunFields.Number(gun.start);
                gunFields.Number(gun.offscreen);
            });
            fields.Number(value.scopePacket);
            fields.Number(value.justifierPacket);
            fields.Number(value.selectSecond);
            fields.Number(value.scopeTriggerLock);
            fields.Number(value.scopePauseLock);
            fields.Number(value.scopeTurbo);
            fields.Number(value.scopeTurboPressed);
            fields.Number(value.scopeOffscreen);
        });
    archive.Number(beamFired_);
    archive.Number(beamPulse_);
    archive.Number(latch_);
    archive.Number(pio_);
    archive.Number(vdisp_);
}

void AutoJoypad::SerializeState(detail::StateArchive& archive) {
    archive.Number(autoJoypadPoll_);
    archive.Bounded(counter_, uint8_t{0}, uint8_t{33});
    ports_.SerializeState(archive);
    archive.Number(manualLatch_);
    archive.Number(autoLatch_);
    archive.Bounded(port1Data_, uint8_t{0}, uint8_t{3});
    archive.Bounded(port2Data_, uint8_t{0}, uint8_t{3});
    archive.Number(joy1_);
    archive.Number(joy2_);
    archive.Number(joy3_);
    archive.Number(joy4_);
}
} // namespace snes::core
