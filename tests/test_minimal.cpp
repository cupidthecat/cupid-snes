// Minimal executable sanity check.

#include "snes/core/Ppu.hpp"
#include <cstdio>

int main() {
    std::printf("Before Ppu\n");
    snes::core::Ppu ppu;
    std::printf("After Ppu\n");
    return 0;
}
