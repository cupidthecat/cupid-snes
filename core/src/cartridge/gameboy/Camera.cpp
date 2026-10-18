#include "snes/core/gameboy/Camera.hpp"

#include <algorithm>
#include <bit>
#include <ctime>
#include <utility>

namespace snes::core::gameboy {
namespace {
struct Ratio {
  unsigned numerator, denominator;
};
// Preserve byte-level gain quantization before applying exposure.
constexpr std::array<Ratio, 32> Gain = {
    {{207, 235}, {43, 47},   {122, 129}, {187, 192}, {1, 1},     {85, 83},   {202, 193}, {268, 251},
     {236, 217}, {281, 250}, {59, 51},   {197, 166}, {295, 243}, {119, 96},  {144, 113}, {321, 244},
     {188, 139}, {309, 223}, {269, 190}, {153, 106}, {260, 177}, {203, 136}, {256, 169}, {367, 239},
     {381, 245}, {107, 68},  {35, 22},   {209, 130}, {138, 85},  {331, 202}, {81, 49},   {5, 3}}};
constexpr std::array<unsigned, 8> Edge = {2, 3, 4, 5, 8, 12, 16, 20};
constexpr unsigned Width = 128, Height = 112;
} // namespace

Camera::Camera() : random_(uint64_t(std::time(nullptr))) {
  random_.discard(64);
}

void Camera::Reset() noexcept {
  registers_.fill(0);
  remaining_ = 0;
  alignment_ = 0;
  dirty_ = true;
}

void Camera::SetEntropy(uint64_t seed, bool enabled) noexcept {
  random_.seed(seed);
  randomEnabled_ = enabled;
}

void Camera::SetNoiseSource(NoiseSource source) {
  noiseSource_ = std::move(source);
}

bool Camera::SetFrame(std::span<const uint8_t> pixels) {
  if (!pixels.empty() && pixels.size() != Width * Height)
    return false;
  if (pixels.empty())
    frame_.clear();
  else
    frame_.assign(pixels.begin(), pixels.end());
  dirty_ = true;
  return true;
}

uint8_t Camera::Read(uint16_t address) const noexcept {
  return (address & 127) == 0 ? registers_[0] : 0;
}

void Camera::Write(uint16_t address, uint8_t value) {
  const unsigned index = address & 127;
  if (index >= registers_.size())
    return;
  if (index == 0) {
    noise_ = !randomEnabled_ ? 0 : noiseSource_ ? noiseSource_() : uint8_t(random_() >> 56);
    value &= 7;
    if ((value & 1) && !Busy()) {
      const unsigned exposure = (unsigned(registers_[2]) << 8) | registers_[3];
      remaining_ = 129792 + ((registers_[1] & 128) ? 0 : 2048) + exposure * 64 + (alignment_ & 4);
    }
    // A cancellation request changes the control flags but leaves capture active.
    if (Busy())
      value |= 1;
  }
  registers_[index] = value;
  if (index <= 4)
    dirty_ = true;
}

void Camera::Advance(unsigned clocks, bool sleeping) noexcept {
  if (sleeping)
    return;
  const uint64_t elapsed = clocks;
  alignment_ = uint8_t(alignment_ + elapsed);
  if (remaining_) {
    remaining_ -= unsigned(std::min(uint64_t(remaining_), elapsed));
    if (remaining_ == 0)
      registers_[0] &= 0xfe;
  }
}

uint8_t Camera::Noise(unsigned x, unsigned y) const noexcept {
  unsigned bits = (x * 151 + y * 149) ^ noise_;
  if (bits == 0)
    return 0;
  bits >>= std::countr_zero(bits);
  uint8_t result = 0;
  while (bits) {
    result ^= std::rotl(uint8_t(161), int(std::countr_zero(bits)));
    bits &= bits - 1;
  }
  return result;
}

void Camera::PrepareColors() {
  if (!dirty_)
    return;
  colors_.resize(Width * Height);
  const auto gain = Gain[registers_[1] & 31];
  const unsigned exposure = (unsigned(registers_[2]) << 8) | registers_[3];
  for (unsigned y = 0; y < Height; ++y)
    for (unsigned x = 0; x < Width; ++x) {
      const unsigned offset = y * Width + x;
      const unsigned pixel = frame_.empty() ? Noise(x, y) : frame_[offset];
      const unsigned amplified = pixel * gain.numerator / gain.denominator;
      colors_[offset] = uint16_t(amplified * exposure / 4096);
    }
  dirty_ = false;
}

int Camera::Color(unsigned x, unsigned y) const noexcept {
  int value = colors_[y * Width + x];
  if ((registers_[1] & 0xe0) != 0xe0)
    return value;
  const int edge = int(Edge[(registers_[4] >> 4) & 7]);
  value *= 1 + edge;
  // Each neighbor subtraction retains its own signed truncation.
  for (unsigned offset : {y * Width + (x ? x - 1 : 0), y * Width + std::min(x + 1, Width - 1),
                          (y ? y - 1 : 0) * Width + x, std::min(y + 1, Height - 1) * Width + x})
    value = (4 * value - edge * int(colors_[offset])) / 4;
  return value;
}

uint8_t Camera::ReadImage(uint16_t offset) {
  if (Busy())
    return 0;
  if (offset >= 3584)
    return 0xff;
  PrepareColors();
  const unsigned tile = offset / 16;
  const unsigned y = (tile / 16) * 8 + (offset / 2) % 8;
  const unsigned firstX = (tile % 16) * 8;
  uint8_t result = 0;
  for (unsigned i = 0; i < 8; ++i) {
    const unsigned x = firstX + i;
    const int value = Color(x, y);
    const unsigned pattern = 6 + ((y & 3) * 4 + (x & 3)) * 3;
    const unsigned shade = value < registers_[pattern]       ? 3
                           : value < registers_[pattern + 1] ? 2
                           : value < registers_[pattern + 2] ? 1
                                                             : 0;
    result |= uint8_t(((shade >> (offset & 1)) & 1) << (7 - i));
  }
  return result;
}

} // namespace snes::core::gameboy
