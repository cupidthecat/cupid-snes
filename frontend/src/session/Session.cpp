#include "Session.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/BatteryStorage.hpp"

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
namespace {
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
  std::unique_ptr<BatteryStorage> battery;
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

void Session::SetSaveDirectory(std::filesystem::path directory,
                               const std::function<void()> &persist) {
  if (directory.native().find(std::filesystem::path::value_type(0)) !=
      std::filesystem::path::string_type::npos)
    throw std::invalid_argument("Save directory contains a null character");
  if (!directory.empty()) {
    directory = std::filesystem::weakly_canonical(std::filesystem::absolute(directory));
    std::filesystem::create_directories(directory);
    if (!std::filesystem::is_directory(directory))
      throw std::runtime_error("Save path must be a directory: " + core::PathText(directory));
  }
  std::unique_ptr<BatteryStorage> battery;
  if (directory != saveDirectory_ && state_) {
    battery = std::make_unique<BatteryStorage>(state_->request, *state_->machine, directory);
  }
  if (persist)
    persist();
  if (directory != saveDirectory_) {
    saveDirectory_ = std::move(directory);
    if (state_)
      state_->battery = std::move(battery);
  }
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
  next->battery = std::make_unique<BatteryStorage>(request, machine, saveDirectory_);
  next->battery->Load(machine);
  machine.AttachVideoOutput(outputs_.video);
  machine.AttachAudioOutput(outputs_.audio);
  machine.AttachInputProvider(outputs_.input);
  state_ = std::move(next);
}

void Session::Flush() {
  if (!state_)
    return;
  state_->battery->Flush(*state_->machine);
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
