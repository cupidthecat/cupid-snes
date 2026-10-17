// DSP-2 bitmap and arithmetic command processing.

#include "snes/core/Dsp2.hpp"

#include <algorithm>

namespace snes::core {

void Dsp2::Reset() {
  commandReady_ = true;
  received_ = needed_ = read_ = available_ = 0;
  lengths_ = {};
  payloads_ = {};
  scaledLength_ = 0;
  transparent_ = 0;
}

uint8_t Dsp2::Read() {
  if (!available_)
    return 0xff;
  const auto result = output_[read_];
  read_ = (read_ + 1) & 511;
  if (read_ == available_)
    available_ = 0;
  return result;
}

void Dsp2::Write(uint8_t value) {
  if (commandReady_) {
    command_ = value;
    received_ = 0;
    commandReady_ = false;
    switch (command_) {
    case 1:
      needed_ = 32;
      break;
    case 3:
    case 5:
    case 6:
      needed_ = 1;
      break;
    case 9:
      needed_ = 4;
      break;
    case 0x0d:
      needed_ = 2;
      break;
    case 0x0f:
      needed_ = 0;
      break;
    default:
      break;
    }
  } else {
    input_[received_] = value;
    received_ = (received_ + 1) & 511;
  }
  if (received_ == needed_)
    Complete(value);
}

void Dsp2::Complete(uint8_t value) {
  commandReady_ = true;
  read_ = 0;
  if (command_ == 5 || command_ == 6 || command_ == 0x0d) {
    const unsigned operation = command_ == 5 ? 0 : command_ == 6 ? 1 : 2;
    if (!payloads_[operation]) {
      lengths_[operation] = input_[0];
      if (operation == 2)
        scaledLength_ = input_[1];
      needed_ = operation == 0   ? lengths_[0] * 2
                : operation == 1 ? lengths_[1]
                                 : (lengths_[2] + 1) / 2;
      received_ = 0;
      payloads_[operation] = true;
      if (value)
        commandReady_ = false;
      return;
    }
    payloads_[operation] = false;
  }
  Execute();
}

void Dsp2::Execute() {
  const size_t length = lengths_[command_ == 5 ? 0 : command_ == 6 ? 1 : 2];
  switch (command_) {
  case 0x01:
    // Eight packed pixel rows become four SNES bitplanes.
    std::fill_n(output_.begin(), 32, 0);
    for (size_t row = 0; row < 8; ++row) {
      for (size_t x = 0; x < 8; ++x) {
        const unsigned pixel = (input_[row * 4 + x / 2] >> ((x & 1) ? 0 : 4)) & 15;
        for (size_t plane = 0; plane < 4; ++plane) {
          const size_t offset = (plane / 2) * 16 + row * 2 + plane % 2;
          output_[offset] |= static_cast<uint8_t>(((pixel >> plane) & 1) << (7 - x));
        }
      }
    }
    available_ = 32;
    break;
  case 0x03:
    transparent_ = input_[0];
    break;
  case 0x05:
    for (size_t i = 0; i < length; ++i) {
      unsigned result = 0;
      for (unsigned shift : {0u, 4u}) {
        const unsigned overlay = (input_[length + i] >> shift) & 15;
        const unsigned pixel = overlay == (transparent_ & 15) ? (input_[i] >> shift) & 15 : overlay;
        result |= pixel << shift;
      }
      output_[i] = static_cast<uint8_t>(result);
    }
    available_ = length;
    break;
  case 0x06:
    for (size_t i = 0; i < length; ++i) {
      const uint8_t pixels = input_[length - 1 - i];
      output_[i] = static_cast<uint8_t>((pixels >> 4) | (pixels << 4));
    }
    available_ = length;
    break;
  case 0x09: {
    const uint32_t a = input_[0] | (uint32_t(input_[1]) << 8);
    const uint32_t b = input_[2] | (uint32_t(input_[3]) << 8);
    for (unsigned i = 0; i < 4; ++i)
      output_[i] = static_cast<uint8_t>((a * b) >> (i * 8));
    available_ = 4;
    break;
  }
  case 0x0d: {
    const uint32_t step = length <= scaledLength_
                              ? 0x10000
                              : static_cast<uint32_t>((length << 17) / (scaledLength_ * 2 + 1));
    for (size_t i = 0; i < scaledLength_; ++i) {
      unsigned packed = 0;
      for (unsigned half = 0; half < 2; ++half) {
        const size_t pixel = ((i * 2 + half) * step) >> 16;
        packed = (packed << 4) | ((input_[pixel / 2] >> ((pixel & 1) ? 0 : 4)) & 15);
      }
      output_[i] = static_cast<uint8_t>(packed);
    }
    available_ = scaledLength_;
    break;
  }
  default:
    break;
  }
}

} // namespace snes::core
