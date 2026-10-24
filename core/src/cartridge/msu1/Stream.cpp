#include "Resource.hpp"

#include <algorithm>

namespace snes::core {
bool Msu1::Stream::Open(const std::filesystem::path &path) {
  Close();
  resource_ = Resource::Open(path);
  if (!resource_)
    return false;
  state_.size = resource_->Size();
  state_.opened = true;
  return true;
}
void Msu1::Stream::Close() {
  resource_.reset();
  state_.identity.fill(0);
  state_.size = state_.mediaSize = state_.cacheOffset = state_.cacheSize = 0;
  state_.opened = false;
}
bool Msu1::Stream::Read(uint64_t offset, std::span<uint8_t> destination) {
  if (!IsOpen() || offset > state_.size || destination.size() > state_.size - offset ||
      destination.size() > state_.cache.size())
    return false;
  if (destination.empty())
    return true;
  const bool cached = offset >= state_.cacheOffset &&
                      offset - state_.cacheOffset <= state_.cacheSize &&
                      destination.size() <= state_.cacheSize - size_t(offset - state_.cacheOffset);
  if (!cached) {
    state_.cacheSize = 0;
    state_.cacheOffset = offset;
    const auto count = size_t(std::min<uint64_t>(state_.cache.size(), state_.size - offset));
    state_.cacheSize = uint32_t(resource_->Read(offset, std::span(state_.cache).first(count)));
    if (state_.cacheSize < destination.size())
      return false;
  }
  std::copy_n(state_.cache.data() + size_t(offset - state_.cacheOffset), destination.size(),
              destination.data());
  return true;
}
} // namespace snes::core
