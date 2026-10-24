#pragma once

#include "snes/core/StateOperation.hpp"
#include "snes/core/detail/Sha256.hpp"
#include <algorithm>

namespace snes::core::detail {
inline auto StateDigest(std::span<const uint8_t> bytes, const StateOperation &operation,
                        StatePhase phase, std::span<const uint8_t> prefix = {}) {
  Sha256Hasher hash;
  hash.Update(prefix);
  if (operation) {
    for (size_t offset = 0; offset < bytes.size(); offset += 65536) {
      operation.Report(phase, offset, bytes.size());
      hash.Update(bytes.subspan(offset, std::min<size_t>(65536, bytes.size() - offset)));
    }
    operation.Report(phase, bytes.size(), bytes.size());
  } else
    hash.Update(bytes);
  return hash.Finish();
}
} // namespace snes::core::detail
