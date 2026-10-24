#pragma once

#include <cstdint>
#include <span>

namespace snes::core {
namespace detail {
class StateArchive;
}

enum class EntropyMode { None, Low, High };

// One stream supplies memory contents and register bits during console startup.
class PowerOnState {
public:
  PowerOnState(EntropyMode mode, uint32_t seed, uint32_t sequence = 0);
  uint64_t Value(uint64_t fallback = 0);
  void Fill(std::span<uint8_t> bytes);
  void SerializeState(detail::StateArchive &archive);

private:
  uint32_t Step();
  void FlipBit(uint8_t &byte, unsigned mask);

  EntropyMode mode_;
  uint64_t state_ = 0, increment_;
};

} // namespace snes::core
