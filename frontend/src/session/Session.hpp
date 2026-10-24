#pragma once

#include "LaunchRequest.hpp"
#include "Playback.hpp"
#include "cheats/CheatBook.hpp"
#include "debug/CpuDebugger.hpp"
#include "debug/MemoryTools.hpp"
#include "settings/ConsoleDevices.hpp"
#include "snes/core/Emulator.hpp"
#include "storage/StateStorage.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace snes::frontend {

class Session {
  public:
    struct Outputs {
        core::IVideoOutput* video = nullptr;
        core::IAudioOutput* audio = nullptr;
        core::IInputProvider* input = nullptr;
    };

    Session(core::Emulator::Config config, Outputs outputs,
            std::array<core::ControllerDevice, 2> devices);
    ~Session();

    // Failed replacement or save writes leave the current machine available.
    void Open(const LaunchRequest& request);
    void Close();
    void Flush();
    void Reset(bool powerCycle = false);
    core::Emulator* Machine() const noexcept;
    core::FrameStepResult StepFrame(const core::FrameStepOptions& options = {},
                                    bool collectHistory = true);
    Playback& Transport();
    const Playback* TransportIfCreated() const noexcept;
    std::array<uint8_t, 32> MovieCheats() const;
    void SetRewind(const Playback::RewindSettings& settings);
    bool Rewind(const core::StateOperation& operation = {});
    void RequireEditable() const;
    const LaunchRequest* Loaded() const noexcept;
    ConsoleDevices Devices() const noexcept;
    void SetDevices(const ConsoleDevices& devices);
    // Change future save destinations without loading or writing battery data.
    // Persist must not replace the loaded game.
    // A rejected path or throwing persist callback retains the current save destinations.
    void SetSaveDirectory(std::filesystem::path directory,
                          const std::function<void()>& persist = {});
    void SetStateDirectory(std::filesystem::path directory,
                           const std::function<void()>& persist = {});
    const StateStorage* States() const noexcept;
    const CheatBook* Cheats() const noexcept;
    const std::string& CheatError() const noexcept;
    std::filesystem::path CheatPath() const;
    void SaveCheats(CheatBook book, bool replaceUnreadable = false);
    void ReloadCheats();
    MemoryTools& InspectMemory();
    CpuDebugger& DebugCpu();
    void SaveState(const std::filesystem::path& path, const core::StateOperation& operation = {});
    void LoadState(const std::filesystem::path& path, const core::StateOperation& operation = {});
    bool CanUndoState() const noexcept;
    bool CanRedoState() const noexcept;
    void UndoState(const core::StateOperation& operation = {});
    void RedoState(const core::StateOperation& operation = {});

    void SetFirmware(std::shared_ptr<const core::CartridgeFirmware> firmware) {
        config_.cartridgeFirmware = std::move(firmware);
    }

  private:
    struct State;
    enum class RestoreAction { Load, Undo, Redo };
    void RestoreState(std::span<const uint8_t> bytes, RestoreAction action,
                      const core::StateOperation& operation);
    void RestorePrepared(core::Emulator::PreparedStateRestore prepared, RestoreAction action,
                         const core::StateOperation& operation);
    Playback& EnsureTransport(State& state);
    void RequireReplaceable() const;
    core::Emulator::Config config_;
    Outputs outputs_;
    std::array<core::ControllerDevice, 2> devices_;
    std::filesystem::path saveDirectory_;
    std::filesystem::path stateDirectory_;
    Playback::RewindSettings rewind_;
    std::unique_ptr<State> state_;
};

} // namespace snes::frontend
