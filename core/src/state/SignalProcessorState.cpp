#include "snes/core/CompetitionBoard.hpp"
#include "snes/core/NecDsp.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/UpdDsp.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void UpdDsp::SerializeState(detail::StateArchive &archive) {
  archive.Expect(programMask_);
  archive.Expect(romMask_);
  archive.Expect(ramMask_);
  // Public mutable views can change firmware storage, so inspect its current bytes.
  detail::StateArchive identity;
  identity.Array(program_);
  identity.Array(dataRom_);
  for (auto byte : detail::Sha256(identity.Bytes()))
    archive.Expect(byte);
  archive.Object(state_, [this](auto &fields, auto &state) {
    fields.Array(state.stack);
    fields.Array(state.accumulator);
    for (auto &flags : state.flags)
      fields.Bounded(flags, uint8_t{0}, uint8_t{63});
    fields.Bounded(state.pc, uint16_t{0}, programMask_);
    fields.Bounded(state.romPointer, uint16_t{0}, romMask_);
    fields.Bounded(state.ramPointer, uint16_t{0}, ramMask_);
    fields.Number(state.serialInput);
    fields.Number(state.serialOutput);
    fields.Number(state.temporary);
    fields.Number(state.temporaryBuffer);
    fields.Number(state.data);
    fields.Number(state.status);
    fields.Number(state.factorX);
    fields.Number(state.factorY);
    fields.Number(state.productHigh);
    fields.Number(state.productLow);
    fields.Bounded(state.stackPointer, uint8_t{0}, uint8_t{15});
    fields.Number(state.inputAcknowledge);
    fields.Number(state.outputAcknowledge);
  });
  archive.Array(ram_);
  archive.Number(cycles_);
}

void NecDsp::SerializeState(detail::StateArchive &archive) {
  archive.Expect(static_cast<uint32_t>(board_.Kind()));
  archive.Expect(frequency_);
  archive.Expect(masterFrequency_);
  processor_.SerializeState(archive);
  archive.Bounded(clockBalance_, int64_t{0}, int64_t(Timing::kMasterClockHz - 1));
}

void CompetitionBoard::SerializeState(detail::StateArchive &archive) {
  archive.Expect(static_cast<uint32_t>(settings_.board));
  archive.Expect(settings_.dipSwitches);
  archive.Expect(uint64_t(save_.size()));
  archive.Expect(masterFrequency_);
  for (unsigned index = 0; index < images_.size(); ++index) {
    archive.Expect(uint64_t(images_[index].size()));
    for (auto byte : imageDigests_[index])
      archive.Expect(byte);
  }
  archive.Expect(bool(processor_));
  if (processor_)
    processor_->SerializeState(archive);
  else
    commandDsp_->SerializeState(archive);
  archive.Fixed(std::span<uint8_t>(save_));
  archive.Bounded(timerBalance_, int64_t{0}, int64_t(Timing::kMasterClockHz - 1));
  archive.Bounded(dspBalance_, int64_t{0}, int64_t(Timing::kMasterClockHz - 1));
  archive.Bounded(gameSeconds_, 0u, 18u * 60);
  archive.Bounded(scoreSeconds_, 0u, 5u);
  archive.Number(status_);
  archive.Number(select_);
}
} // namespace snes::core
