// Align the adapter and console streams before stereo clipping.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>

namespace snes::core::gameboy {

class AudioMixer {
public:
  using Output = std::function<void(float left, float right)>;
  void SetOutput(Output output);
  void Reset(uint32_t handheldRate);
  // Every control write restarts the handheld stream, including unchanged rates.
  void SetHandheldRate(uint32_t rate);
  void WriteHandheld(int16_t left, int16_t right);
  void WriteConsole(int16_t left, int16_t right);

private:
  using Frame = std::array<double, 2>;
  struct Stream {
    std::array<Frame, 4> history{};
    std::deque<Frame> ready;
    unsigned cursor = 0;
    double interval = 1, next = 0;
    void Reset(uint32_t inputRate);
    void Write(Frame frame);
  };
  Output output_;
  Stream handheld_, console_;
  uint32_t handheldRate_ = 0;
  void MixReady();
};

} // namespace snes::core::gameboy
