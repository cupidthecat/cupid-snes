// Resample independent stereo clocks and clip their sum once.
#pragma once

#include "snes/core/AudioTiming.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>

namespace snes::core::audio {

class StreamMixer {
public:
  enum class Input : unsigned { Console, Handheld, Expansion };
  using Output = std::function<void(float left, float right)>;
  void SetOutput(Output output);
  void Reset();
  // A zero rate disables this input. Each call discards its queued history.
  void SetRate(Input input, uint32_t rate);
  void Write(Input input, double left, double right);

private:
  using Frame = std::array<double, 2>;
  struct Stream {
    std::array<Frame, 4> history{};
    std::deque<Frame> ready;
    unsigned cursor = 0;
    uint32_t rate = 0;
    double interval = 1, next = 0;
    void Reset(uint32_t inputRate);
    void Write(Frame frame);
  };
  std::array<Stream, 3> streams_;
  Output output_;
  void MixReady();
};

} // namespace snes::core::audio
