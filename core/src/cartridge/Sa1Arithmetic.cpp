// snes emulator
// SA-1 timer, arithmetic unit, and variable-width bit stream.
#include "snes/core/Sa1.hpp"

namespace snes::core {

void Sa1::AdvanceTimer(uint32_t masterClocks) {
  const auto control = registers_[0x10];
  hClocks_ += static_cast<uint16_t>(masterClocks);
  if (control & 0x80) {
    vCounter_ = static_cast<uint16_t>((vCounter_ + (hClocks_ >> 11)) & 0x1ff);
    hClocks_ &= 0x7ff;
  } else if (hClocks_ >= 1364) {
    hClocks_ = 0;
    if (++vCounter_ >= videoLines_)
      vCounter_ = 0;
  }
  if (!(control & 3))
    return;
  const bool horizontal = hClocks_ == uint32_t(RegisterWord(0x12)) * 4;
  const bool vertical = vCounter_ == RegisterWord(0x14);
  const bool match = (control & 3) == 1   ? horizontal
                     : (control & 3) == 2 ? vertical && !hClocks_
                                          : (control & 3) == 3 && horizontal && vertical;
  if (match)
    sa1Flags_ |= 0x40;
}

void Sa1::StartArithmetic() {
    const uint16_t bitsA = RegisterWord(0x51);
    const uint16_t bitsB = RegisterWord(0x53);
    const int64_t a = bitsA < 0x8000 ? int64_t(bitsA) : int64_t(bitsA) - 0x10000;
    const int64_t b = bitsB < 0x8000 ? int64_t(bitsB) : int64_t(bitsB) - 0x10000;
    const uint8_t mode = registers_[0x50] & 3;
    if (mode & 2) {
      const uint64_t result = arithmeticResult_ + static_cast<uint64_t>(a * b);
      arithmeticOverflow_ = result > AccumulatorMask;
      arithmeticResult_ = result & AccumulatorMask;
    } else if (mode == 1) {
      arithmeticResult_ = 0;
      if (bitsB) {
        int64_t quotient = a / bitsB;
        int64_t remainder = a % bitsB;
        if (remainder < 0) {
          --quotient;
          remainder += bitsB;
        }
        const uint32_t packed = uint16_t(quotient) | (uint32_t(remainder) << 16);
        arithmeticResult_ = packed;
        if (packed & 0x80000000)
          arithmeticResult_ |= uint64_t{0xff} << 32;
      }
        registers_[0x51] = registers_[0x52] = 0;
    } else {
      arithmeticResult_ = static_cast<uint32_t>(a * b);
    }
    registers_[0x53] = registers_[0x54] = 0;
}

void Sa1::RefreshBitResult() {
    uint32_t bits = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
        uint32_t address = (bitAddress_ + byte) & 0xffffff;
        const uint8_t bank = static_cast<uint8_t>(address >> 16);
        if (bank < 0xc0 && ((bank & 0x40) || !(address & 0x8000)))
            address = 0x008000 | (address & 0x7fff);
        uint32_t offset = 0;
        const uint8_t value = RomOffset(address, offset) ? ReadRom(offset, regs().mdr) : regs().mdr;
        bits |= uint32_t(value) << (byte * 8);
    }
    bitResult_ = static_cast<uint16_t>(bits >> bitOffset_);
}

void Sa1::AdvanceBits() {
    const unsigned length = (registers_[0x58] & 15) ? (registers_[0x58] & 15) : 16;
    const unsigned position = bitOffset_ + length;
    bitAddress_ = (bitAddress_ + (position / 16) * 2) & 0xffffff;
    bitOffset_ = static_cast<uint8_t>(position & 15);
    StoreAddress(0x59, bitAddress_);
    RefreshBitResult();
}

} // namespace snes::core
