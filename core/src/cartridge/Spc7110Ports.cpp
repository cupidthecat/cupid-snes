#include "snes/core/Spc7110.hpp"

namespace snes::core {
uint32_t Spc7110::Word(unsigned reg, unsigned bytes) const {
  uint32_t value = 0;
  for (unsigned i = 0; i < bytes; ++i)
    value |= uint32_t(registers_[reg + i]) << (i * 8);
  return value;
}
void Spc7110::Store(unsigned reg, unsigned bytes, uint32_t value) {
  for (unsigned i = 0; i < bytes; ++i)
    registers_[reg + i] = uint8_t(value >> (i * 8));
  if (reg == 0x11)
    registers_[0x13] &= 0x7f;
}
void Spc7110::RefreshData() {
  const uint32_t adjust = registers_[0x18] & 2 ? Word(0x14, 2) : 0;
  const int32_t displacement = registers_[0x18] & 8 ? int16_t(adjust) : int32_t(adjust);
  registers_[0x10] = Data(Word(0x11, 3) + displacement);
}
void Spc7110::AdjustData(unsigned trigger) {
  const auto control = registers_[0x18];
  if ((control >> 5) != trigger)
    return;
  const uint32_t adjust = Word(0x14, 2);
  const int32_t displacement = control & 8 ? int16_t(adjust) : int32_t(adjust);
  Store(0x11, 3, Word(0x11, 3) + displacement);
  RefreshData();
}
uint8_t Spc7110::ReadStream() {
  if (!(registers_[0x0c] & 0x80))
    return 0;
  const unsigned depth = decoder_.BitDepth();
  if (!tileOffset_) {
    for (unsigned row = 0; row < 8; ++row) {
      for (unsigned plane = 0; plane < depth; ++plane) {
        const unsigned offset = depth == 1 ? row : row * 2 + (plane & 1) + (plane >> 1) * 16;
        tile_[offset] = uint8_t(decodedRow_ >> (plane * 8));
      }
      const unsigned stride = registers_[11] & 1 ? registers_[7] : 1;
      for (unsigned n = 0; n < stride; ++n)
        decodedRow_ = decoder_.NextRow();
    }
  }
  const uint8_t value = tile_[tileOffset_];
  tileOffset_ = uint8_t((tileOffset_ + 1) & (8 * depth - 1));
  return value;
}
uint8_t Spc7110::ReadRegister(unsigned reg, uint8_t openBus) {
  if (reg == 0) {
    Store(9, 2, Word(9, 2) - 1);
    return ReadStream();
  }
  if (reg == 8)
    return 0;
  if (reg == 0x10) {
    const uint8_t value = registers_[0x10], control = registers_[0x18];
    const uint32_t raw = control & 1 ? Word(0x16, 2) : 1;
    const int32_t stride = control & 4 ? int16_t(raw) : int32_t(raw);
    const unsigned target = control & 16 ? 0x14 : 0x11;
    Store(target, target == 0x14 ? 2 : 3, Word(target, target == 0x14 ? 2 : 3) + stride);
    RefreshData();
    return value;
  }
  if (reg == 0x1a) {
    AdjustData(3);
    return 0;
  }
  if ((reg >= 1 && reg <= 12) || (reg >= 0x11 && reg <= 0x18) || (reg >= 0x20 && reg <= 0x2f) ||
      (reg >= 0x30 && reg <= 0x34))
    return registers_[reg];
  return openBus;
}
void Spc7110::WriteRegister(unsigned reg, uint8_t value) {
  if ((reg >= 1 && reg <= 7) || (reg >= 9 && reg <= 11) || (reg >= 0x11 && reg <= 0x18) ||
      (reg >= 0x20 && reg <= 0x27) || reg == 0x2e || (reg >= 0x30 && reg <= 0x34)) {
    uint8_t mask = 0xff;
    if (reg == 3 || reg == 0x13 || reg == 0x18)
      mask = 0x7f;
    if (reg == 11)
      mask = 3;
    if (reg == 0x2e)
      mask = 1;
    if (reg == 0x30)
      mask = 0x87;
    if (reg >= 0x31)
      mask = 7;
    registers_[reg] = value & mask;
  } else
    return;
  switch (reg) {
  case 4: {
    const uint32_t entry = Word(1, 3) + uint32_t(value) * 4;
    mode_ = Data(entry) & 3;
    streamAddress_ =
        ((uint32_t(Data(entry + 1)) << 16) | (uint32_t(Data(entry + 2)) << 8) | Data(entry + 3)) &
        0x7fffff;
    break;
  }
  case 6:
    registers_[12] &= 0x7f;
    decodePending_ = true;
    break;
  case 0x13:
  case 0x18:
    RefreshData();
    break;
  case 0x14:
    AdjustData(1);
    break;
  case 0x15:
    if (registers_[0x18] & 2)
      RefreshData();
    AdjustData(2);
    break;
  case 0x25:
    registers_[0x2f] |= 0x81;
    multiplyPending_ = true;
    break;
  case 0x27:
    registers_[0x2f] |= 0x80;
    dividePending_ = true;
    break;
  }
}
} // namespace snes::core
