#include "ConsoleConfig.hpp"
#include "debug/MemoryTools.hpp"
#include "state/EmulatorBoards.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

using namespace snes::frontend;
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
  Check(rejected, "Unsupported memory tool operation rejects");
}
void Console() {
  using namespace snes::tests::emulator_state;
  auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Load(*machine, Board::Lo);
  auto *ram = machine->GetBus().WramData();
  std::fill_n(ram, MemoryBus::WramSize, 0);
  constexpr uint8_t bytes[]{0x98, 0xba, 0xdc, 0xfe};
  std::copy(std::begin(bytes), std::end(bytes), ram + 0x1234);
  MemoryTools tools(*machine);
  Check(tools.Selected().key == "wram" && tools.CanCreateCheat(),
        "Console tools start with directly mapped work RAM");
  for (unsigned width = 1; width <= 4; ++width)
    for (bool big : {false, true})
      for (bool sign : {false, true}) {
        tools.Start({width, sign, big});
        const auto result = tools.Search().Results(0x1234, 1).front();
        const auto entry = tools.MakeCheat(result, "Selected value");
        const auto codes = DecodeCheats(entry.codes, CheatSystem::Console);
        Check(!entry.enabled && codes.size() == width,
              "Search creates a disabled group for review");
        for (unsigned index = 0; index < width; ++index)
          Check(codes[index].address == 0x7e1234 + index && codes[index].value == bytes[index],
                "Signed and endian search values produce the original physical byte sequence");
      }
  tools.Filter({RamSearch::Relation::NotEqual, RamSearch::Operand::Constant, 0});
  const auto count = tools.Search().Count();
  Check(count != 0, "Search model captures live machine storage");
  Reject([&] { tools.Select("missing"); });
  Check(tools.Search().Count() == count, "Invalid selection retains the previous candidates");
  tools.Select("vram");
  Check(!tools.Search().Active() && !tools.CanCreateCheat(),
        "Changing physical region clears incompatible search history");
  tools.Start({});
  Reject([&] { tools.MakeCheat(tools.Search().Results(0, 1).front(), "Invalid mapping"); });
  Check(!tools.Read(0, 32).empty(),
        "Physical graphics memory remains readable without a CPU address mapping");
}
void Handheld() {
  using namespace snes::tests::emulator_state;
  auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Load(*machine, Board::Handheld);
  MemoryTools tools(*machine);
  Check(tools.Selected().key == "handheld-wram" && tools.CanCreateCheat(),
        "Handheld tools choose the correct cheat address space");
  tools.Start({2});
  auto entry = tools.MakeCheat(tools.Search().Results(0x123, 1).front(), "Handheld value");
  const auto codes = DecodeCheats(entry.codes, CheatSystem::Handheld);
  Check(codes.size() == 2 && codes[0].address == 0xc123 && codes[1].address == 0xc124,
        "Handheld multi-byte results retain handheld addresses");
  machine->SetCheats(codes);
  tools.Select("wram");
  Check(!tools.CanCreateCheat(), "Console storage cannot be mistaken for a handheld cheat address");
  tools.Select("handheld-hram");
  tools.Start({});
  entry = tools.MakeCheat(tools.Search().Results(126, 1).front(), "Last byte");
  Check(DecodeCheats(entry.codes, CheatSystem::Handheld).front().address == 0xfffe,
        "The last handheld high-RAM byte stays within its real address range");
}
} // namespace
int main() {
  try {
    Console();
    Handheld();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
