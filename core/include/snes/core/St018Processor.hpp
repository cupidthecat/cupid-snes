#pragma once

#include "snes/core/Arm6.hpp"

#include <array>
#include <memory>
#include <span>

namespace snes::core {
namespace detail {
class ExecutionContext;
}

class St018Processor : private Arm6 {
public:
  explicit St018Processor(std::span<const uint8_t> firmware);
  ~St018Processor();
  void Reset();
  void SetPal(bool pal);
  void Advance(uint32_t masterClocks);
  uint8_t Read(uint32_t address);
  void Write(uint32_t address, uint8_t value);
  static bool Selects(uint32_t address) noexcept {
    return (address & 0x40ff00) == 0x003800;
  }

private:
  uint8_t Status() const;
  void Synchronize();
  void Restart();
  void Cycle(unsigned clocks);
  uint32_t ReadArm(unsigned access, uint32_t address) override;
  void WriteArm(unsigned access, uint32_t address, uint32_t value) override;
  void IdleArm() override;
  std::array<uint8_t, 0x28000> firmware_{};
  std::array<uint8_t, 0x4000> ram_{};
  std::unique_ptr<detail::ExecutionContext> execution_;
  uint32_t masterFrequency_ = 21'477'272;
  int64_t balance_ = 0;
  uint32_t timer_ = 0, timerLatch_ = 0;
  uint8_t input_ = 0, output_ = 0;
  bool inputReady_ = false, outputReady_ = false;
  bool ready_ = false, signal_ = false, reset_ = false;
};

} // namespace snes::core
