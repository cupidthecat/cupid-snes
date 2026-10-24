#include "snes/core/detail/Sha256.hpp"
#include "state/HashVectors.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace snes::core::detail;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::string Hex(const std::array<uint8_t, 32> &digest) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text;
  for (auto byte : digest) {
    text.push_back(digits[byte >> 4]);
    text.push_back(digits[byte & 15]);
  }
  return text;
}
void ChunkBoundaries() {
  for (const auto &test : snes::tests::StreamHashVectors) {
    std::vector<uint8_t> bytes(test.size);
    for (size_t n = 0; n < bytes.size(); ++n)
      bytes[n] = uint8_t(n * 17 + n / 7 + 0x39);
    Check(Hex(Sha256(bytes)) == test.digest, "Contiguous hash agrees with the fixed answer");
    for (size_t chunk : {1u, 3u, 7u, 55u, 56u, 63u, 64u, 65u, 127u, 4096u, 65536u}) {
      Sha256Hasher hash;
      for (size_t offset = 0; offset < bytes.size(); offset += chunk) {
        hash.Update({});
        hash.Update(std::span(bytes).subspan(offset, std::min(chunk, bytes.size() - offset)));
      }
      Check(Hex(hash.Finish()) == test.digest && Hex(hash.Finish()) == test.digest,
            "Arbitrary stream chunks and repeated finalization retain exact padding");
    }
    if (bytes.size() <= 129)
      for (size_t cut = 0; cut <= bytes.size(); ++cut) {
        Sha256Hasher hash;
        hash.Update(std::span(bytes).first(cut));
        (void)hash.Finish();
        hash.Update(std::span(bytes).subspan(cut));
        Check(Hex(hash.Finish()) == test.digest, "Finalizing a prefix does not consume its stream");
      }
  }
  std::vector<uint8_t> million(1000000, 'a');
  Sha256Hasher hash;
  for (size_t offset = 0; offset < million.size(); offset += 997)
    hash.Update(std::span(million).subspan(offset, std::min(size_t(997), million.size() - offset)));
  Check(Hex(hash.Finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "Million-byte stream retains the standard digest");
}
} // namespace
int main() {
  try {
    ChunkBoundaries();
    std::puts("Incremental SHA-256 vectors and chunk boundaries passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
