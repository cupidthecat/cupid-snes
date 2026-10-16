#pragma once

#include <array>
#include <cstdint>

namespace snes::core {

class Arm6 {
public:
  enum Access : unsigned {
    Byte = 0,
    Half = 1,
    Word = 2,
    Width = 3,
    Sequential = 4,
    Fetch = 8,
    Load = 16,
    Store = 32
  };
  struct State {
    std::array<uint32_t, 16> common{};
    std::array<uint32_t, 7> fiq{};
    std::array<std::array<uint32_t, 2>, 4> bank{};
    std::array<uint32_t, 5> saved{};
    uint32_t status = 0;
  };

  virtual ~Arm6() = default;
  void Reset();
  void RunInstruction();
  const State &GetState() const noexcept {
    return state_;
  }
  void SetState(const State &state);

protected:
  virtual uint32_t ReadArm(unsigned access, uint32_t address) = 0;
  virtual void WriteArm(unsigned access, uint32_t address, uint32_t value) = 0;
  virtual void IdleArm() = 0;
  uint32_t PrefetchedWord() const noexcept {
    return fetched_;
  }

private:
  static constexpr uint32_t N = 0x80000000, Z = 0x40000000, C = 0x20000000, V = 0x10000000;
  State state_;
  uint32_t fetched_ = 0, decoded_ = 0, decodedAddress_ = 0;
  bool reload_ = true, sequential_ = false, carry_ = false;
  uint32_t &Register(unsigned index);
  uint32_t &SavedStatus();
  void Put(unsigned index, uint32_t value);
  void Exception(unsigned mode, uint32_t vector);
  void Prefetch();
  void Idle();
  bool Condition(unsigned condition) const;
  bool ExceptionMode() const;
  uint32_t Shift(uint32_t value, unsigned type, unsigned count, bool immediate);
  void ResultFlags(uint32_t value);
  uint32_t Add(uint32_t a, uint32_t b, bool carry, bool flags);
  void Data(uint32_t instruction);
  void Transfer(uint32_t instruction, bool extended);
  void Multiple(uint32_t instruction);
  void Multiply(uint32_t instruction, bool wide);
  void Status(uint32_t instruction, bool read);
  uint32_t LoadValue(unsigned width, bool sign, uint32_t address);
  void StoreValue(unsigned width, uint32_t address, uint32_t value);
  void Execute(uint32_t instruction);
};

} // namespace snes::core
