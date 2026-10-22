#include "snes/core/gameboy/StartupState.hpp"

namespace snes::core::gameboy {

void StartupState::Configure(bool enabled, uint64_t seed) noexcept {
  enabled_ = enabled;
  state_ = seed;
}

uint64_t StartupState::SeedFromTime(uint64_t timestamp) noexcept {
  StartupState noise;
  noise.Configure(true, timestamp);
  for (unsigned sample = 0; sample < 64; ++sample)
    noise.NoiseByte();
  return noise.state_;
}

uint8_t StartupState::NoiseByte() noexcept {
  if (!enabled_)
    return 0;
  constexpr uint64_t Multiplier = 0x27bb2ee687b0b0fd;
  constexpr uint64_t Increment = 0xb504f32d;
  const uint64_t product = state_ * Multiplier;
  state_ = product + Increment;
  return uint8_t(state_ >> 56);
}

uint8_t StartupState::Combine(unsigned count, bool mostlySet) noexcept {
  uint8_t bits = NoiseByte();
  for (unsigned sample = 1; sample < count; ++sample) {
    const auto next = NoiseByte();
    bits = mostlySet ? bits | next : bits & next;
  }
  return bits;
}

StartupState::Memory StartupState::Power(bool secondModel) noexcept {
  Memory memory;
  for (unsigned address = 0; address < memory.work.size(); ++address)
    memory.work[address] = secondModel ? 0x55 ^ Combine(3, false) : Combine(2, !(address & 0x100));
  for (unsigned address = 0; address < memory.high.size(); ++address)
    memory.high[address] = Combine(3, address & 1);
  for (unsigned address = 0; address < memory.objects.size(); ++address)
    memory.objects[address] = Combine(3, !(address & 2));
  // The final wave byte retains its cleared reset value.
  for (unsigned address = 0; address + 1 < memory.wave.size(); ++address)
    memory.wave[address] = Combine(3, !(address & 1));
  // Unmapped object-memory cells also advance the persistent startup stream.
  for (unsigned address = 0; address < 96; ++address)
    NoiseByte();
  return memory;
}

} // namespace snes::core::gameboy
