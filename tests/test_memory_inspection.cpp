#include "ConsoleConfig.hpp"
#include "snes/core/debug/MemoryInspection.hpp"
#include "state/EmulatorBoards.hpp"

#include <cstdio>
#include <set>
#include <stdexcept>

using namespace snes::core;
namespace {
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class Function> void Reject(Function run) {
  bool rejected = false;
  try {
    run();
  } catch (const std::exception &) {
    rejected = true;
  }
  Check(rejected, "Invalid inspection range rejects");
}
struct Input final : IInputProvider {
  unsigned polls = 0, fields = 0;
  InputState Poll(uint64_t) override {
    ++polls;
    return {.a = true};
  }
  void AdvanceFrame() noexcept override {
    ++fields;
  }
};
void Boards() {
  using namespace snes::tests::emulator_state;
  for (unsigned n = 0; n < unsigned(Board::Count); ++n) {
    const auto board = Board(n);
    for (const auto region : {Region::NTSC, Region::PAL}) {
      auto config = snes::tests::DeterministicConsoleConfig();
      config.regionOverride = region;
      config.cartridgeFirmware = Firmware(board);
      auto machine = std::make_unique<Emulator>(config);
      Input input;
      machine->AttachInputProvider(&input);
      Load(*machine, board);
      machine->StepFrame();
      const auto before = machine->CaptureState();
      const auto clock = machine->CurrentMasterCycles(), frame = machine->CurrentFrame();
      const auto polls = input.polls, fields = input.fields;
      std::set<std::string> keys;
      const auto regions = MemoryInspector::Regions(*machine);
      Check(regions.size() >= 7, "All machines expose console storage and cartridge ROM");
      for (const auto &memory : regions) {
        Check(keys.insert(memory.key).second && !memory.label.empty() && memory.size,
              "Region identifiers are unique and memory sizes are nonzero");
        const auto bytes = MemoryInspector::Read(*machine, memory.key, 0, memory.size);
        Check(bytes.size() == memory.size, "A physical region can be read in full");
        const auto last = MemoryInspector::Read(*machine, memory.key, memory.size - 1, 1);
        Check(last.front() == bytes.back(), "Partial reads retain physical byte ordering");
        Check(MemoryInspector::Read(*machine, memory.key, memory.size, 0).empty(),
              "An empty read at the end is valid");
        Reject([&] { MemoryInspector::Read(*machine, memory.key, memory.size, 1); });
        Reject([&] { MemoryInspector::Read(*machine, memory.key, 1, SIZE_MAX); });
      }
      Reject([&] { MemoryInspector::Read(*machine, "missing", 0, 1); });
      Check(machine->CurrentMasterCycles() == clock && machine->CurrentFrame() == frame &&
                input.polls == polls && input.fields == fields,
            "Inspection never advances clocks, fields or host input");
      Check(machine->CaptureState() == before, "Inspection retains every serialized machine byte");
      if (board == Board::Sa1 || board == Board::SlotSa1)
        Check(keys.contains("sa1-iram"), "SA-1 internal RAM is exposed");
      if (board == Board::SuperFx)
        Check(keys.contains("superfx-cache"),
              "Graphics cache is exposed without CPU register reads");
      if (board == Board::Handheld || board == Board::Handheld2)
        for (const auto *key : {"handheld-rom", "handheld-save", "handheld-wram", "handheld-hram",
                                "handheld-vram", "handheld-oam", "handheld-wave"})
          Check(keys.contains(key), "Handheld storage retains distinct physical regions");
    }
  }
}
void Bytes() {
  using namespace snes::tests::emulator_state;
  for (uint32_t vramSize : {65536u, 131072u}) {
    auto config = snes::tests::DeterministicConsoleConfig();
    config.ppu.vramBytes = vramSize;
    auto machine = std::make_unique<Emulator>(config);
    Load(*machine, Board::Lo);
    machine->GetPpu().VramData()[0] = 0x1234;
    machine->GetPpu().VramData()[vramSize / 2 - 1] = 0xabcd;
    const auto bytes = MemoryInspector::Read(*machine, "vram", 0, vramSize);
    Check(bytes[0] == 0x34 && bytes[1] == 0x12 && bytes[vramSize - 2] == 0xcd &&
              bytes.back() == 0xab,
          "Both VRAM sizes use little-endian physical bytes including the final word");
    machine->GetPpu().CgramData()[1] = 0x3b4c;
    Check(MemoryInspector::Read(*machine, "cgram", 2, 2) == std::vector<uint8_t>({0x4c, 0x3b}),
          "Color memory is independent of host word byte order");
    machine->GetBus().WramData()[0x1234] = 0x5a;
    Check(MemoryInspector::Read(*machine, "wram", 0x1234, 1).front() == 0x5a,
          "Work RAM uses physical offsets rather than serial data ports");
  }
  auto machine = std::make_unique<Emulator>();
  Check(MemoryInspector::Regions(*machine).empty(), "An idle machine has no inspectable regions");
  Reject([&] { MemoryInspector::Read(*machine, "wram", 0, 1); });
}
} // namespace
int main() {
  try {
    Boards();
    Bytes();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
