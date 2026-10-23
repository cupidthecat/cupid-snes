#pragma once

#include "LaunchRequest.hpp"
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
    core::IVideoOutput *video = nullptr;
    core::IAudioOutput *audio = nullptr;
    core::IInputProvider *input = nullptr;
  };
  Session(core::Emulator::Config config, Outputs outputs,
          std::array<core::ControllerDevice, 2> devices);
  ~Session();

  // Failed replacement or save writes leave the current machine available.
  void Open(const LaunchRequest &request);
  void Close();
  void Flush();
  void Reset(bool powerCycle = false);
  core::Emulator *Machine() const noexcept;
  const LaunchRequest *Loaded() const noexcept;
  ConsoleDevices Devices() const noexcept;
  void SetDevices(const ConsoleDevices &devices);
  // Change future save destinations without loading or writing battery data.
  // Persist must not replace the loaded game.
  // A rejected path or throwing persist callback retains the current save destinations.
  void SetSaveDirectory(std::filesystem::path directory, const std::function<void()> &persist = {});
  void SetStateDirectory(std::filesystem::path directory,
                         const std::function<void()> &persist = {});
  const StateStorage *States() const noexcept;
  void SaveState(const std::filesystem::path &path);
  void LoadState(const std::filesystem::path &path);
  bool CanUndoState() const noexcept;
  bool CanRedoState() const noexcept;
  void UndoState();
  void RedoState();
  void SetFirmware(std::shared_ptr<const core::CartridgeFirmware> firmware) {
    config_.cartridgeFirmware = std::move(firmware);
  }

private:
  struct State;
  enum class RestoreAction { Load, Undo, Redo };
  void RestoreState(std::span<const uint8_t> bytes, RestoreAction action);
  core::Emulator::Config config_;
  Outputs outputs_;
  std::array<core::ControllerDevice, 2> devices_;
  std::filesystem::path saveDirectory_;
  std::filesystem::path stateDirectory_;
  std::unique_ptr<State> state_;
};

} // namespace snes::frontend
