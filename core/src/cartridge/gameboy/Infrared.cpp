#include "snes/core/gameboy/Infrared.hpp"

#include <algorithm>

namespace snes::core::gameboy {

void Infrared::Reset() noexcept {
    charge_ = 0;
    input_ = output_ = detected_ = false;
}

void Infrared::Advance(unsigned clocks, bool sensing) noexcept {
    const uint64_t elapsed = uint64_t(clocks) * 2;
    const bool illuminated = sensing && (input_ || output_);
    if (illuminated) {
        charge_ = std::min(uint64_t(32248), charge_ + elapsed);
    } else {
        const uint64_t rest = sensing ? 19900 : 0;
        if (charge_ >= rest)
            charge_ -= std::min(elapsed, charge_ - rest);
        else
            charge_ += elapsed;
    }
    detected_ = illuminated && charge_ >= 20140 && charge_ <= 51640;
}

} // namespace snes::core::gameboy
