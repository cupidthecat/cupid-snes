#include "snes/core/gameboy/Oam.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

using snes::core::gameboy::Oam;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void ScanBusCorpus() {
  struct Scan {
    uint8_t row, blocked;
  };
  constexpr std::array<Scan, 28> scans = {
      {{255, 0}, {255, 2}, {255, 3}, {0, 0},   {0, 1},   {0, 3},   {8, 3},
       {16, 3},  {24, 3},  {32, 3},  {40, 3},  {48, 3},  {56, 3},  {64, 3},
       {72, 3},  {80, 3},  {88, 3},  {96, 3},  {104, 3}, {112, 3}, {120, 3},
       {128, 3}, {136, 3}, {144, 3}, {152, 3}, {152, 1}, {160, 1}, {255, 3}}};
  constexpr std::array<uint8_t, 28> addresses = {0,   1,   2,   3,   4,   5,   6,   7,   32,  33,
                                                 34,  35,  36,  37,  38,  39,  152, 153, 154, 155,
                                                 156, 157, 158, 159, 160, 167, 254, 255};
  constexpr std::array<uint64_t, 2> expected = {0x65d36676a66e2c83ull, 0x0682f0959e33bb63ull};
  uint32_t entropy = 0x4f414d;
  const auto byte = [&] {
    entropy = entropy * 1664525 + 1013904223;
    return uint8_t(entropy >> 24);
  };
  for (unsigned model = 0; model < 2; ++model) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto scan : scans)
      for (unsigned operation = 0; operation < 3; ++operation)
        for (const auto address : addresses) {
          Oam memory;
          memory.Reset(model != 0);
          for (unsigned i = 0; i < 160; ++i)
            memory.Write(uint8_t(i), byte());
          const auto value = byte();
          uint8_t returned = 0;
          if (operation == 0)
            returned = memory.ReadCpu(address, scan.row, scan.blocked);
          else if (operation == 1)
            memory.WriteCpu(address, value, scan.row, (scan.blocked & 2) != 0);
          else
            memory.AddressBus(scan.row);
          hash = (hash ^ returned) * 1099511628211ull;
          for (unsigned i = 0; i < 160; ++i)
            hash = (hash ^ memory.Read(uint8_t(i))) * 1099511628211ull;
        }
    Check(hash == expected[model],
          "All 4704 scan-bus fixtures retain their return values and complete object memory");
  }
}

void ResetAndUnconnectedCells() {
  Oam memory;
  memory.Reset(false);
  memory.Write(0, 0x5a);
  memory.Write(255, 0x71);
  Check(memory.Read(0) == 0x5a && memory.Read(255) == 0,
        "Raw object memory connects only its 160 physical bytes");
  memory.WriteCpu(255, 0x71, 0, false);
  Check(memory.Read(7) == 0x71,
        "A CPU write to an unconnected address can drive the first scanner row");
  memory.Reset(true);
  for (unsigned i = 0; i < 160; ++i)
    Check(memory.Read(uint8_t(i)) == 0, "Reset clears every physical object cell");
}
} // namespace

int main() {
  try {
    ScanBusCorpus();
    ResetAndUnconnectedCells();
    std::puts("Handheld object memory and scanner bus checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
