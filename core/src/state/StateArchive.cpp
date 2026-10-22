#include "snes/core/detail/StateArchive.hpp"

namespace snes::core::detail {
StateArchive::StateArchive() = default;
StateArchive::StateArchive(std::span<const uint8_t> bytes)
    : mode_(Mode::Stage), input_(bytes), plan_(std::make_shared<RestorePlan>()) {
  Require(bytes.size() <= MaximumBytes, "Machine state exceeds the supported size");
}
StateArchive::StateArchive(Mode mode, std::span<const uint8_t> bytes,
                           std::shared_ptr<RestorePlan> plan)
    : mode_(mode), root_(false), input_(bytes), plan_(std::move(plan)) {}

void StateArchive::Require(bool condition, const char *message) {
  if (!condition) {
    Invalidate();
    throw std::runtime_error(message);
  }
}
void StateArchive::Invalidate() noexcept {
  if (plan_)
    plan_->failed = true;
}
void StateArchive::WriteUnsigned(uint64_t value, size_t bytes) {
  Require(bytes <= MaximumBytes - output_.size(), "Machine state exceeds the supported size");
  for (size_t byte = 0; byte < bytes; ++byte)
    output_.push_back(uint8_t(value >> (8 * byte)));
}
uint64_t StateArchive::ReadUnsigned(size_t bytes) {
  Require(!plan_->committed, "Machine state has already been restored");
  Require(!plan_->failed, "Machine state validation has failed");
  Require(bytes <= Remaining(), "Truncated machine state value");
  uint64_t value = 0;
  for (size_t byte = 0; byte < bytes; ++byte)
    value |= uint64_t(input_[position_++]) << (8 * byte);
  return value;
}
void StateArchive::PatchLength(size_t position, size_t length) {
  Require(length <= std::numeric_limits<uint32_t>::max(), "Machine state section is too large");
  for (size_t byte = 0; byte < 4; ++byte)
    output_[position + byte] = uint8_t(length >> (8 * byte));
}
void StateArchive::WriteCount(size_t count, size_t maximumElements) {
  Require(count <= maximumElements && count <= std::numeric_limits<uint32_t>::max(),
          "Machine state collection exceeds its limit");
  WriteUnsigned(count, 4);
}
size_t StateArchive::ReadCount(size_t maximumElements, size_t minimumBytes) {
  const auto count = ReadUnsigned(4);
  Require(count <= maximumElements && count <= Remaining() / minimumBytes,
          "Invalid machine state collection size");
  return size_t(count);
}
void StateArchive::ReserveStaging(size_t count, size_t bytes) {
  Require(bytes != 0 && count <= (MaximumBytes - plan_->bytes) / bytes,
          "Machine state restore exceeds its memory limit");
  plan_->bytes += count * bytes;
}
void StateArchive::Commit() {
  Require(root_ && mode_ == Mode::Stage && !plan_->committed && !plan_->failed,
          "Machine state restore cannot be committed here");
  Require(Remaining() == 0, "Unexpected data at the end of machine state");
  for (auto &prepare : plan_->preparations)
    prepare();
  for (auto &change : plan_->changes)
    change();
  plan_->committed = true;
  plan_->changes.clear();
  plan_->preparations.clear();
}
} // namespace snes::core::detail
