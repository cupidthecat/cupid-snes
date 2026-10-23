#include "snes/core/Cx4.hpp"
#include "snes/core/Cx4Processor.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/detail/ExecutionContext.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void Cx4::SerializeState(detail::StateArchive &archive) {
  archive.Expect(uint64_t(rom_.size()));
  archive.Array(ram_);
}
void Cx4Processor::SerializeState(detail::StateArchive &archive) {
  archive.Expect(uint64_t(rom_.size()));
  archive.Expect(uint64_t(save_.size()));
  archive.Expect(static_cast<uint32_t>(layout_));
  archive.Expect(masterFrequency_);
  archive.Expect(delayedSync_);
  archive.BeforeRestore([this]() noexcept { execution_.reset(); });
  Hg51b::SerializeState(archive);
  const auto balance = archive.Number(balance_);
  archive.Validate(balance <= int64_t(255) * Timing::kMasterClockHz - 1,
                   "Invalid Cx4 oscillator lead");
  archive.Number(irqLine_);
}
} // namespace snes::core
