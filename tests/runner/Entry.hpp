#pragma once

// Declare shared core types before a fixture's entry-point macro is active.
#include "snes/core/Emulator.hpp"

#include <type_traits>

namespace snes::tests {
// The fixture receives its check name as argv[0], followed by its original arguments.
template <auto Entry> int RunEntry(int argc, char** argv) {
    if constexpr (std::is_invocable_r_v<int, decltype(Entry), int, char**>)
        return Entry(argc, argv);
    else
        return Entry();
}
} // namespace snes::tests
