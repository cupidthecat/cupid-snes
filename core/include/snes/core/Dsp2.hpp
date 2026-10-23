// DSP-2 command interface.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace snes::core {
namespace detail {
class StateArchive;
}

// Byte-oriented command interface for the cartridge bitmap processor.
class Dsp2 {
public:
  // Warm reset clears the protocol while retaining both physical byte buffers.
  void Reset();
  void SerializeState(detail::StateArchive &archive);
  uint8_t Read();
  void Write(uint8_t value);

private:
  void Complete(uint8_t value);
  void Execute();
  std::array<uint8_t, 512> input_{};
  std::array<uint8_t, 512> output_{};
  uint32_t received_ = 0;
  uint32_t needed_ = 0;
  uint32_t read_ = 0;
  uint32_t available_ = 0;
  std::array<uint32_t, 3> lengths_{};
  std::array<bool, 3> payloads_{};
  uint32_t scaledLength_ = 0;
  uint8_t command_ = 0;
  uint8_t transparent_ = 0;
  bool commandReady_ = true;
};

} // namespace snes::core
