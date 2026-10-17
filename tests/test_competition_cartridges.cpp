#include "snes/core/CompetitionBoard.hpp"
#include "snes/core/Emulator.hpp"
#include "snes/core/SnesCpu.hpp"
#include "snes/core/Timing.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace snes::core;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

CompetitionBoard::Images Images() {
  CompetitionBoard::Images images;
  for (unsigned i = 0; i < images.size(); ++i)
    images[i].assign(i == 2 ? 0x18000 : 0x10000, uint8_t(0x10 + i * 0x11));
  images[2][0x8000] = 0x72;
  auto &boot = images[0];
  std::fill_n(boot.begin() + 0x7fc0, 64, 0);
  std::fill_n(boot.begin() + 0x7fc0, 21, ' ');
  std::copy_n("COMPETITION FIXTURE", 19, boot.begin() + 0x7fc0);
  boot[0x7fd5] = 0x20;
  boot[0x7fd7] = 6;
  boot[0x7fd9] = 1;
  boot[0x7fdc] = boot[0x7fdd] = 0xff;
  boot[0x7ffd] = 0x80;
  return images;
}

uint32_t Control(CompetitionBoard::Board board) {
  return board == CompetitionBoard::Board::CampusChallenge92 ? 0xe00000 : 0x206000;
}
uint32_t Status(CompetitionBoard::Board board) {
  return board == CompetitionBoard::Board::CampusChallenge92 ? 0xc00000 : 0x106000;
}

void Mappings() {
  CompetitionBoard campus({CompetitionBoard::Board::CampusChallenge92}, Images());
  Check(campus.Read(0x008000, 0xa5) == 0x10, "The 1992 boot chip is selected at reset");
  for (const auto choice :
       std::array<std::pair<uint8_t, uint8_t>, 3>{{{9, 0x21}, {5, 0x32}, {3, 0x43}}}) {
    campus.Write(0xe00000, choice.first);
    Check(campus.Read(0x008000, 0xa5) == choice.second && campus.Read(0x808000, 0xa5) == 0x10,
          "The 1992 low window selects games while the high window retains the boot chip");
  }
  campus.Write(0xe00001, 9);
  Check(campus.Read(0x008000, 0xa5) == 0x43,
        "Addresses adjacent to the selection register do not change the active game");
  campus.Write(0xe00000, 0xff);
  Check(campus.Read(0x008000, 0xa5) == 0x10 && campus.Read(0x408000, 0xa5) == 0xa5,
        "Unknown selections use the boot chip without extending the ROM window");
  Check(campus.Read(0x208000, 0xa5) == 0 && campus.Read(0xa0ffff, 0xa5) == 0x84,
        "The 1992 mapped DSP window selects status at every address");

  CompetitionBoard powerfest({CompetitionBoard::Board::PowerFest94}, Images());
  for (const auto choice :
       std::array<std::pair<uint8_t, uint8_t>, 3>{{{9, 0x21}, {12, 0x32}, {10, 0x43}}}) {
    powerfest.Write(0x206000, choice.first);
    Check(powerfest.Read(0xc00000, 0xa5) == choice.second &&
              powerfest.Read(0x208000, 0xa5) == 0x10 && powerfest.Read(0xe08000, 0xa5) == 0x10,
          "The 1994 full-bank window selects games while its fixed boot windows remain visible");
  }
  powerfest.Write(0x206000, 12);
  Check(powerfest.Read(0x008000, 0xa5) == 0x72 && powerfest.Read(0xc00000, 0xa5) == 0x32,
        "The second 1994 game uses HiROM offsets in the lower CPU banks");
  Check(powerfest.Read(0x018000, 0xa5) == 0x32,
        "Non-power-of-two game ROMs fold their upper address lines");
  Check(powerfest.Read(0x406000, 0xa5) == 0xa5 && powerfest.Read(0x600000, 0xa5) == 0xa5,
        "The 1994 board leaves unmapped lower banks open");
}

void Advance(CompetitionBoard &board, uint64_t clocks) {
  while (clocks) {
    const auto count = uint32_t(std::min<uint64_t>(clocks, 0xffffffff));
    board.Advance(count);
    clocks -= count;
  }
}

void CountdownAndSaves() {
  for (auto type :
       {CompetitionBoard::Board::CampusChallenge92, CompetitionBoard::Board::PowerFest94}) {
    for (bool pal : {false, true}) {
      const uint64_t frequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
      for (unsigned dip = 0; dip < 16; ++dip) {
        CompetitionBoard board({type, uint8_t(dip | 0xf0)}, Images());
        board.SetPal(pal);
        Advance(board, frequency / 3);
        board.Write(Control(type), 9);
        const uint64_t untilLastEdge =
            frequency - frequency / 3 + (uint64_t(3 + dip) * 60 - 1) * frequency;
        Advance(board, untilLastEdge);
        Check(board.Read(Status(type), 0xa5) == 0,
              "The timer does not expire before its final clock edge");
        board.Advance(1);
        Check(board.Read(Status(type), 0xa5) == 2,
              "Every DIP duration expires on the correct regional clock edge");
        board.Write(Control(type), 9);
        Check(board.Read(Status(type), 0xa5) == 2,
              "Restarting the timer retains the time-over flag");
        board.Reset();
        Check(board.Read(Status(type), 0xa5) == 0 && board.Read(0x008000, 0xa5) == 0x10,
              "Reset clears the timer status and restores the boot chip");
      }
    }
    CompetitionBoard board({type}, Images());
    const uint32_t save = type == CompetitionBoard::Board::CampusChallenge92 ? 0x700123 : 0x306123;
    Check(board.Read(save, 0xa5) == 0xff, "Fresh competition save RAM is erased");
    board.Write(save, 0x57);
    board.Reset();
    Check(board.Read(save ^ 0x800000, 0xa5) == 0x57, "Reset retains save RAM and its bank mirrors");
    const std::array<uint8_t, 2> prefix{0x19, 0x63};
    board.LoadRam(prefix);
    Check(board.SaveRam()[0] == 0x19 && board.SaveRam()[1] == 0x63 && board.SaveRam()[2] == 0xff,
          "Short saves replace their prefix without changing the remaining allocation");
  }
}

std::vector<uint8_t> CounterFirmware() {
  std::vector<uint8_t> bytes(8192);
  const std::array<uint32_t, 5> words{0xf1, 9u << 16, 0x1f, 0xf6,
                                      0x800000 | (0x100 << 13) | (4 << 2)};
  for (unsigned i = 0; i < words.size(); ++i)
    for (unsigned byte = 0; byte < 3; ++byte)
      bytes[i * 3 + byte] = uint8_t(words[i] >> (byte * 8));
  return bytes;
}

void FirmwarePorts() {
  CompetitionBoard board({CompetitionBoard::Board::PowerFest94}, Images());
  const auto firmware = CounterFirmware();
  Check(board.LoadFirmware(firmware) && board.HasSignalProcessor(),
        "Complete DSP-1 firmware selects processor execution");
  board.Advance(12);
  Check(board.Read(0x007000, 0xa5) == 0x80 && board.Read(0x807fff, 0xa5) == 0x80,
        "Firmware status reads retain the ready byte across aliases");
  Check(board.Read(0x006000, 0xa5) == 1 && board.Read(0x806fff, 0xa5) == 0,
        "Firmware output uses little-endian byte phases");
  Check(board.Read(0x007000, 0xa5) == 0, "Completing a firmware word acknowledges its request");
  board.Reset();
  board.Advance(12);
  Check(board.Read(0x006000, 0xa5) == 2 && board.Read(0x006000, 0xa5) == 0,
        "Board reset retains the processor's internal data RAM");
  Check(!board.LoadFirmware(std::span<const uint8_t>(firmware).first(8191)) &&
            board.HasSignalProcessor(),
        "Invalid replacement firmware leaves the active processor intact");
}

void ConsoleLoading() {
  auto machine = std::make_unique<Emulator>();
  for (auto type :
       {CompetitionBoard::Board::CampusChallenge92, CompetitionBoard::Board::PowerFest94}) {
    auto images = Images();
    const bool campus = type == CompetitionBoard::Board::CampusChallenge92;
    const uint8_t bank = campus ? 0x80 : 0x20;
    images[0][0] = 0x5c;
    images[0][1] = 0x10;
    images[0][2] = 0x80;
    images[0][3] = bank;
    std::vector<uint8_t> code{0x78};
    auto write = [&](uint32_t address, uint8_t value) {
      code.insert(code.end(), {0xa9, value, 0x8f, uint8_t(address), uint8_t(address >> 8),
                               uint8_t(address >> 16)});
    };
    auto read = [&](uint32_t address, uint8_t offset) {
      code.insert(code.end(), {0xaf, uint8_t(address), uint8_t(address >> 8),
                               uint8_t(address >> 16), 0x8f, offset, 0x10, 0x7e});
    };
    for (unsigned i = 0; i < 3; ++i) {
      const std::array<uint8_t, 3> selects =
          campus ? std::array<uint8_t, 3>{9, 5, 3} : std::array<uint8_t, 3>{9, 12, 10};
      write(Control(type), selects[i]);
      read(campus ? 0x008000 : 0xc00000, uint8_t(i));
    }
    const uint32_t save = campus ? 0x700123 : 0x306123;
    write(save, 0xa5);
    read(save ^ 0x800000, 3);
    read(Status(type), 4);
    code.push_back(0xdb);
    std::copy(code.begin(), code.end(), images[0].begin() + 0x10);
    std::array<std::span<const uint8_t>, 4> views;
    for (unsigned i = 0; i < images.size(); ++i)
      views[i] = images[i];
    Check(machine->LoadCompetitionCartridge({type}, views),
          "All four images load through the emulator");
    for (unsigned step = 0; step < 100 && !machine->GetCpu()->regs().stp; ++step)
      machine->GetCpu()->Step();
    Check(machine->GetCpu()->regs().stp, "The fixed boot mirror executes through game selection");
    const auto *ram = machine->GetBus().WramData();
    const std::array<uint8_t, 5> expected{0x21, 0x32, 0x43, 0xa5, 0};
    Check(
        std::equal(expected.begin(), expected.end(), ram + 0x1000),
        "The 65816 observes game selection, save mirrors and timer status through its mapped bus");
    auto invalid = views;
    invalid[2] = {};
    const auto *loaded = machine->LoadedCartridge();
    Check(!machine->LoadCompetitionCartridge({type}, invalid) &&
              machine->LoadedCartridge() == loaded,
          "A missing game image does not replace the running cartridge");
    Check(machine->Reset() && machine->GetBus().Read(save) == 0xa5,
          "Console reset retains the board save allocation");
  }
  Check(machine->LoadCartridge(Images()[0]),
        "An ordinary cartridge replaces the competition board");
  machine->GetBus().SetOpenBus(0x5a);
  Check(machine->GetBus().Read(0x106000) == 0x5a,
        "Reloading removes the competition register handlers");
}
} // namespace

int main() {
  try {
    Mappings();
    CountdownAndSaves();
    FirmwarePorts();
    ConsoleLoading();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts(
      "Competition board mapping, countdown, save, firmware and console loading checks passed");
}
