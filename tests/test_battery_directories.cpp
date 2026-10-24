#include "ConsoleConfig.hpp"
#include "cartridge/BroadcastFixture.hpp"
#include "gameboy/BatteryProgram.hpp"
#include "session/Session.hpp"
#include "settings/Preferences.hpp"
#include "snes/core/FilePath.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>

namespace fs = std::filesystem;
using snes::frontend::LaunchRequest;
using snes::frontend::Session;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Write(const fs::path &path, std::span<const uint8_t> data) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(data.data()), data.size());
  Check(bool(file), "The battery directory fixture writes");
}
std::vector<uint8_t> Read(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  Check(bool(file), "The selected battery file exists");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::vector<uint8_t> Image() {
  std::vector<uint8_t> image(32768);
  image[0x7fd5] = 0x20;
  image[0x7fd6] = 2;
  image[0x7fd7] = 5;
  image[0x7fd8] = 3;
  image[0x7fd9] = 1;
  image[0x7fdc] = image[0x7fdd] = 0xff;
  image[0x7ffd] = 0x80;
  const uint8_t program[] = {0x78, 0xa9, 0x80, 0x8d, 0, 0x21, 0xa9, 0x5a, 0x8f, 0, 0, 0x70, 0xdb};
  std::copy(std::begin(program), std::end(program), image.begin());
  return image;
}
Session NewSession() {
  return Session(snes::tests::DeterministicConsoleConfig(), {},
                 {snes::core::ControllerDevice::Gamepad, snes::core::ControllerDevice::Gamepad});
}

void RecoverDirectory(const fs::path &home) {
  fs::create_directories(home);
  Write(home / "game.sfc", Image());
  auto session = NewSession();
  LaunchRequest request;
  request.paths[0] = snes::core::PathText(home / "game.sfc");
  session.Open(request);
  auto *machine = session.Machine();
  machine->GetBus().Write(0x700000, 0x5a);
  session.Flush();
  fs::remove(home / "game.srm");
  fs::create_directory(home / "game.srm");
  machine->GetBus().Write(0x700000, 0x6b);
  session.SetSaveDirectory(home / "recovered");
  session.Flush();
  Check(session.Machine() == machine && machine->LoadedCartridge()->SramData()[0] == 0x6b &&
            fs::is_directory(home / "game.srm") && Read(home / "recovered/game.srm")[0] == 0x6b,
        "A new save folder recovers live battery data without requiring the broken old path to "
        "write");
  session.Close();
}

void LiveDirectory(const fs::path &home) {
  fs::create_directories(home / "games");
  const auto rom = home / "games/game.sfc", original = home / "games/game.srm";
  Write(rom, Image());
  Write(original, std::vector<uint8_t>(8192, 0x12));
  auto session = NewSession();
  LaunchRequest request;
  request.paths[0] = snes::core::PathText(rom);
  session.Open(request);
  auto *machine = session.Machine();
  Check(machine->LoadedCartridge()->SramData()[0] == 0x12,
        "Blank save folders retain adjacent saves");
  machine->StepFrame();
  machine->GetBus().Write(0x7e8123, 0xa6);
  const auto field = machine->CurrentFrame(), cycles = machine->CurrentMasterCycles();
  const auto selected = home / fs::path(u8"\u4fdd\u5b58") / "nested";
  fs::create_directories(selected);
  Write(selected / "game.srm", std::vector<uint8_t>(8192, 0xc3));
  snes::frontend::PreferencesFile preferences(home / "profile/preferences.cfg");
  auto candidate = preferences.Load();
  preferences.Save(candidate);
  candidate.saveDirectory = snes::core::PathText(selected);
  const auto backup = home / "profile/original.cfg";
  fs::rename(preferences.Path(), backup);
  fs::create_directory(preferences.Path());
  bool rejected = false;
  try {
    session.SetSaveDirectory(selected, [&] { preferences.Save(candidate); });
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected && Read(original)[0] == 0x12 && Read(selected / "game.srm")[0] == 0xc3,
        "A failed preference write leaves both old and new battery files untouched");
  fs::remove(preferences.Path());
  fs::rename(backup, preferences.Path());
  machine->GetBus().Write(0x700000, 0xa7);
  session.Flush();
  Check(Read(original)[0] == 0xa7 && Read(selected / "game.srm")[0] == 0xc3,
        "Failed Apply keeps the original active save destination");
  session.SetSaveDirectory(selected, [&] { preferences.Save(candidate); });
  Check(machine->LoadedCartridge()->SramData()[0] == 0xa7 && Read(selected / "game.srm")[0] == 0xc3,
        "Selecting a folder does not load or overwrite its existing battery file");
  session.Flush();
  Check(Read(selected / "game.srm")[0] == 0xa7,
        "The next save writes retained battery data even when emulation has stayed paused");
  machine->GetBus().Write(0x700000, 0xb8);
  session.Flush();
  Check(Read(original)[0] == 0xa7 && Read(selected / "game.srm")[0] == 0xb8,
        "Subsequent writes use the selected folder and preserve the old save");
  const auto invalid = home / "not-a-directory";
  Write(invalid, std::vector<uint8_t>{0x82});
  unsigned committed = 0;
  rejected = false;
  try {
    session.SetSaveDirectory(invalid, [&] { ++committed; });
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  const auto blocked = home / "blocked";
  fs::create_directories(blocked / "game.srm");
  bool blockedFile = false;
  try {
    session.SetSaveDirectory(blocked, [&] { ++committed; });
  } catch (const std::runtime_error &) {
    blockedFile = true;
  }
  Check(rejected && blockedFile && committed == 0,
        "Invalid folders and occupied file destinations are rejected before settings commit");
  const auto target = selected / "game.srm";
  machine->GetBus().Write(0x700000, 0xc9);
  session.Flush();
  Check(Read(target)[0] == 0xc9, "Rejected destinations leave the selected save file active");
  session.SetSaveDirectory({});
  session.Flush();
  machine->GetBus().Write(0x700000, 0xda);
  session.Flush();
  Check(Read(original)[0] == 0xda && Read(target)[0] == 0xc9,
        "Clearing the override resumes adjacent saves without deleting the selected copy");
  Check(session.Machine() == machine && machine->CurrentFrame() == field &&
            machine->CurrentMasterCycles() == cycles && machine->GetBus().Read(0x7e8123) == 0xa6,
        "Folder changes and failures retain the machine, timing, work RAM and loaded assets");
  session.Close();
  auto fresh = NewSession();
  fresh.SetSaveDirectory(preferences.ResolvePath(candidate.saveDirectory));
  fresh.Open(request);
  Check(fresh.Machine()->LoadedCartridge()->SramData()[0] == 0xc9,
        "A restarted profile loads the selected copy rather than the adjacent save");
  fresh.Close();
}

void HandheldDirectory(const fs::path &home) {
  using namespace snes::tests::gameboy;
  fs::create_directories(home);
  Write(home / "adapter.sfc", BatteryBios(false, false));
  Write(home / "guest.gb", BatteryProgram());
  Write(home / "boot.bin", BatteryBoot());
  Write(home / "adapter.srm", std::vector<uint8_t>(19, 0x42));
  const auto folder = home / "saves";
  fs::create_directory(folder);
  Write(folder / "guest.sav", std::vector<uint8_t>(17, 0x66));
  auto session = NewSession();
  session.SetSaveDirectory(folder);
  LaunchRequest request;
  request.kind = LaunchRequest::Kind::Handheld;
  request.paths = {snes::core::PathText(home / "adapter.sfc"),
                   snes::core::PathText(home / "guest.gb"), snes::core::PathText(home / "boot.bin"),
                   ""};
  session.Open(request);
  Check(session.Machine()->SaveSlotBattery(0)[16] == 0x66,
        "Handheld loading finds a short save in the selected folder");
  session.Machine()->StepFrame();
  session.Machine()->StepFrame();
  const auto bytes = session.Machine()->SaveSlotBattery(0);
  session.Flush();
  session.SetSaveDirectory(home / "moved");
  session.Close();
  Check(Read(folder / "guest.sav") == bytes && Read(home / "moved/guest.sav") == bytes &&
            Read(home / "adapter.srm") == std::vector<uint8_t>(19, 0x42) &&
            !fs::exists(home / "guest.sav") && !fs::exists(home / "moved/adapter.srm"),
        "Handheld directory changes retain the full battery file and never write a BIOS save");
  session.Open(request);
  Check(session.Machine()->SaveSlotBattery(0) == bytes, "Handheld restart uses the new folder");
  session.Close();
}

void SlotDirectories(const fs::path &home) {
  fs::create_directories(home / "first");
  fs::create_directories(home / "second");
  auto bios = Image();
  bios.resize(0x40000);
  constexpr std::string_view signature = "BANDAI SFC-ADX", backup = "SFC-ADX BACKUP";
  std::copy(signature.begin(), signature.end(), bios.begin());
  std::copy(backup.begin(), backup.end(), bios.begin() + 0x10);
  std::vector<uint8_t> game(0x80000);
  std::copy(signature.begin(), signature.end(), game.begin());
  game[0x37] = 1;
  Write(home / "bios.bin", bios);
  Write(home / "first/game.st", game);
  Write(home / "second/game.st", game);
  Write(home / "second/other.st", game);
  Write(home / "bios.srm", std::vector<uint8_t>(19, 0x82));
  LaunchRequest request;
  request.kind = LaunchRequest::Kind::SufamiTurbo;
  request.paths = {snes::core::PathText(home / "bios.bin"),
                   snes::core::PathText(home / "first/game.st"),
                   snes::core::PathText(home / "second/game.st"), ""};
  auto session = NewSession();
  session.Open(request);
  auto *machine = session.Machine();
  machine->GetBus().Write(0x608123, 0x57);
  machine->GetBus().Write(0x708123, 0x63);
  bool rejected = false;
  try {
    session.SetSaveDirectory(home / "shared");
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  session.Close();
  Check(rejected && Read(home / "first/game.srm")[0x123] == 0x57 &&
            Read(home / "second/game.srm")[0x123] == 0x63 && !fs::exists(home / "shared/game.srm"),
        "Same-named slots reject a shared folder and retain their separate existing files");
  session.SetSaveDirectory(home / "shared");
  rejected = false;
  try {
    session.Open(request);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected && !session.Machine(), "A startup folder also rejects colliding slot save names");
  request.paths[2] = snes::core::PathText(home / "second/other.st");
  session.Open(request);
  session.Machine()->GetBus().Write(0x608123, 0x74);
  session.Machine()->GetBus().Write(0x708123, 0x91);
  session.Flush();
  session.SetSaveDirectory(home / "moved");
  session.Close();
  Check(Read(home / "shared/game.srm")[0x123] == 0x74 &&
            Read(home / "shared/other.srm")[0x123] == 0x91 &&
            Read(home / "moved/game.srm")[0x123] == 0x74 &&
            Read(home / "moved/other.srm")[0x123] == 0x91 &&
            Read(home / "bios.srm") == std::vector<uint8_t>(19, 0x82),
        "Distinct slot names follow their owners across directory changes and preserve the BIOS");
  std::swap(request.paths[1], request.paths[2]);
  session.Open(request);
  Check(session.Machine()->GetBus().Read(0x608123) == 0x91 &&
            session.Machine()->GetBus().Read(0x708123) == 0x74,
        "Saves follow the game basename when slots swap in a custom folder");
  session.Close();
}

void ClockDirectory(const fs::path &home) {
  fs::create_directories(home);
  auto image = Image();
  image.resize(0x500000);
  std::copy_n(image.begin() + 0x7fc0, 64, image.begin() + 0x40ffc0);
  std::fill_n(image.begin() + 0x7fc0, 64, 0);
  image[0x40ffd5] = 0x35;
  image[0x40ffd6] = 0x55;
  image[0x40ffd7] = 13;
  image[0x408000] = 0x78;
  image[0x408001] = 0xdb;
  const auto rom = home / "clock.sfc";
  Write(rom, image);
  auto session = NewSession();
  LaunchRequest request;
  request.paths[0] = snes::core::PathText(rom);
  session.Open(request);
  auto clock = session.Machine()->SaveRtc();
  Check(clock.size() == snes::core::Srtc::SaveSize,
        "The generated cartridge exposes clock storage");
  clock[6] = 3;
  clock[7] = 2;
  clock[8] = 8;
  Check(session.Machine()->LoadRtc(clock), "The session accepts the clock calendar");
  session.Flush();
  session.SetSaveDirectory(home / "saves");
  session.Flush();
  Check(Read(home / "clock.rtc")[8] == 8 && Read(home / "saves/clock.rtc")[8] == 8,
        "Clock data uses the console basename in both outgoing and selected folders");
  session.Close();
  Write(home / "clock.rtc", std::vector<uint8_t>(3, 0x18));
  session.Open(request);
  Check(session.Machine()->SaveRtc()[8] == 8,
        "A restarted session reads the selected clock and ignores an invalid adjacent file");
  session.Close();
}

void FlashDirectory(const fs::path &home) {
  fs::create_directories(home);
  Write(home / "broadcast.sfc", broadcast_test::ErasePollingRom(false));
  std::vector<uint8_t> pack(0x100000, 0xff);
  Write(home / "pack.bs", pack);
  auto session = NewSession();
  LaunchRequest request;
  request.kind = LaunchRequest::Kind::Broadcast;
  request.paths = {snes::core::PathText(home / "broadcast.sfc"),
                   snes::core::PathText(home / "pack.bs"), "", ""};
  session.Open(request);
  pack[0x1234] = 0x47;
  Check(session.Machine()->LoadMemoryPack(pack), "The session accepts writable pack bytes");
  auto metadata = session.Machine()->LoadedCartridge()->MemoryPackMetadata();
  metadata[10] ^= 0x83;
  Check(session.Machine()->LoadMemoryPackMetadata(metadata), "The session accepts pack identity");
  session.Flush();
  session.SetSaveDirectory(home / "saves");
  session.Close();
  Check(Read(home / "pack.bs.flash") == pack && Read(home / "saves/pack.bs.flash") == pack &&
            Read(home / "pack.bs.flash.meta") == metadata &&
            Read(home / "saves/pack.bs.flash.meta") == metadata &&
            !fs::exists(home / "saves/broadcast.sfc.flash"),
        "A separate pack retains flash and identity ownership after changing folders");
  session.Open(request);
  const auto restored = session.Machine()->LoadedCartridge()->MemoryPackData();
  Check(std::equal(pack.begin(), pack.end(), restored.begin(), restored.end()) &&
            session.Machine()->LoadedCartridge()->MemoryPackMetadata() == metadata,
        "The selected folder restores the pack image and its identity together");
  session.Close();
}
} // namespace

int main() {
  const auto directory =
      fs::temp_directory_path() /
      ("cupid-battery-directories-" +
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
    RecoverDirectory(directory / "recovery");
    LiveDirectory(directory / "console");
    HandheldDirectory(directory / "handheld");
    SlotDirectories(directory / "slots");
    ClockDirectory(directory / "clock");
    FlashDirectory(directory / "flash");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
