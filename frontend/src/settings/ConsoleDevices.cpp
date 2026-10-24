#include "ConsoleDevices.hpp"
#include <stdexcept>

namespace snes::frontend {
void ConsoleDevices::Validate() const {
    using D = core::ControllerDevice;
    for (const auto device : ports)
        if (device < D::None || device > D::MacsRifle)
            throw std::invalid_argument("Unknown controller device");
    if (core::IsLightGun(ports[0]))
        throw std::invalid_argument("Connect light guns to port 2 for beam-counter input");
    if (ports[0] == D::Mouse && ports[1] == D::Mouse)
        throw std::invalid_argument(
            "The frontend supports one host mouse; choose one console port");
    if (ports[0] == D::Mouse && core::IsLightGun(ports[1]))
        throw std::invalid_argument(
            "The host pointer cannot control a relative mouse and a light gun together");
    if (expansion < core::Emulator::ExpansionDevice::None ||
        expansion > core::Emulator::ExpansionDevice::Satellaview)
        throw std::invalid_argument("Unknown console expansion device");
}

ConsoleDevices DeviceOverrides::Resolve(ConsoleDevices saved) const {
    for (size_t port = 0; port < ports.size(); ++port)
        if (ports[port])
            saved.ports[port] = *ports[port];
    if (expansion)
        saved.expansion = *expansion;
    saved.Validate();
    return saved;
}
} // namespace snes::frontend
