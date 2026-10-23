#include "ConsoleConfig.hpp"
#include "cartridge/StateFixture.hpp"
#include "cheats/CheatBook.hpp"
#include "session/Session.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/AtomicFile.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace snes::frontend;
using snes::core::CheatSystem;
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
  Check(rejected, "Invalid cheat storage operation rejects");
}
void Text(const fs::path &path, const std::string &text) {
  std::ofstream file(path, std::ios::binary);
  file.write(text.data(), std::streamsize(text.size()));
  Check(bool(file), "Write fixture");
}
void Format(const fs::path &directory) {
  const auto path = directory / "codes.ccht";
  const CheatBook book{
      true,
      {{"Health \"A\" \\ B", "7e1234ab+7E1235=FF?00", true}, {"Disabled", "DF47-0915", false}}};
  CheatStorage::Write(path, book, CheatSystem::Console);
  const auto loaded = CheatStorage::Read(path, CheatSystem::Console);
  Check(loaded.enabled && loaded.entries.size() == 2 &&
            loaded.entries[0].name == book.entries[0].name &&
            loaded.entries[0].codes == "7E1234=AB+7E1235=FF?00" && !loaded.entries[1].enabled &&
            loaded.Compile(CheatSystem::Console) == book.Compile(CheatSystem::Console),
        "Versioned format retains names, flags, group order and canonical codes");
  const auto bytes = ReadFile(path, 1024 * 1024);
  auto invalid = book;
  invalid.entries[1].codes = "bad";
  Reject([&] { CheatStorage::Write(path, invalid, CheatSystem::Console); });
  Check(ReadFile(path, 1024 * 1024) == bytes, "Invalid disabled codes preserve the previous file");
  Reject([&] { CheatStorage::Read(path, CheatSystem::Handheld); });
  for (const auto &text :
       {std::string{}, std::string("CupidCheats 2 console\nenabled 1\n"),
        std::string("CupidCheats 1 console\nenabled 2\n"),
        std::string("CupidCheats 1 console\nenabled 1 extra\n"),
        std::string("CupidCheats 1 console\nenabled 1\ncheat 1 \"A\" \"7E0000=GG\"\n"),
        std::string("CupidCheats 1 console\nenabled 1\nunknown\n"),
        std::string(1024 * 1024 + 1, 'x')}) {
    Text(path, text);
    Reject([&] { CheatStorage::Read(path, CheatSystem::Console); });
  }
  Text(path, std::string("CupidCheats 1 console\nenabled 1\n") + std::string(1, '\0'));
  Reject([&] { CheatStorage::Read(path, CheatSystem::Console); });
  const auto handheld = directory / "handheld.ccht";
  CheatStorage::Write(handheld, {true, {{"Lives", "01AB34C1", true}}}, CheatSystem::Handheld);
  Check(CheatStorage::Read(handheld, CheatSystem::Handheld).entries[0].codes == "C134=AB",
        "Handheld files retain their address space");
  Check(CheatStorage::Read(directory / "missing.ccht", CheatSystem::Console).entries.empty(),
        "A missing file starts an empty book");
}
void SessionFlow(const fs::path &directory) {
  const auto image = snes::tests::cartridge_state::Image(0x20, 2, 32768);
  const auto rom = directory / "game.sfc";
  Text(rom, std::string(image.begin(), image.end()));
  LaunchRequest request;
  request.paths[0] = snes::core::PathText(rom);
  Session session(snes::tests::DeterministicConsoleConfig(), {},
                  {snes::core::ControllerDevice::Gamepad, snes::core::ControllerDevice::Gamepad});
  session.Open(request);
  const auto path = session.CheatPath();
  auto &bus = session.Machine()->GetBus();
  bus.WramData()[0x1234] = 17;
  CheatBook book{true, {{"Health", "7E1234=22", true}, {"Inactive", "7E1235=33", false}}};
  session.SaveCheats(book);
  Check(bus.WramData()[0x1234] == 0x22 && *session.Cheats() == book && fs::exists(path),
        "Saving applies only enabled groups and persists the book");
  book.enabled = false;
  session.SaveCheats(book);
  Check(bus.WramData()[0x1234] == 17, "Global disable restores the original byte");
  session.Close();
  session.Open(request);
  Check(!session.Cheats()->enabled && session.Cheats()->entries.size() == 2,
        "Global and individual flags persist across game replacement");
  book.enabled = true;
  session.SaveCheats(book);
  const auto frame = session.Machine()->CurrentFrame();
  const auto live = session.Machine()->GetBus().WramData()[0x1234];
  const auto validFile = ReadFile(path, 1024 * 1024);
  Text(path, "broken file");
  Reject([&] { session.ReloadCheats(); });
  Check(session.Machine()->CurrentFrame() == frame && *session.Cheats() == book &&
            session.Machine()->GetBus().WramData()[0x1234] == live,
        "A failed reload preserves the current game and active cheats");
  session.Close();
  session.Open(request);
  Check(session.Machine() && !session.CheatError().empty() && session.Cheats()->entries.empty(),
        "Unreadable cheats leave the game available without applying partial input");
  Reject([&] { session.SaveCheats(book); });
  Check(ReadFile(path, 1024 * 1024) ==
            std::vector<uint8_t>({'b', 'r', 'o', 'k', 'e', 'n', ' ', 'f', 'i', 'l', 'e'}),
        "An unreadable file requires explicit replacement");
  WriteFileAtomically(path, validFile);
  session.ReloadCheats();
  Check(session.CheatError().empty() && *session.Cheats() == book,
        "Repair and retry recover the original book");
  fs::remove(path);
  fs::create_directory(path);
  auto changed = book;
  changed.entries[0].codes = "7E1234=44";
  Reject([&] { session.SaveCheats(changed); });
  Check(*session.Cheats() == book && session.Machine()->GetBus().WramData()[0x1234] == live,
        "Failed replacement keeps active values and editable settings");
  for (const auto &entry : fs::directory_iterator(path.parent_path()))
    Check(entry.path().filename().string().find(".tmp-") == std::string::npos,
          "Failed writes remove their temporary files");
  fs::remove(path);
  session.SaveCheats(changed);
  Check(session.Machine()->GetBus().WramData()[0x1234] == 0x44,
        "Retry saves and applies repaired changes");
  Text(path, "broken again");
  session.Close();
  session.Open(request);
  session.SaveCheats(book, true);
  Check(session.CheatError().empty() &&
            CheatStorage::Read(path, CheatSystem::Console).entries.size() == 2,
        "Explicit replacement repairs an unreadable file");
  session.Close();
}
} // namespace
int main() {
  const auto directory =
      fs::temp_directory_path() /
      ("cupid-cheats-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  if (!fs::create_directory(directory))
    return 2;
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{directory};
  try {
    Format(directory);
    SessionFlow(directory);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
