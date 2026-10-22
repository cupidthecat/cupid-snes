#include "ConsoleConfig.hpp"
#include "PpuScanlineProgram.hpp"
#include "cartridge/BroadcastFixture.hpp"
#include "gameboy/BatteryProgram.hpp"
#include "session/Session.hpp"
#include "snes/core/FilePath.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
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
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void Write(const fs::path &path, std::span<const uint8_t> bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  Check(bool(file), "Fixture file writes");
}
std::vector<uint8_t> Read(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  Check(bool(file), "Saved file exists");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::vector<uint8_t> ConsoleImage() {
  std::vector<uint8_t> image(32768);
  std::fill_n(image.begin() + 0x7fc0, 21, ' ');
  image[0x7fd5] = 0x20;
  image[0x7fd6] = 2;
  image[0x7fd7] = 5;
  image[0x7fd8] = 3;
  image[0x7fd9] = 1;
  image[0x7fdc] = image[0x7fdd] = 0xff;
  image[0x7ffd] = 0x80;
  const uint8_t code[] = {0x78, 0xa9, 0x80, 0x8d, 0, 0x21, 0xa9, 0x5a, 0x8f, 0, 0, 0x70, 0xdb};
  std::copy(std::begin(code), std::end(code), image.begin());
  return image;
}
Session NewSession() {
  return Session(snes::tests::DeterministicConsoleConfig(), {},
                 {snes::core::ControllerDevice::Gamepad, snes::core::ControllerDevice::Gamepad});
}
void Replacement(const fs::path &directory) {
  const auto rom = directory / "console.sfc", save = directory / "console.srm";
  const auto bad = directory / "bad.sfc", badSave = directory / "bad.srm";
  Write(rom, ConsoleImage());
  Write(bad, ConsoleImage());
  Write(badSave, std::vector<uint8_t>(7, 0x31));
  auto session = NewSession();
  Check(!session.Machine() && !session.Loaded(), "New session is idle");
  LaunchRequest request;
  request.paths[0] = snes::core::PathText(rom);
  session.Open(request);
  auto *machine = session.Machine();
  machine->StepFrame();
  Check(machine->LoadedCartridge()->SramData()[0] == 0x5a, "CPU writes its battery byte");
  const auto frame = machine->CurrentFrame();
  auto replacement = request;
  replacement.paths[0] = snes::core::PathText(bad);
  bool rejected = false;
  try {
    session.Open(replacement);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected && session.Machine() == machine && machine->CurrentFrame() == frame,
        "A bad replacement save leaves the running machine intact");
  Check(Read(save).size() == 8192 && Read(save)[0] == 0x5a,
        "Replacement flushes the old CPU-written save first");
  Check(Read(badSave) == std::vector<uint8_t>(7, 0x31), "Rejected save remains untouched");
  machine->GetBus().Write(0x7e8123, 0xa6);
  machine->GetSmp().Ram()[0x42] = 0xd3;
  session.Reset();
  Check(session.Machine() == machine && machine->LoadedCartridge()->SramData()[0] == 0x5a &&
            machine->GetBus().Read(0x7e8123) == 0xa6 && machine->GetSmp().Ram()[0x42] == 0xd3,
        "Warm reset retains the machine, work RAM, audio RAM and battery");
  Write(rom, std::vector<uint8_t>{1, 2, 3});
  session.Reset(true);
  Check(
      session.Machine() == machine && machine->LoadedCartridge()->SramData()[0] == 0x5a &&
          machine->GetBus().Read(0x7e8123) != 0xa6 && machine->GetSmp().Ram()[0x42] == 0xd3 &&
          machine->CurrentFrame() == 0 && machine->CurrentMasterCycles() == 0,
      "Power cycle initializes work RAM while retaining cartridge storage and physical audio RAM");
  Write(rom, ConsoleImage());
  machine = session.Machine();
  machine->GetBus().Write(0x700000, 0xa7);
  fs::remove(save);
  fs::create_directory(save);
  rejected = false;
  try {
    session.Close();
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected && session.Machine() == machine,
        "A failed atomic save keeps the current session available for retry");
  fs::remove(save);
  session.Close();
  Check(!session.Machine() && Read(save)[0] == 0xa7, "Retry saves and unloads successfully");
}
void DeviceChanges(const fs::path &directory) {
  using D = snes::core::ControllerDevice;
  using E = snes::core::Emulator::ExpansionDevice;
  struct Input : snes::core::IInputProvider {
    std::array<snes::core::InputState, 8> players{};
    snes::core::InputState Poll(uint64_t) override {
      return players[0];
    }
    snes::core::InputState PollController(int player, uint64_t) override {
      return players.at(player);
    }
    snes::core::MouseState PollMouse(int, uint64_t) override {
      return {1, 0, true, false};
    }
  } input;
  input.players[0].b = input.players[0].y = input.players[4].b = true;
  const auto rom = directory / "device-settings.sfc";
  Write(rom, ConsoleImage());
  Session session(snes::tests::DeterministicConsoleConfig(), {nullptr, nullptr, &input},
                  {D::Gamepad, D::Gamepad});
  LaunchRequest request;
  request.paths[0] = snes::core::PathText(rom);
  session.Open(request);
  auto *machine = session.Machine();
  machine->StepFrame();
  machine->GetBus().Write(0x7e0123, 0xa6);
  const auto frame = machine->CurrentFrame(), cycles = machine->CurrentMasterCycles();
  auto &ports = machine->GetAutoJoypad().Ports();
  ports.SetLatch(true);
  ports.SetLatch(false);
  Check((ports.Read(0) & 1) == 1 && (ports.Read(1) & 1) == 0,
        "The initial pads use players one and two");
  session.SetDevices(session.Devices());
  Check((ports.Read(0) & 1) == 1,
        "Applying unchanged devices preserves a partially shifted controller packet");
  snes::frontend::ConsoleDevices devices{{D::Multitap, D::Gamepad}, E::Satellaview};
  session.SetDevices(devices);
  ports.SetLatch(true);
  ports.SetLatch(false);
  Check((ports.Read(1) & 1) == 1, "Port two moves to player five after attaching a multitap");
  Check(session.Machine() == machine && machine->CurrentFrame() == frame &&
            machine->CurrentMasterCycles() == cycles && machine->GetBus().Read(0x7e0123) == 0xa6 &&
            machine->LoadedCartridge()->SramData()[0] == 0x5a &&
            machine->GetExpansionDevice() == E::Satellaview,
        "Live device changes preserve the machine, timing, work RAM and cartridge storage");
  auto invalid = devices;
  invalid.ports[0] = D::SuperScope;
  bool rejected = false;
  try {
    session.SetDevices(invalid);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Check(rejected && session.Devices() == devices && session.Machine() == machine,
        "Rejected hardware choices leave the current session intact");
  session.Close();
  session.Open(request);
  Check(session.Devices() == devices && session.Machine()->GetExpansionDevice() == E::Satellaview,
        "Device choices remain active when replacing or reopening games");
  devices.ports = {D::Gamepad, D::Mouse};
  session.SetDevices(devices);
  auto &mousePorts = session.Machine()->GetAutoJoypad().Ports();
  mousePorts.SetLatch(true);
  mousePorts.SetLatch(false);
  for (unsigned bit = 0; bit < 9; ++bit)
    mousePorts.Read(1);
  devices.ports[0] = D::Multitap;
  session.SetDevices(devices);
  Check((mousePorts.Read(1) & 1) == 1,
        "Changing another port preserves the mouse's partially shifted button packet");
}
void Handheld(const fs::path &directory) {
  using namespace snes::tests::gameboy;
  for (bool second : {false, true}) {
    for (bool pal : {false, true}) {
      const auto home = directory / (std::to_string(second) + std::to_string(pal));
      fs::create_directories(home / "bios");
      fs::create_directories(home / "games");
      fs::create_directories(home / "boot");
      const auto bios = home / "bios/adapter.sfc", game = home / "games/guest.gb";
      const auto boot = home / "boot/boot.bin", save = home / "games/guest.sav";
      Write(bios, BatteryBios(second, pal));
      Write(game, BatteryProgram());
      Write(boot, BatteryBoot());
      Write(save, std::vector<uint8_t>(17, 0x66));
      const auto biosSave = home / "bios/adapter.srm";
      Write(biosSave, std::vector<uint8_t>(19, 0x42));
      auto session = NewSession();
      LaunchRequest request;
      request.kind = LaunchRequest::Kind::Handheld;
      request.paths = {snes::core::PathText(bios), snes::core::PathText(game),
                       snes::core::PathText(boot), ""};
      session.Open(request);
      const auto initial = session.Machine()->SaveSlotBattery(0);
      Check(initial.size() == 32768 && initial[16] == 0x66 && initial[17] == 0xff,
            "Handheld short save supplies its prefix without touching the BIOS save");
      session.Machine()->StepFrame();
      session.Machine()->StepFrame();
      session.Close();
      const auto saved = Read(save);
      Check(saved.size() == 32768, "Handheld session flushes all banks");
      for (unsigned bank = 0; bank < 4; ++bank)
        Check(saved[bank * 8192 + 0x123] == uint8_t(0x53 + bank * 37),
              "Handheld CPU writes reach the game's save path");
      Check(Read(biosSave) == std::vector<uint8_t>(19, 0x42), "Separate BIOS save stays unchanged");
      session.Open(request);
      Check(session.Machine()->SaveSlotBattery(0) == saved, "Fresh session restores the full save");
      session.Close();
    }
  }
}

void UnicodePaths(const fs::path &directory) {
  const auto home = directory / fs::path(u8"\u65e5\u672c\u8a9e-\u00e9-\U0001f3ae");
  fs::create_directories(home / "firmware");
  const auto rom = home / fs::path(u8"\u30b2\u30fc\u30e0.sfc");
  Write(rom, ConsoleImage());
  auto request = LaunchRequest{};
  request.paths[0] = snes::core::PathText(rom);
  const auto firmware = home / "firmware" / "dsp1.rom";
  Write(firmware, std::vector<uint8_t>(3));
  auto session = NewSession();
  bool rejected = false;
  try {
    session.Open(request);
  } catch (const std::runtime_error &error) {
    rejected = std::string(error.what()).find(snes::core::PathText(firmware)) != std::string::npos;
  }
  Check(rejected && !session.Machine(), "Unicode firmware errors retain their complete path");
  Write(firmware,
        std::vector<uint8_t>(snes::core::CartridgeFirmware::Size(snes::core::FirmwareKind::Dsp1)));
  auto msu = rom;
  msu.replace_extension(".msu");
  Write(msu, std::vector<uint8_t>{0x17, 0x3a});
  session.Open(request);
  Check(session.Machine()->GetBus().Read(0x002002) == 'S',
        "Unicode game paths attach adjacent streaming media");
  session.Machine()->StepFrame();
  session.Close();
  auto save = rom;
  save.replace_extension(".srm");
  Check(Read(save)[0] == 0x5a, "CPU-written saves use the exact Unicode game path");
  session.Open(request);
  Check(session.Machine()->LoadedCartridge()->SramData()[0] == 0x5a,
        "Unicode battery saves survive a new session");
  session.Close();
  Handheld(home);
}

void SlotSaves(const fs::path &directory) {
  const auto home = directory / "slots";
  fs::create_directories(home / "first");
  fs::create_directory(home / "second");
  const auto biosPath = home / "bios.bin", aPath = home / "first/game.st",
             bPath = home / "second/game.st";
  auto bios = ConsoleImage();
  bios.resize(0x40000);
  constexpr std::string_view signature = "BANDAI SFC-ADX", backup = "SFC-ADX BACKUP";
  std::copy(signature.begin(), signature.end(), bios.begin());
  std::copy(backup.begin(), backup.end(), bios.begin() + 0x10);
  std::vector<uint8_t> game(0x80000, 0);
  std::copy(signature.begin(), signature.end(), game.begin());
  game[0x37] = 1;
  Write(biosPath, bios);
  Write(aPath, game);
  Write(bPath, game);
  const auto biosSave = home / "bios.srm";
  Write(biosSave, std::vector<uint8_t>(17, 0x82));
  auto session = NewSession();
  LaunchRequest request;
  request.kind = LaunchRequest::Kind::SufamiTurbo;
  request.paths = {snes::core::PathText(biosPath), snes::core::PathText(aPath),
                   snes::core::PathText(bPath), ""};
  session.Open(request);
  session.Machine()->GetBus().Write(0x608123, 0x57);
  session.Machine()->GetBus().Write(0x708123, 0x63);
  auto duplicate = request;
  duplicate.paths[2] = duplicate.paths[1];
  bool rejected = false;
  auto *machine = session.Machine();
  try {
    session.Open(duplicate);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected && session.Machine() == machine,
        "Two slots sharing a save path cannot replace the current session");
#ifdef _WIN32
  std::transform(duplicate.paths[2].begin(), duplicate.paths[2].end(), duplicate.paths[2].begin(),
                 [](unsigned char value) {
                   return value >= 'a' && value <= 'z' ? char(value - 'a' + 'A') : char(value);
                 });
  rejected = false;
  try {
    session.Open(duplicate);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected && session.Machine() == machine,
        "Windows path case cannot make two slots overwrite the same save");
  const auto freshPath = home / "fresh.st";
  Write(freshPath, game);
  duplicate.paths[1] = snes::core::PathText(freshPath);
  duplicate.paths[2] = duplicate.paths[1];
  std::transform(duplicate.paths[2].begin(), duplicate.paths[2].end(), duplicate.paths[2].begin(),
                 [](unsigned char value) {
                   return value >= 'a' && value <= 'z' ? char(value - 'a' + 'A') : char(value);
                 });
  auto fresh = NewSession();
  rejected = false;
  try {
    fresh.Open(duplicate);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected && !fresh.Machine() && !fs::exists(home / "fresh.srm"),
        "Case aliases are rejected even before a shared save exists");
#endif
  session.Close();
  Check(Read(home / "first/game.srm")[0x123] == 0x57 &&
            Read(home / "second/game.srm")[0x123] == 0x63,
        "Same-named slot games in separate directories keep distinct saves");
  Check(Read(biosSave) == std::vector<uint8_t>(17, 0x82),
        "Slot saves never overwrite the BIOS save");
  std::swap(request.paths[1], request.paths[2]);
  session.Open(request);
  Check(session.Machine()->GetBus().Read(0x608123) == 0x63 &&
            session.Machine()->GetBus().Read(0x708123) == 0x57,
        "Battery saves follow their games when slots are exchanged");
  session.Close();
}

void FlashSaves(const fs::path &directory) {
  const auto bios = directory / "broadcast.sfc", packPath = directory / "pack.bs";
  Write(bios, broadcast_test::ErasePollingRom(false));
  std::vector<uint8_t> pack(0x100000, 0xff);
  Write(packPath, pack);
  const auto untouched = directory / "broadcast.sfc.flash";
  Write(untouched, std::vector<uint8_t>(7, 0x29));
  auto session = NewSession();
  LaunchRequest request;
  request.kind = LaunchRequest::Kind::Broadcast;
  request.paths = {snes::core::PathText(bios), snes::core::PathText(packPath), "", ""};
  session.Open(request);
  pack[0x1234] = 0x47;
  Check(session.Machine()->LoadMemoryPack(pack), "The session accepts pack storage");
  auto metadata = session.Machine()->LoadedCartridge()->MemoryPackMetadata();
  Check(!metadata.empty(), "Writable packs expose persistent metadata");
  metadata[10] ^= 0x83;
  Check(session.Machine()->LoadMemoryPackMetadata(metadata),
        "The session accepts valid pack metadata");
  session.Close();
  Check(Read(directory / "pack.bs.flash") == pack &&
            Read(directory / "pack.bs.flash.meta") == metadata,
        "Separate broadcast packs own their flash image and metadata sidecars");
  Check(Read(untouched) == std::vector<uint8_t>(7, 0x29), "A BIOS flash sentinel stays untouched");
  session.Open(request);
  const auto restored = session.Machine()->LoadedCartridge()->MemoryPackData();
  Check(std::equal(pack.begin(), pack.end(), restored.begin(), restored.end()) &&
            session.Machine()->LoadedCartridge()->MemoryPackMetadata() == metadata,
        "A fresh session restores both pack bytes and identity");
  session.Close();
}

void OutputParity(const fs::path &directory) {
  struct Output : snes::core::IVideoOutput, snes::core::IAudioOutput {
    snes::core::VideoFrame picture;
    snes::core::AudioBuffer sound;
    unsigned pictures = 0, sounds = 0;
    void Present(const snes::core::VideoFrame &frame) override {
      picture = frame;
      ++pictures;
    }
    void Submit(const snes::core::AudioBuffer &buffer) override {
      sound = buffer;
      ++sounds;
    }
  } directOutput, sessionOutput;
  for (bool pal : {false, true}) {
    auto image = PpuGraphicsProgram(1, false, true, false);
    image[0x7fd9] = pal ? 2 : 1;
    const auto rom = directory / (pal ? "picture-pal.sfc" : "picture-ntsc.sfc");
    Write(rom, image);
    snes::core::Emulator::Config config;
    config.entropyMode = snes::core::EntropyMode::None;
    config.entropySeed = 0;
    auto direct = std::make_unique<snes::core::Emulator>(config);
    direct->AttachVideoOutput(&directOutput);
    direct->AttachAudioOutput(&directOutput);
    Check(direct->LoadCartridgeFromFile(snes::core::PathText(rom)), "The direct machine loads");
    Session session(config, {&sessionOutput, &sessionOutput, nullptr},
                    {snes::core::ControllerDevice::Gamepad, snes::core::ControllerDevice::Gamepad});
    LaunchRequest request;
    request.paths[0] = snes::core::PathText(rom);
    session.Open(request);
    for (unsigned frame = 0; frame < 6; ++frame) {
      const auto expected = direct->StepFrame(), actual = session.Machine()->StepFrame();
      Check(expected.masterCycles == actual.masterCycles &&
                expected.frameIndex == actual.frameIndex &&
                directOutput.picture.width == sessionOutput.picture.width &&
                directOutput.picture.height == sessionOutput.picture.height &&
                directOutput.picture.pixels == sessionOutput.picture.pixels &&
                directOutput.sound.sampleRate == sessionOutput.sound.sampleRate &&
                directOutput.sound.interleavedStereo == sessionOutput.sound.interleavedStereo &&
                directOutput.pictures == sessionOutput.pictures &&
                directOutput.sounds == sessionOutput.sounds,
            "Desktop sessions preserve default picture, audio and field timing for both regions");
    }
    session.Close();
  }
}
} // namespace
int main() {
  const auto directory =
      fs::temp_directory_path() / ("snes-session-" + std::to_string(SDL_GetTicksNS()));
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
    Replacement(directory);
    DeviceChanges(directory);
    Handheld(directory);
    UnicodePaths(directory);
    SlotSaves(directory);
    FlashSaves(directory);
    OutputParity(directory);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
