#pragma once

#include "LaunchRequest.hpp"
#include "snes/core/Emulator.hpp"

#include <array>
#include <memory>
#include <string>

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

private:
  struct State;
  core::Emulator::Config config_;
  Outputs outputs_;
  std::array<core::ControllerDevice, 2> devices_;
  std::unique_ptr<State> state_;
};

} // namespace snes::frontend
