#include "snes/core/Spc7110Decoder.hpp"
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace snes::core {
namespace {
// Packed probability, MPS successor, and LPS successor for each coding state.
constexpr uint32_t Models[]{
    0x01015a, 0x060225, 0x080311, 0x0a0408, 0x0c0503, 0x0f0501, 0x07075a, 0x13083f, 0x15092c,
    0x160a20, 0x170b17, 0x190c11, 0x1a0d0c, 0x1c0e09, 0x1d0f07, 0x1f1005, 0x201104, 0x221203,
    0x230502, 0x14145a, 0x271548, 0x28163a, 0x2a172e, 0x2c1826, 0x2d191f, 0x2e1a19, 0x191b15,
    0x1a1c11, 0x1a1d0e, 0x1b1e0b, 0x1c1f09, 0x1d2008, 0x1e2107, 0x1f2205, 0x212304, 0x212404,
    0x222503, 0x232602, 0x240502, 0x272858, 0x2f294d, 0x302a43, 0x312b3b, 0x322c34, 0x332d2e,
    0x2c2e29, 0x2d1825, 0x2f3056, 0x2f314f, 0x303247, 0x313341, 0x32343c, 0x332b37};
} // namespace
Spc7110Decoder::Spc7110Decoder(Reader reader) : reader_(std::move(reader)) {}
void Spc7110Decoder::Initialize(unsigned mode, uint32_t origin) {
  if (mode > 2)
    throw std::invalid_argument("Invalid SPC7110 compression mode");
  contexts_ = {};
  std::iota(colors_.begin(), colors_.end(), uint8_t(0));
  depth_ = uint8_t(1u << mode);
  address_ = origin;
  interval_ = 256;
  code_ = uint16_t(uint16_t(reader_(address_++)) << 8);
  code_ |= reader_(address_++);
  remaining_ = 8;
  history_ = 0;
  decisions_ = 0;
}
void Spc7110Decoder::Prioritize(std::array<uint8_t, 16> &colors, uint8_t value) {
  auto found = std::find(colors.begin(), colors.end(), value);
  std::rotate(colors.begin(), found, found + 1);
}
bool Spc7110Decoder::Symbol(unsigned index) {
  auto &context = contexts_[index];
  const uint32_t model = Models[context.model];
  const uint8_t probability = uint8_t(model);
  const uint8_t split = uint8_t(interval_ - probability);
  const bool lessLikely = code_ >= unsigned(split) * 256;
  const bool value = lessLikely ^ context.inverted;
  if (lessLikely) {
    interval_ = uint16_t(interval_ - split);
    code_ = uint16_t(code_ - unsigned(split) * 256);
  } else
    interval_ = split;
  if (interval_ < 128) {
    context.model = uint8_t(model >> (lessLikely ? 16 : 8));
    do {
      interval_ *= 2;
      code_ = uint16_t(code_ << 1);
      if (--remaining_ == 0) {
        remaining_ = 8;
        code_ = uint16_t(code_ + reader_(address_++));
      }
    } while (interval_ < 128);
  }
  if (lessLikely && probability > 0x55)
    context.inverted = !context.inverted;
  return value;
}
uint32_t Spc7110Decoder::NextRow() {
  uint32_t row = 0;
  for (unsigned x = 0; x < 8; ++x) {
    auto order = colors_;
    unsigned category = 0;
    if (depth_ != 1) {
      const uint8_t left = uint8_t(history_ >> (depth_ == 2 ? 2 : 0)) & (depth_ == 2 ? 3 : 15);
      const uint8_t upper = uint8_t(history_ >> (depth_ == 2 ? 14 : 28)) & (depth_ == 2 ? 3 : 15);
      const uint8_t corner = uint8_t(history_ >> (depth_ == 2 ? 16 : 32)) & (depth_ == 2 ? 3 : 15);
      if (left != upper || upper != corner)
        category = left == upper ? 3 : left == corner ? 2 : upper == corner ? 1 : 4;
      Prioritize(colors_, left);
      Prioritize(order, corner);
      Prioritize(order, upper);
      Prioritize(order, left);
    }
    for (unsigned plane = 0; plane < depth_; ++plane) {
      const unsigned branch = 1u << (depth_ == 1 ? x & 3 : plane);
      const unsigned prior = decisions_ & (branch - 1);
      const unsigned set = depth_ == 1                                 ? unsigned(x >= 4)
                           : depth_ == 2 || (plane >= 2 && prior <= 1) ? category
                                                                       : 0;
      decisions_ = uint8_t((decisions_ << 1) | Symbol(set * 15 + branch + prior - 1));
    }
    unsigned rank = decisions_ & ((1u << depth_) - 1);
    if (depth_ == 1)
      rank ^= unsigned(history_ >> 15) & 1;
    const uint8_t color = order[rank];
    history_ = (history_ << depth_) | color;
    for (unsigned plane = 0; plane < depth_; ++plane)
      row |= ((uint32_t(color) >> (depth_ - 1 - plane)) & 1) << (plane * 8 + 7 - x);
  }
  return depth_ == 1 ? uint32_t(history_) : row;
}
} // namespace snes::core
