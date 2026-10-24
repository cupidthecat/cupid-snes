#include "Session.hpp"
#include "GameAssets.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/BatteryStorage.hpp"

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
namespace {
auto PlayersFor(const std::array<core::ControllerDevice, 2>& devices) {
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

void ConfigurePorts(core::Emulator& machine, const std::array<core::ControllerDevice, 2>& devices,
                    const std::array<core::ControllerDevice, 2>* previous = nullptr) {
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
    std::unique_ptr<StateStorage> states;
    std::vector<uint8_t> undo, redo;
    CheatBook cheats;
    std::filesystem::path cheatPath;
    std::string cheatError;
    std::unique_ptr<MemoryTools> memory;
    std::unique_ptr<CpuDebugger> debugger;
    std::unique_ptr<Playback> playback;
};

CpuDebugger& Session::DebugCpu() {
    RequireEditable();
    if (!state_)
        throw std::runtime_error("The CPU debugger requires a loaded game");
    if (!state_->debugger)
        state_->debugger = std::make_unique<CpuDebugger>(*state_->machine);
    return *state_->debugger;
}

Session::Session(core::Emulator::Config config, Outputs outputs,
                 std::array<core::ControllerDevice, 2> devices)
    : config_(std::move(config)), outputs_(outputs), devices_(devices) {
    Devices().Validate();
}

Session::~Session() = default;

core::Emulator* Session::Machine() const noexcept {
    return state_ ? state_->machine.get() : nullptr;
}

Playback& Session::EnsureTransport(State& state) {
    if (!state.playback) {
        auto playback = std::make_unique<Playback>(
            *state.machine, outputs_.input,
            [this](core::Emulator::PreparedStateRestore prepared,
                   const core::StateOperation& operation) {
                RestorePrepared(std::move(prepared), RestoreAction::Load, operation);
            });
        playback->ConfigureRewind(rewind_);
        state.playback = std::move(playback);
    }
    return *state.playback;
}

Playback& Session::Transport() {
    if (!state_)
        throw std::runtime_error("Movie controls require a loaded game");
    return EnsureTransport(*state_);
}

const Playback* Session::TransportIfCreated() const noexcept {
    return state_ ? state_->playback.get() : nullptr;
}

core::FrameStepResult Session::StepFrame(const core::FrameStepOptions& options,
                                         bool collectHistory) {
    if (!state_)
        throw std::runtime_error("Frame advance requires a loaded game");
    return state_->playback ? state_->playback->StepFrame(options, collectHistory)
                            : state_->machine->StepFrame(options);
}

std::array<uint8_t, 32> Session::MovieCheats() const {
    if (!state_)
        throw std::runtime_error("Movie controls require a loaded game");
    const auto system = state_->machine->CheatsSystem();
    return Playback::CheatIdentity(system, state_->cheats.Compile(system));
}

void Session::SetRewind(const Playback::RewindSettings& settings) {
    // Validate before creating a live input wrapper or changing retained history.
    Playback::ValidateRewind(settings);
    if (state_ && (settings.interval || state_->playback))
        EnsureTransport(*state_).ConfigureRewind(settings);
    rewind_ = settings;
}

bool Session::Rewind(const core::StateOperation& operation) {
    if (!state_ || !state_->playback || !state_->playback->Rewind(operation))
        return false;
    if (state_->debugger)
        state_->debugger->Repositioned();
    devices_ = state_->machine->GetAutoJoypad().Ports().Devices();
    return true;
}

void Session::RequireEditable() const {
    const auto* playback = TransportIfCreated();
    if (playback && (playback->Recording() || playback->Replaying()))
        throw std::runtime_error("Stop the movie before changing the machine or its settings");
}

void Session::RequireReplaceable() const {
    RequireEditable();
    if (const auto* playback = TransportIfCreated(); playback && playback->PendingMovie())
        throw std::runtime_error("Save or discard the recorded movie before closing the game");
}

const LaunchRequest* Session::Loaded() const noexcept {
    return state_ ? &state_->request : nullptr;
}

ConsoleDevices Session::Devices() const noexcept {
    return {devices_, config_.expansionDevice};
}

void Session::SetDevices(const ConsoleDevices& devices) {
    devices.Validate();
    if (devices == Devices())
        return;
    RequireEditable();
    if (auto* machine = Machine()) {
        ConfigurePorts(*machine, devices.ports, &devices_);
        if (devices.expansion != config_.expansionDevice)
            machine->SetExpansionDevice(devices.expansion);
        if (state_->playback)
            state_->playback->TimelineChanged();
    }
    devices_ = devices.ports;
    config_.expansionDevice = devices.expansion;
}

void Session::SetSaveDirectory(std::filesystem::path directory,
                               const std::function<void()>& persist) {
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

void Session::SetStateDirectory(std::filesystem::path directory,
                                const std::function<void()>& persist) {
    if (directory.native().find(std::filesystem::path::value_type(0)) !=
        std::filesystem::path::string_type::npos)
        throw std::invalid_argument("State directory contains a null character");
    if (!directory.empty()) {
        directory = std::filesystem::weakly_canonical(std::filesystem::absolute(directory));
        std::filesystem::create_directories(directory);
        if (!std::filesystem::is_directory(directory))
            throw std::runtime_error("State path must be a directory: " +
                                     core::PathText(directory));
    }
    std::unique_ptr<StateStorage> next;
    if (state_)
        next = std::make_unique<StateStorage>(state_->request, directory);
    if (persist)
        persist();
    stateDirectory_ = std::move(directory);
    if (state_)
        state_->states = std::move(next);
}

const StateStorage* Session::States() const noexcept {
    return state_ ? state_->states.get() : nullptr;
}

void Session::SaveState(const std::filesystem::path& path, const core::StateOperation& operation) {
    RequireEditable();
    if (!state_)
        throw std::runtime_error("Saving a state requires a loaded game");
    auto bytes = state_->machine->CaptureState(operation);
    StateFile::Write(path, bytes, state_->machine->LastFrame(), operation);
}

void Session::LoadState(const std::filesystem::path& path, const core::StateOperation& operation) {
    const auto file = StateFile::Read(path, operation);
    RestoreState(file.machine, RestoreAction::Load, operation);
}

bool Session::CanUndoState() const noexcept {
    return state_ && !state_->undo.empty();
}

bool Session::CanRedoState() const noexcept {
    return state_ && !state_->redo.empty();
}

void Session::UndoState(const core::StateOperation& operation) {
    if (!CanUndoState())
        throw std::runtime_error("There is no state load to undo");
    RestoreState(state_->undo, RestoreAction::Undo, operation);
}

void Session::RedoState(const core::StateOperation& operation) {
    if (!CanRedoState())
        throw std::runtime_error("There is no state load to redo");
    RestoreState(state_->redo, RestoreAction::Redo, operation);
}

void Session::RestoreState(std::span<const uint8_t> bytes, RestoreAction action,
                           const core::StateOperation& operation) {
    RequireEditable();
    if (!state_)
        throw std::runtime_error("Loading a state requires a loaded game");
    auto prepared = state_->machine->PrepareStateRestore(bytes, operation);
    RestorePrepared(std::move(prepared), action, operation);
    if (state_->playback)
        state_->playback->TimelineChanged();
}

void Session::RestorePrepared(core::Emulator::PreparedStateRestore prepared, RestoreAction action,
                              const core::StateOperation& operation) {
    auto& machine = *state_->machine;
    auto previous = machine.CaptureState(operation);
    prepared.Commit();
    if (state_->debugger)
        state_->debugger->Repositioned();
    if (action == RestoreAction::Undo) {
        state_->redo = std::move(previous);
        state_->undo.clear();
    } else {
        state_->undo = std::move(previous);
        state_->redo.clear();
    }
    // The restored port packets already contain their device and player wiring.
    devices_ = machine.GetAutoJoypad().Ports().Devices();
}

void Session::Open(const LaunchRequest& request, const core::StateOperation& operation) {
    RequireReplaceable();
    auto plan = GameAssets::Resolve(request, operation);
    if (!plan.Ready())
        throw std::runtime_error("Archive contains multiple ROMs; select one before opening it");
    const auto& launch = plan.request;
    Flush();
    auto next = std::make_unique<State>();
    next->request = launch;
    next->machine = std::make_unique<core::Emulator>(config_);
    auto& machine = *next->machine;
    ConfigurePorts(machine, devices_);
    using Kind = LaunchRequest::Kind;
    const auto& paths = launch.paths;
    const bool archived = GameAssets::Archived(launch);
    std::array<std::vector<uint8_t>, 4> images;
    if (archived)
        for (unsigned index = 0; index < images.size(); ++index)
            images[index] = GameAssets::Read(launch, index, operation);
    const auto origin = archived ? core::PathText(GameAssets::Origin(launch, 0)) : std::string{};
    std::string error;
    bool loaded = false;
    switch (request.kind) {
    case Kind::Cartridge:
        loaded = archived ? machine.LoadCartridge(images[0], &error, origin)
                          : machine.LoadCartridgeFromFile(paths[0], &error);
        break;
    case Kind::Handheld:
        loaded = archived
                     ? machine.LoadSuperGameBoy(images[0], images[1], images[2], &error, origin)
                     : machine.LoadSuperGameBoyFromFiles(paths[0], paths[1], paths[2], &error);
        break;
    case Kind::SufamiTurbo:
        loaded = archived ? machine.LoadSufamiTurbo(images[0], images[1], images[2], &error)
                          : machine.LoadSufamiTurboFromFiles(paths[0], paths[1], paths[2], &error);
        break;
    case Kind::Broadcast:
        loaded = archived ? machine.LoadBroadcastCartridge(images[0], images[1], &error)
                          : machine.LoadBroadcastCartridgeFromFiles(paths[0], paths[1], &error);
        break;
    case Kind::Campus92:
    case Kind::PowerFest94: {
        const core::CompetitionBoard::Settings settings{
            request.kind == Kind::Campus92 ? core::CompetitionBoard::Board::CampusChallenge92
                                           : core::CompetitionBoard::Board::PowerFest94,
            request.competitionDip, 0x2000};
        if (archived) {
            std::array<std::span<const uint8_t>, 4> views;
            for (unsigned index = 0; index < views.size(); ++index)
                views[index] = images[index];
            loaded = machine.LoadCompetitionCartridge(settings, views, &error, origin);
        } else
            loaded = machine.LoadCompetitionCartridgeFromFiles(settings, paths, &error);
        break;
    }
    }
    if (!loaded)
        throw std::runtime_error(error.empty() ? "Cannot load the selected cartridge" : error);
    next->battery = std::make_unique<BatteryStorage>(launch, machine, saveDirectory_);
    next->battery->Load(machine);
    next->states = std::make_unique<StateStorage>(launch, stateDirectory_);
    next->cheatPath = StateStorage(launch).Directory() / "cheats.ccht";
    try {
        next->cheats = CheatStorage::Read(next->cheatPath, machine.CheatsSystem());
        machine.SetCheats(next->cheats.Compile(machine.CheatsSystem()));
    } catch (const std::exception& failure) {
        next->cheatError = failure.what();
    }
    machine.AttachVideoOutput(outputs_.video);
    machine.AttachAudioOutput(outputs_.audio);
    machine.AttachInputProvider(outputs_.input);
    if (rewind_.interval)
        EnsureTransport(*next);
    state_ = std::move(next);
}

void Session::Flush() {
    if (!state_)
        return;
    state_->battery->Flush(*state_->machine);
}

const CheatBook* Session::Cheats() const noexcept {
    return state_ ? &state_->cheats : nullptr;
}

const std::string& Session::CheatError() const noexcept {
    static const std::string empty;
    return state_ ? state_->cheatError : empty;
}

std::filesystem::path Session::CheatPath() const {
    return state_ ? state_->cheatPath : std::filesystem::path{};
}

void Session::SaveCheats(CheatBook book, bool replaceUnreadable) {
    RequireEditable();
    if (!state_)
        throw std::runtime_error("Cheats require a loaded game");
    if (!state_->cheatError.empty() && !replaceUnreadable)
        throw std::runtime_error("Reload the unreadable cheat file or confirm its replacement");
    const auto system = state_->machine->CheatsSystem();
    const auto codes = book.Compile(system);
    CheatStorage::Write(state_->cheatPath, book, system);
    state_->machine->SetCheats(codes);
    state_->cheats = std::move(book);
    state_->cheatError.clear();
    if (state_->playback)
        state_->playback->TimelineChanged();
}

void Session::ReloadCheats() {
    RequireEditable();
    if (!state_)
        throw std::runtime_error("Cheats require a loaded game");
    auto book = CheatStorage::Read(state_->cheatPath, state_->machine->CheatsSystem());
    state_->machine->SetCheats(book.Compile(state_->machine->CheatsSystem()));
    state_->cheats = std::move(book);
    state_->cheatError.clear();
    if (state_->playback)
        state_->playback->TimelineChanged();
}

MemoryTools& Session::InspectMemory() {
    RequireEditable();
    if (!state_)
        throw std::runtime_error("Memory tools require a loaded game");
    if (!state_->memory)
        state_->memory = std::make_unique<MemoryTools>(*state_->machine);
    return *state_->memory;
}

void Session::Close() {
    RequireReplaceable();
    Flush();
    state_.reset();
}

void Session::Reset(bool powerCycle) {
    if (!state_)
        return;
    RequireEditable();
    if (powerCycle) {
        Flush();
    }
    if (!(powerCycle ? state_->machine->PowerCycle() : state_->machine->Reset())) {
        throw std::runtime_error("Cannot reset the loaded cartridge");
    }
    if (state_->debugger)
        state_->debugger->Repositioned();
    if (state_->playback)
        state_->playback->TimelineChanged();
}

} // namespace snes::frontend
