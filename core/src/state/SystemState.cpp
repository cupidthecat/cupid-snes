#include "snes/core/PowerOnState.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void PowerOnState::SerializeState(detail::StateArchive& archive) {
    archive.Enumeration(mode_, EntropyMode::None, EntropyMode::High);
    archive.Number(state_);
    const auto increment = archive.Number(increment_);
    archive.Validate((increment & 1) != 0, "Invalid startup sequence in machine state");
}
} // namespace snes::core
