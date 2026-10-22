// SA-1 normal DMA and packed/unpacked character conversion.
#include "snes/core/Sa1.hpp"
#include "snes/core/detail/ExecutionContext.hpp"

#include <algorithm>

namespace snes::core {

void Sa1::StartDma() {
  const auto count = RegisterWord(0x38);
  const auto route = registers_[0x30] & 7;
  if (!count || route == 2 || route == 3 || route == 5 || route == 7) {
    // An unselected port pair consumes the count without memory cycles.
    StoreAddress(0x32, RegisterAddress(0x32) + count);
    StoreAddress(0x35, RegisterAddress(0x35) + count);
    registers_[0x38] = registers_[0x39] = 0xff;
    dmaActive_ = false;
    RaiseSa1Interrupt(0x20);
    return;
  }
  dmaActive_ = true;
  if (execution_ && execution_->Running())
    while (dmaActive_)
      StepDma();
}

void Sa1::StepDma() {
  const auto remaining = RegisterWord(0x38);
  const auto next = static_cast<uint16_t>(remaining - 1);
  registers_[0x38] = static_cast<uint8_t>(next);
  registers_[0x39] = static_cast<uint8_t>(next >> 8);
  if (!remaining) {
    dmaActive_ = false;
    RaiseSa1Interrupt(0x20);
    return;
  }
  const auto source = RegisterAddress(0x32);
  const auto destination = RegisterAddress(0x35);
  const auto openBus = regs().mdr;
  StoreAddress(0x32, source + 1);
  StoreAddress(0x35, destination + 1);

  // Control writes can change the port pair while a memory cycle is suspended.
  for (uint8_t route : {4, 0, 1, 6}) {
    if ((registers_[0x30] & 7) != route)
      continue;
    const bool toBwram = (route & 4) != 0;
    const auto device = route & 3;
    Clock(2);
    if (route == 0) {
      if (CpuIramConflict() || CpuRomConflict())
        Clock(2);
      if (CpuIramConflict())
        Clock(2);
    } else {
      Clock(2);
      if (CpuBwramConflict() || (device != 0 && CpuIramConflict()))
        Clock(2);
      if (CpuBwramConflict())
        Clock(2);
    }
    uint8_t value = openBus;
    if (device == 0) {
      // DMA selects the ROM port independently of the processor's address windows.
      value = ReadRomPort(source, openBus);
    } else if (device == 1) {
      value = ReadBwram(source, openBus);
    } else {
      value = iram_[source & 0x7ff];
    }
    // DMA owns the memory port, independently of processor write protection.
    if (toBwram)
      WriteBwram(destination, value, Side::Sa1, true);
    else
      iram_[destination & 0x7ff] = value;
  }
  if (!RegisterWord(0x38)) {
    registers_[0x38] = registers_[0x39] = 0xff;
    dmaActive_ = false;
    RaiseSa1Interrupt(0x20);
  }
}

uint8_t Sa1::ConversionDepth() const noexcept {
  switch (registers_[0x31] & 3) {
  case 0:
    return 8;
  case 1:
    return 4;
  default:
    return 2;
  }
}

void Sa1::WritePlanarTile(const std::array<uint8_t, 64> &pixels, uint16_t destination) {
  const unsigned depth = ConversionDepth();
  for (unsigned plane = 0; plane < depth; ++plane) {
    for (unsigned y = 0; y < 8; ++y) {
      uint8_t value = 0;
      for (unsigned x = 0; x < 8; ++x)
        value |= ((pixels[y * 8 + x] >> plane) & 1) << (7 - x);
      const unsigned offset = (plane / 2) * 16 + y * 2 + (plane & 1);
      iram_[(destination + offset) & 0x7ff] = value;
    }
  }
}

void Sa1::StartConversion() {
  conversionActive_ = true;
  RaiseCpuInterrupt(0x20);
}

void Sa1::ConvertBitmapTile(uint32_t tile) {
  const unsigned depth = ConversionDepth();
  const unsigned width = 1u << std::min((registers_[0x31] >> 2) & 7, 5);
  const uint32_t tileRow = tile / width;
  const uint32_t tileColumn = tile % width;
  const auto source = RegisterAddress(0x32);
  const auto mask = static_cast<uint32_t>(bwram_.size()) - 1;
  std::array<uint8_t, 64> pixels{};
  for (unsigned y = 0; y < 8; ++y) {
    const uint32_t sourceRow = source + (tileRow * 8 + y) * width * depth + tileColumn * depth;
    for (unsigned x = 0; x < 8; ++x) {
      const unsigned bit = x * depth;
      pixels[y * 8 + x] = static_cast<uint8_t>(
          (ReadBwram((sourceRow + bit / 8) & mask) >> (bit & 7)) & ((1u << depth) - 1));
    }
  }
  WritePlanarTile(pixels, RegisterWord(0x35));
}

uint8_t Sa1::ReadConverted(uint32_t offset) {
  const unsigned tileBytes = 8 * ConversionDepth();
  if (!(offset & (tileBytes - 1))) {
    const auto mask = static_cast<uint32_t>(bwram_.size()) - 1;
    const uint32_t tile = ((offset - RegisterAddress(0x32)) & mask) / tileBytes;
    ConvertBitmapTile(tile);
  }
  return iram_[(RegisterWord(0x35) + (offset & (tileBytes - 1))) & 0x7ff];
}

void Sa1::PushConversionRows() {
  // Each completed eight-pixel register row is visible immediately. The two
  // register halves alternate while the output walks eight planar rows.
  const unsigned registerBase = 0x40 + (conversionRows_ & 1) * 8;
  const unsigned tileBytes = 8 * ConversionDepth();
  const unsigned base = (RegisterWord(0x35) & 0x7ff) & ~(tileBytes * 2 - 1);
  for (unsigned plane = 0; plane < ConversionDepth(); ++plane) {
    uint8_t value = 0;
    for (unsigned pixel = 0; pixel < 8; ++pixel)
      value |= ((registers_[registerBase + pixel] >> plane) & 1) << (7 - pixel);
    const unsigned offset =
        conversionBuffer_ * tileBytes + (plane / 2) * 16 + conversionRows_ * 2 + (plane & 1);
    iram_[(base + offset) & 0x7ff] = value;
  }
  if (++conversionRows_ == 8) {
    conversionRows_ = 0;
    conversionBuffer_ ^= 1;
  }
}

} // namespace snes::core
