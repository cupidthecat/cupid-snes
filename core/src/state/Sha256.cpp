#include "snes/core/detail/Sha256.hpp"

#include <algorithm>
#include <bit>

namespace snes::core::detail {
std::array<uint8_t, 32> Sha256(std::span<const uint8_t> bytes) {
  constexpr std::array<uint32_t, 64> constants{
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  std::array<uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const auto block = [&](std::span<const uint8_t, 64> input) {
    std::array<uint32_t, 64> words{};
    for (unsigned index = 0; index < 16; ++index)
      for (unsigned byte = 0; byte < 4; ++byte)
        words[index] = (words[index] << 8) | input[index * 4 + byte];
    for (unsigned index = 16; index < words.size(); ++index) {
      const auto x = words[index - 15], y = words[index - 2];
      const auto first = std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
      const auto second = std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10);
      words[index] = words[index - 16] + first + words[index - 7] + second;
    }
    auto [a, b, c, d, e, f, g, h] = state;
    for (unsigned index = 0; index < words.size(); ++index) {
      const auto sumE = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choice = (e & f) ^ (~e & g);
      const auto first = h + sumE + choice + constants[index] + words[index];
      const auto sumA = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      h = g;
      g = f;
      f = e;
      e = d + first;
      d = c;
      c = b;
      b = a;
      a = first + sumA + majority;
    }
    const std::array<uint32_t, 8> result{a, b, c, d, e, f, g, h};
    for (unsigned index = 0; index < state.size(); ++index)
      state[index] += result[index];
  };
  const auto bitLength = uint64_t(bytes.size()) * 8;
  while (bytes.size() >= 64) {
    block(bytes.first<64>());
    bytes = bytes.subspan(64);
  }
  std::array<uint8_t, 64> tail{};
  std::copy(bytes.begin(), bytes.end(), tail.begin());
  tail[bytes.size()] = 0x80;
  if (bytes.size() >= 56) {
    block(tail);
    tail.fill(0);
  }
  for (unsigned byte = 0; byte < 8; ++byte)
    tail[63 - byte] = uint8_t(bitLength >> (byte * 8));
  block(tail);
  std::array<uint8_t, 32> digest{};
  for (unsigned index = 0; index < state.size(); ++index)
    for (unsigned byte = 0; byte < 4; ++byte)
      digest[index * 4 + byte] = uint8_t(state[index] >> (24 - byte * 8));
  return digest;
}
} // namespace snes::core::detail
