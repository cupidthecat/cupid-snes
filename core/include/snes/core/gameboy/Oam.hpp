// Object memory and CPU bus contention during LCD scanning.
#pragma once

#include <array>
#include <cstdint>

namespace snes::core::detail {
class StateArchive;
}

namespace snes::core::gameboy {

class Oam {
public:
  void SerializeState(detail::StateArchive &archive);
  void Reset(bool secondModel) noexcept;
  uint8_t Read(uint8_t address) const noexcept;
  void Write(uint8_t address, uint8_t value) noexcept;
  uint8_t ReadCpu(uint8_t address, uint8_t row, uint8_t blocked) noexcept;
  void WriteCpu(uint8_t address, uint8_t value, uint8_t row, bool blocked) noexcept;
  void AddressBus(uint8_t row) noexcept;
  void BlockedRead(uint8_t row) noexcept;

private:
  using Row = std::array<uint8_t, 8>;
  std::array<uint8_t, 160> bytes_{};
  bool secondModel_ = false;

  uint16_t Word(unsigned address) const noexcept;
  void StoreWord(unsigned address, uint16_t value) noexcept;
  Row LoadRow(unsigned address) const noexcept;
  void StoreRow(unsigned address, const Row &row) noexcept;
};

} // namespace snes::core::gameboy
