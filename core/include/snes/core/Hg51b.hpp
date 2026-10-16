#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace snes::core {

class Hg51b {
public:
  enum Flag : uint8_t { Negative = 1, Zero = 2, Carry = 4, Overflow = 8, Interrupt = 16 };
  struct Registers {
    std::array<uint32_t, 16> general{};
    uint64_t product = 0;
    uint32_t accumulator = 0, memoryData = 0, romData = 0, ramData = 0;
    uint32_t memoryAddress = 0, dataPointer = 0;
    uint16_t bank = 0, page = 0;
    uint8_t counter = 0, flags = 0;
  };
  struct Control {
    bool locked = false, halted = true, irqDisabled = false, singleRom = true;
    uint8_t romWait = 3, ramWait = 3;
    std::array<uint8_t, 32> vectors{};
    bool suspended = false;
    uint8_t suspendClocks = 0;
    struct Cache {
      bool loading = false, preload = false;
      uint8_t page = 0;
      std::array<bool, 2> locked{};
      std::array<uint32_t, 2> address{};
      uint32_t base = 0;
      uint16_t bank = 0;
      uint8_t counter = 0;
    } cache;
    struct Dma {
      bool active = false;
      uint32_t source = 0, target = 0;
      uint16_t length = 0;
    } dma;
    struct Bus {
      bool active = false, reading = false, writing = false;
      uint8_t pending = 0;
      uint32_t address = 0;
    } bus;
  };
  struct State {
    Registers registers;
    Control control;
    std::array<uint32_t, 8> stack{};
    std::array<std::array<uint16_t, 256>, 2> program{};
    std::array<uint32_t, 1024> dataRom{};
    std::array<uint8_t, 3072> dataRam{};
  };

  virtual ~Hg51b() = default;
  void Reset();
  void RunStep();
  bool LoadDataRom(std::span<const uint8_t> bytes);
  uint8_t ReadHost(uint16_t address) const;
  void WriteHost(uint16_t address, uint8_t value);
  bool Running() const noexcept;
  bool Busy() const noexcept;
  std::span<uint8_t> DataRam() noexcept {
    return state_.dataRam;
  }
  const State &GetState() const noexcept {
    return state_;
  }
  void SetState(const State &state);

protected:
  virtual bool IsRom(uint32_t address) const = 0;
  virtual bool IsRam(uint32_t address) const = 0;
  virtual uint8_t ReadExternal(uint32_t address) = 0;
  virtual void WriteExternal(uint32_t address, uint8_t value) = 0;
  virtual void StepClocks(unsigned clocks) = 0;
  virtual void Halt();
  virtual void ClearIrq() {}
  bool IrqEnabled() const noexcept {
    return !state_.control.irqDisabled;
  }
  void SetInterruptFlag() noexcept {
    state_.registers.flags |= Interrupt;
  }
  bool CanAdvanceIdle() const noexcept;

private:
  static constexpr uint32_t Mask = 0xffffff, Sign = 0x800000;
  State state_;
  bool idleCycle_ = false;
  void Cycle(unsigned clocks);
  unsigned Wait(uint32_t address) const;
  bool CacheReady();
  void Next();
  void Transfer();
  void Execute(uint16_t instruction);
  uint32_t Register(unsigned address);
  void Put(unsigned address, uint32_t value);
  void BusRequest(bool write, bool ram);
  void ResultFlags(uint32_t value);
  uint32_t Arithmetic(uint32_t a, uint32_t b, bool subtract);
  void Branch(uint8_t target, bool far, bool call);
};

} // namespace snes::core
