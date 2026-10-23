#pragma once

#include "snes/core/Msu1.hpp"
#include "snes/core/StateOperation.hpp"

#include <array>
#include <cstdio>
#include <optional>

namespace snes::core {
struct Msu1::Stream::Resource {
  Resource() = default;
  Resource(const Resource &) = delete;
  Resource &operator=(const Resource &) = delete;
  ~Resource();
  static std::shared_ptr<Resource> Open(const std::filesystem::path &path);
  struct Key {
    uint64_t size = 0;
    std::array<uint8_t, 32> digest{};
  };
  const Key &Fingerprint(const StateOperation &operation = {});
  size_t Read(uint64_t offset, std::span<uint8_t> bytes);
  uint64_t Size() const noexcept {
    return size;
  }

  std::FILE *file = nullptr;
  std::filesystem::path path;
  uint64_t size = 0;
  std::optional<std::array<uint64_t, 8>> stamp;
  std::optional<Key> identity;
};
} // namespace snes::core
