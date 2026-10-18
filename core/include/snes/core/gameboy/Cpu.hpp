// SM83 instruction execution and interrupt control.
#pragma once

#include <cstdint>

namespace snes::core::gameboy {

class Cpu {
public:
  struct Bus {
    virtual ~Bus() = default;
    // Reads and writes do not advance time. The bus can defer register effects
    // within the following clock interval when the addressed hardware needs it.
    virtual uint8_t Read(uint16_t address) = 0;
    virtual void Write(uint16_t address, uint8_t value) = 0;
    virtual void Advance(unsigned clocks) = 0;
    virtual uint8_t InterruptEnable() const = 0;
    virtual uint8_t InterruptFlags() const = 0;
    virtual void AcknowledgeInterrupt(uint8_t mask) = 0;
    virtual void DriveAddress(uint16_t) {}
    virtual void AddressIncremented(uint16_t) {}
    virtual void SetStopped(bool) {}
  };

  struct State {
    uint8_t a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    uint16_t sp = 0, pc = 0;
    bool ime = false;
    bool enablePending = false;
    bool halted = false;
    bool firstHaltWait = false;
    bool haltBug = false;
    bool stopped = false;
  };

  explicit Cpu(Bus &bus) : bus_(bus) {}
  void Reset() noexcept;
  void RunInstruction();
  const State &GetState() const noexcept {
    return state_;
  }
  void SetState(const State &state) noexcept;

private:
  static constexpr uint8_t Zero = 0x80, Subtract = 0x40, Half = 0x20, Carry = 0x10;
  Bus &bus_;
  State state_{};

  uint8_t ReadByte(uint16_t address);
  void WriteByte(uint16_t address, uint8_t value);
  uint8_t Fetch();
  uint16_t FetchWord();
  void Idle();
  void IdleAt(uint16_t address);
  uint16_t Pair(unsigned pair) const;
  void SetPair(unsigned pair, uint16_t value);
  uint16_t StackPair(unsigned pair) const;
  void SetStackPair(unsigned pair, uint16_t value);
  uint8_t Register(unsigned index);
  void SetRegister(unsigned index, uint8_t value);
  bool Condition(unsigned condition) const;
  uint16_t Pop();
  void Push(uint16_t value);
  void JumpRelative();
  void AddHl(uint16_t value);
  void AddStackOffset(bool toHl);
  uint8_t Increment(uint8_t value);
  uint8_t Decrement(uint8_t value);
  void Arithmetic(unsigned operation, uint8_t value);
  void DecimalAdjust();
  void AccumulatorRotate(unsigned operation);
  void Extended(uint8_t opcode);
  void Execute(uint8_t opcode);
  void Interrupt();
};

} // namespace snes::core::gameboy
