#include <cstdio>
#include <string_view>

namespace snes::tests {
#define SNES_OPTIMIZED_CHECK(name) int Run_##name(int argc, char** argv);
#include "Cases.inc"
#undef SNES_OPTIMIZED_CHECK

struct Check {
    std::string_view name;
    int (*run)(int, char**);
};

constexpr Check checks[]{
#define SNES_OPTIMIZED_CHECK(name) {#name, Run_##name},
#include "Cases.inc"
#undef SNES_OPTIMIZED_CHECK
};
} // namespace snes::tests

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Expected an optimized check name\n");
        return 2;
    }
    if (std::string_view(argv[1]) == "--list") {
        for (const auto& check : snes::tests::checks)
            std::puts(check.name.data());
        return 0;
    }
    for (const auto& check : snes::tests::checks)
        if (check.name == argv[1])
            return check.run(argc - 1, argv + 1);
    std::fprintf(stderr, "Unknown optimized check: %s\n", argv[1]);
    return 2;
}
