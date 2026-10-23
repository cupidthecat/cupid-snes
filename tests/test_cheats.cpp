#include "ConsoleConfig.hpp"
#include "snes/core/SnesCpu.hpp"
#include "snes/core/debug/Cheat.hpp"
#include "state/EmulatorBoards.hpp"

#include <algorithm>
#include <cstdio>
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
  Check(rejected, "Malformed cheat input rejects");
}
void Decoding() {
  constexpr auto console = CheatSystem::Console, handheld = CheatSystem::Handheld;
  const auto decode = [&](std::string_view text, CheatSystem system = CheatSystem::Console) {
    return DecodeCheats(text, system).front();
  };
  Check(decode("7e1234ff") == CheatCode{0x7e1234, 255, {}}, "PAR retains full address and data");
  Check(decode(" 7E1234=AB?CD \r\n") == CheatCode{0x7e1234, 0xcd, 0xab},
        "Conditional syntax keeps compare before replacement");
  // Single encoded address bits give an independent basis for the permutation.
  constexpr std::array<unsigned, 24> destinations{10, 11, 16, 17, 18, 19, 0, 1, 2,  3,  20, 21,
                                                  22, 23, 8,  9,  4,  5,  6, 7, 12, 13, 14, 15};
  for (unsigned bit = 0; bit < 32; ++bit) {
    uint32_t encoded = uint32_t(1) << bit;
    std::string text(8, 'd');
    for (auto it = text.rbegin(); it != text.rend(); ++it, encoded >>= 4)
      *it = "DF4709156BC8A23E"[encoded & 15];
    text.insert(4, "-");
    const auto code = decode(text);
    Check(code.address == (bit < 24 ? uint32_t(1) << destinations[bit] : 0) &&
              code.value == (bit >= 24 ? 1u << (bit - 24) : 0),
          "Each Game Genie bit has the expected destination");
  }
  Check(decode("ABC-DEF", handheld) == CheatCode{0xcde, 0xab, {}}, "Handheld Game Genie address");
  Check(decode("ABC-DEF-123", handheld) == CheatCode{0xcde, 0xab, 0x7e},
        "Handheld compare byte rotates and inverts");
  Check(decode("01AB34C1", handheld) == CheatCode{0xc134, 0xab, {}}, "GameShark byte order");
  for (const auto system : {console, handheld})
    for (uint32_t n = 0; n < 32768; ++n) {
      const CheatCode code{(n * 719) & (system == console ? 0xffffffu : 0xffffu), uint8_t(n),
                           n & 1 ? std::optional<uint8_t>(uint8_t(n >> 2)) : std::nullopt};
      Check(decode(FormatCheat(code, system), system) == code, "Canonical codes round-trip");
    }
  Check(DecodeCheats("7E0000=01+7e0001ff\r\n7E0002=03\n", console).size() == 3,
        "Groups retain line and plus order");
  for (const auto text :
       {"", " ", "+", "7E0000=01+", "7E0000=01+ \n", "7E0000=01++7E0001=02", "7E0000=0G",
        "7E0000=01?02?03", "7E0000=01 junk", "DF47:0915", "7E 0000=01", "0x7e000001", "1234567890"})
    Reject([&] { DecodeCheats(text, console); });
  Reject([&] { DecodeCheats("02AB34C1", handheld); });
  Reject([&] { DecodeCheats("ABC-DEF-G23", handheld); });
  Reject([&] { FormatCheat({0x10000, 0, {}}, handheld); });
}
void Engine() {
  CheatEngine engine;
  std::array<uint8_t, 256> memory{};
  memory[3] = 9;
  const auto read = [&](uint32_t address) { return memory[address & 255]; };
  unsigned writes = 0;
  const auto write = [&](uint32_t address, uint8_t value) {
    memory[address & 255] = value;
    ++writes;
  };
  const std::array codes{CheatCode{3, 10, 9}, CheatCode{4, 99, 8}, CheatCode{5, 4, {}}};
  engine.Set(codes, read, write);
  Check(memory[3] == 10 && memory[4] == 0 && memory[5] == 4 && writes == 2,
        "Conditions sample original bytes at activation");
  engine.Set(codes, read, write);
  Check(writes == 2, "Reapplying an unchanged list preserves original bytes and activation");
  memory[3] = 17;
  memory[4] = 8;
  engine.Refresh(write);
  Check(memory[3] == 10 && memory[4] == 8,
        "Refresh freezes active codes without rechecking conditions");
  const auto before = memory;
  const std::array invalid{CheatCode{0x1000000, 3, {}}};
  Reject([&] { engine.Set(invalid, read, write); });
  Check(memory == before, "Invalid list preserves active execution");
  engine.Set({}, read, write);
  Check(memory[3] == 9 && memory[4] == 8 && memory[5] == 0,
        "Disabling restores only activated bytes");
  const std::array aliases{CheatCode{3, 11, {}}, CheatCode{0x103, 22, {}}};
  engine.Set(aliases, read, write);
  Check(memory[3] == 22, "Later writes win at mirrored addresses");
  engine.Set(std::span(aliases).first(1), read, write);
  Check(memory[3] == 11, "Removing an overlapping code retains the surviving patch");
  engine.Set({}, read, write);
  Check(memory[3] == 9, "Reverse removal restores the original byte through aliases");
}
void Boards() {
  using namespace snes::tests::emulator_state;
  for (unsigned boardIndex = 0; boardIndex < unsigned(Board::Count); ++boardIndex) {
    const auto board = Board(boardIndex);
    for (const auto region : {Region::NTSC, Region::PAL}) {
      auto config = snes::tests::DeterministicConsoleConfig();
      config.cartridgeFirmware = Firmware(board);
      config.regionOverride = region;
      auto machine = std::make_unique<Emulator>(config);
      Load(*machine, board);
      if (machine->CheatsSystem() == CheatSystem::Handheld)
        continue;
      auto &bus = machine->GetBus();
      bus.WramData()[0x1234] = 17;
      bus.SetOpenBus(0x53);
      const std::array code{CheatCode{0x7e1234, 0xa5, 17}};
      machine->SetCheats(code);
      Check(bus.WramData()[0x1234] == 0xa5 && bus.OpenBus() == 0x53,
            "All console boards apply WRAM cheats without replacing the CPU latch");
      bus.WramData()[0x1234] = 0;
      machine->StepFrame();
      Check(bus.WramData()[0x1234] == 0xa5, "Each completed field refreshes the cheat");
      machine->SetCheats({});
      Check(bus.WramData()[0x1234] == 17, "Each board restores its original WRAM byte");
      const auto original = bus.ReadCpu(0x009234);
      const auto patched = uint8_t(original ^ 255);
      const std::array romCode{CheatCode{0x009234, patched, {}}};
      const auto unpatchedState = machine->CaptureState();
      machine->SetCheats(romCode);
      const bool readOnlyPort =
          board == Board::Sdd1 || board == Board::Sa1 || board == Board::Spc7110 ||
          board == Board::SpcRtc || board == Board::Cx4Processor || board == Board::SlotSa1 ||
          board == Board::Competition92 || board == Board::Competition94 ||
          board == Board::Competition92Processor || board == Board::Competition94Processor ||
          board == Board::Disconnected;
      Check(bus.ReadCpu(0x009234) == (readOnlyPort ? original : patched),
            "ROM writes retain the board's programmable-port behavior");
      machine->RestoreState(unpatchedState);
      Check(bus.ReadCpu(0x009234) == (readOnlyPort ? original : patched),
            "State identity uses the inserted image and retains current ROM cheats");
      machine->SetCheats({});
      Check(bus.ReadCpu(0x009234) == original, "Disabling restores the ROM byte");
      if (board != Board::Disconnected) {
        bus.Write(0x009234, patched);
        Check(bus.ReadCpu(0x009234) == original, "Ordinary CPU writes retain ROM protection");
      }
    }
  }
}
void Handheld() {
  using namespace snes::tests::handheld;
  MachineStateFixture fixture(0);
  auto &machine = fixture.machine;
  machine.WriteMemory(0xff50, 1);
  machine.WriteMemory(0xc123, 8);
  const std::array codes{CheatCode{0xc123, 9, 8}, CheatCode{0xc123, 10, {}},
                         CheatCode{0x0100, 0xdb, {}}};
  machine.SetCheats(codes);
  Check(machine.ReadMemory(0xc123) == 9, "Handheld reads use the first matching code");
  machine.WriteMemory(0xc123, 7);
  Check(machine.ReadMemory(0xc123) == 10, "Handheld conditional reads use the current byte");
  Check(machine.ReadMemory(0x0100) == 0xdb, "Handheld ROM reads can be substituted");
  machine.SetCheats({});
  Check(machine.ReadMemory(0xc123) == 7, "Handheld substitution leaves underlying RAM untouched");
}
void Identity() {
  using namespace snes::tests::emulator_state;
  auto plain = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  auto patched = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Load(*plain, Board::Lo);
  Load(*patched, Board::Lo);
  const auto original = plain->GetBus().ReadCpu(0x009234);
  const std::array codes{CheatCode{0x009234, uint8_t(original ^ 255), {}}};
  patched->SetCheats(codes);
  const auto checkpoint = patched->CaptureState();
  plain->RestoreState(checkpoint);
  Check(plain->GetBus().ReadCpu(0x009234) == original,
        "A first capture after ROM patching retains the inserted image identity");
  patched->SetCheats({});
  Check(patched->GetBus().ReadCpu(0x009234) == original, "ROM bytes restore after a first capture");
}
} // namespace
int main() {
  try {
    Decoding();
    Engine();
    Boards();
    Handheld();
    Identity();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
