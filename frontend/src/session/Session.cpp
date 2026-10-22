#include "Session.hpp"
#include "../SaveRamFile.hpp"
#include "snes/core/FilePath.hpp"

#include <filesystem>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace snes::frontend {
namespace {
std::filesystem::path Sidecar(const std::string &source, const char *extension,
                              bool canonical = false) {
  auto path = core::FilePath(source);
  if (canonical)
    path = std::filesystem::weakly_canonical(path);
  path.replace_extension(extension);
  return path;
}
bool SameSavePath(const std::filesystem::path &first, const std::filesystem::path &second) {
  if (first.empty() || second.empty())
    return false;
  if (first == second)
    return true;
  std::error_code error;
  if (std::filesystem::equivalent(first, second, error))
    return true;
#ifdef _WIN32
  const auto &left = first.native(), &right = second.native();
  return CompareStringOrdinal(left.c_str(), int(left.size()), right.c_str(), int(right.size()),
                              TRUE) == CSTR_EQUAL;
#else
  return false;
#endif
}
auto PlayersFor(const std::array<core::ControllerDevice, 2> &devices) {
  std::array<std::array<int, 4>, 2> result{};
  int player = 0;
  for (size_t port = 0; port < devices.size(); ++port) {
    result[port].fill(-1);
    const int count = devices[port] == core::ControllerDevice::Multitap ? 4 : 1;
    for (int index = 0; index < count; ++index)
      result[port][index] = player++;
  }
  return result;
}
void ConfigurePorts(core::Emulator &machine, const std::array<core::ControllerDevice, 2> &devices,
                    const std::array<core::ControllerDevice, 2> *previous = nullptr) {
  const auto players = PlayersFor(devices);
  const auto oldPlayers = PlayersFor(previous ? *previous : devices);
  for (unsigned port = 0; port < devices.size(); ++port) {
    const bool usesPlayers = devices[port] == core::ControllerDevice::Gamepad ||
                             devices[port] == core::ControllerDevice::Multitap;
    if (!previous || devices[port] != (*previous)[port] ||
        (usesPlayers && players[port] != oldPlayers[port]))
      machine.GetAutoJoypad().Ports().Configure(port, devices[port], players[port]);
  }
}
} // namespace

struct Session::State {
  LaunchRequest request;
  std::unique_ptr<core::Emulator> machine;
  std::unique_ptr<SaveRamFile> ram, clock, pack, packMetadata;
  std::array<std::unique_ptr<SaveRamFile>, 2> slots;
};

Session::Session(core::Emulator::Config config, Outputs outputs,
                 std::array<core::ControllerDevice, 2> devices)
    : config_(std::move(config)), outputs_(outputs), devices_(devices) {
  Devices().Validate();
}
Session::~Session() = default;

core::Emulator *Session::Machine() const noexcept {
  return state_ ? state_->machine.get() : nullptr;
}
const LaunchRequest *Session::Loaded() const noexcept {
  return state_ ? &state_->request : nullptr;
}
ConsoleDevices Session::Devices() const noexcept {
  return {devices_, config_.expansionDevice};
}
void Session::SetDevices(const ConsoleDevices &devices) {
  devices.Validate();
  if (auto *machine = Machine()) {
    ConfigurePorts(*machine, devices.ports, &devices_);
    if (devices.expansion != config_.expansionDevice)
      machine->SetExpansionDevice(devices.expansion);
  }
  devices_ = devices.ports;
  config_.expansionDevice = devices.expansion;
}

void Session::Open(const LaunchRequest &request) {
  Flush();
  auto next = std::make_unique<State>();
  next->request = request;
  next->machine = std::make_unique<core::Emulator>(config_);
  auto &machine = *next->machine;
  ConfigurePorts(machine, devices_);
  using Kind = LaunchRequest::Kind;
  const auto &paths = request.paths;
  std::string error;
  bool loaded = false;
  switch (request.kind) {
  case Kind::Cartridge:
    loaded = machine.LoadCartridgeFromFile(paths[0], &error);
    break;
  case Kind::Handheld:
    loaded = machine.LoadSuperGameBoyFromFiles(paths[0], paths[1], paths[2], &error);
    break;
  case Kind::SufamiTurbo:
    loaded = machine.LoadSufamiTurboFromFiles(paths[0], paths[1], paths[2], &error);
    break;
  case Kind::Broadcast:
    loaded = machine.LoadBroadcastCartridgeFromFiles(paths[0], paths[1], &error);
    break;
  case Kind::Campus92:
  case Kind::PowerFest94: {
    const core::CompetitionBoard::Settings settings{
        request.kind == Kind::Campus92 ? core::CompetitionBoard::Board::CampusChallenge92
                                       : core::CompetitionBoard::Board::PowerFest94,
        request.competitionDip, 0x2000};
    loaded = machine.LoadCompetitionCartridgeFromFiles(settings, paths, &error);
    break;
  }
  }
  if (!loaded)
    throw std::runtime_error(error.empty() ? "Cannot load the selected cartridge" : error);
  const auto *cart = machine.LoadedCartridge();
  if (request.kind == Kind::Handheld) {
    next->ram = std::make_unique<SaveRamFile>(Sidecar(paths[1], ".sav", true));
    const auto bytes = next->ram->LoadUpTo(machine.SaveSlotBattery(0).size());
    if (!bytes.empty() && !machine.LoadSlotBattery(0, bytes))
      throw std::runtime_error("Invalid handheld battery save");
  } else if (request.kind == Kind::SufamiTurbo) {
    std::array<std::filesystem::path, 2> saves;
    for (unsigned slot = 0; slot < 2; ++slot)
      if (!cart->SlotSramData(slot).empty())
        saves[slot] = Sidecar(paths[slot + 1], ".srm", true);
    if (SameSavePath(saves[0], saves[1]))
      throw std::runtime_error(
          "Both slots would write the same save file; use separate game paths");
    for (unsigned slot = 0; slot < 2; ++slot) {
      if (saves[slot].empty())
        continue;
      next->slots[slot] = std::make_unique<SaveRamFile>(saves[slot]);
      machine.LoadSlotSram(slot, next->slots[slot]->Load(cart->SlotSramData(slot).size()));
    }
  } else {
    next->ram = std::make_unique<SaveRamFile>(Sidecar(paths[0], ".srm"));
    machine.LoadSram(next->ram->Load(cart->SramLoadSize()));
    const auto rtcPath = Sidecar(paths[0], ".rtc");
    next->clock = std::make_unique<SaveRamFile>(rtcPath);
    const auto clock = next->clock->Load(machine.SaveRtc().size());
    if (!clock.empty() && !machine.LoadRtc(clock))
      throw std::runtime_error("Invalid cartridge clock save: " + core::PathText(rtcPath));
    auto packPath = core::FilePath(
        request.kind == Kind::Broadcast && paths[1] != "-" && !paths[1].empty() ? paths[1]
                                                                                : paths[0]);
    packPath += ".flash";
    next->pack = std::make_unique<SaveRamFile>(packPath);
    const auto pack = next->pack->Load(cart->MemoryPackData().size());
    if (!pack.empty() && !machine.LoadMemoryPack(pack))
      throw std::runtime_error("Invalid memory pack save: " + core::PathText(packPath));
    auto metadataPath = packPath;
    metadataPath += ".meta";
    next->packMetadata = std::make_unique<SaveRamFile>(metadataPath);
    const auto metadata = next->packMetadata->Load(cart->MemoryPackMetadata().size());
    if (!metadata.empty() && !machine.LoadMemoryPackMetadata(metadata))
      throw std::runtime_error("Invalid memory pack metadata: " + core::PathText(metadataPath));
  }
  machine.AttachVideoOutput(outputs_.video);
  machine.AttachAudioOutput(outputs_.audio);
  machine.AttachInputProvider(outputs_.input);
  state_ = std::move(next);
}

void Session::Flush() {
  if (!state_)
    return;
  auto &state = *state_;
  auto &machine = *state.machine;
  const auto *cart = machine.LoadedCartridge();
  if (state.request.kind == LaunchRequest::Kind::Handheld) {
    state.ram->Flush(machine.SaveSlotBattery(0));
  } else if (state.request.kind == LaunchRequest::Kind::SufamiTurbo) {
    for (unsigned slot = 0; slot < 2; ++slot)
      if (state.slots[slot])
        state.slots[slot]->Flush(cart->SlotSramData(slot));
  } else {
    state.ram->Flush(cart->SramData());
    state.clock->Flush(machine.SaveRtc());
    state.pack->Flush(cart->MemoryPackData());
    state.packMetadata->Flush(cart->MemoryPackMetadata());
  }
}

void Session::Close() {
  Flush();
  state_.reset();
}
void Session::Reset(bool powerCycle) {
  if (!state_)
    return;
  if (powerCycle) {
    Flush();
  }
  if (!(powerCycle ? state_->machine->PowerCycle() : state_->machine->Reset())) {
    throw std::runtime_error("Cannot reset the loaded cartridge");
  }
}

} // namespace snes::frontend
