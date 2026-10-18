#include "snes/core/PowerOnState.hpp"

#include <algorithm>
#include <bit>

namespace snes::core {

PowerOnState::PowerOnState(EntropyMode mode, uint32_t seed, uint32_t sequence)
    : mode_(mode), increment_(uint32_t(sequence * 2 + 1)) {
  Step();
  state_ += seed;
  Step();
}

uint64_t PowerOnState::Value(uint64_t fallback) {
  if (mode_ == EntropyMode::None)
    return fallback;
  const uint64_t high = Step();
  return (high << 32) | Step();
}

void PowerOnState::Fill(std::span<uint8_t> bytes) {
  if (mode_ == EntropyMode::None) {
    std::fill(bytes.begin(), bytes.end(), 0);
    return;
  }
  if (mode_ == EntropyMode::High) {
    for (auto &byte : bytes)
      byte = uint8_t(Value());
    return;
  }

  const unsigned selectBit = unsigned(Value() & 3);
  const unsigned invertBit = (selectBit + 8 + unsigned(Value() & 3)) & 15;
  uint8_t selected = uint8_t(Value());
  uint8_t unselected = uint8_t(Value());
  if ((Value() & 3) == 0)
    selected = 0;
  if ((Value() & 1) == 0)
    unselected = uint8_t(~selected);
  for (size_t index = 0; index < bytes.size(); ++index) {
    uint8_t byte = (index >> selectBit) & 1 ? selected : unselected;
    if ((index >> invertBit) & 1)
      byte ^= 0xff;
    FlipBit(byte, 511);
    FlipBit(byte, 2047);
    bytes[index] = byte;
  }
}

uint32_t PowerOnState::Step() {
  const auto before = state_;
  state_ = before * uint64_t(6364136223846793005ull) + increment_;
  const uint32_t mixed = uint32_t(((before >> 18) ^ before) >> 27);
  return std::rotr(mixed, int(before >> 59));
}

void PowerOnState::FlipBit(uint8_t &byte, unsigned mask) {
  if ((Value() & mask) == 0)
    byte ^= uint8_t(1u << (Value() & 7));
}

} // namespace snes::core
