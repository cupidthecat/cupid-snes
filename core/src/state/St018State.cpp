#include "snes/core/St018Processor.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/detail/ExecutionContext.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void St018Processor::ValidateInstruction(detail::StateArchive &archive,
                                         const Instruction &instruction,
                                         const ExecutionState &current) const {
  struct Pending {};
  struct Probe final : Arm6 {
    Probe(const Instruction &instruction) : instruction(instruction) {
      RestoreExecutionState(instruction.start);
    }
    const Instruction &instruction;
    unsigned index = 0;
    const AccessRecord &Next(AccessKind kind, unsigned access, uint32_t address, uint32_t data) {
      if (index >= instruction.count)
        throw std::runtime_error("Missing ARM continuation access");
      const auto &record = instruction.accesses[index++];
      if (record.kind != kind || record.access != access || record.address != address ||
          record.data != data)
        throw std::runtime_error("Invalid ARM continuation access");
      if (index == instruction.count)
        throw Pending{};
      return record;
    }
    uint32_t ReadArm(unsigned access, uint32_t address) override {
      return Next(AccessKind::Read, access, address, 0).result;
    }
    void WriteArm(unsigned access, uint32_t address, uint32_t value) override {
      Next(AccessKind::Write, access, address, value);
    }
    void IdleArm() override {
      Next(AccessKind::Idle, 0, 0, 0);
    }
    ExecutionState Current() const noexcept {
      return CaptureExecutionState();
    }
  } probe(instruction);
  bool pending = false;
  try {
    probe.RunInstruction();
  } catch (const Pending &) {
    pending = true;
  }
  archive.Validate(pending && probe.Current() == current,
                   "ARM continuation does not match its pipeline state");
}

void St018Processor::SerializeState(detail::StateArchive &archive) {
  archive.Expect(masterFrequency_);
  for (auto byte : firmwareDigest_)
    archive.Expect(byte);
  const auto current = SerializeArmState(archive);
  const auto phase = archive.Enumeration(phase_, Phase::Begin, Phase::Instruction);
  archive.Object(instruction_, [&](auto &fields, auto &next) {
    SerializeExecutionState(fields, next.start);
    fields.Bounded(next.count, 0u, MaximumAccesses);
    fields.Validate((phase == Phase::Instruction) == (next.count != 0),
                    "Invalid ARM continuation phase");
    for (unsigned index = 0; index < next.count; ++index) {
      auto &record = next.accesses[index];
      fields.Enumeration(record.kind, AccessKind::Idle, AccessKind::Write);
      fields.Number(record.access);
      fields.Number(record.address);
      fields.Number(record.data);
      fields.Number(record.result);
      fields.Validate((record.kind == AccessKind::Read && index + 1 < next.count) ||
                          record.result == 0,
                      "Invalid ARM pending access result");
    }
    if (next.count)
      ValidateInstruction(fields, next, current);
  });
  archive.Array(ram_);
  constexpr auto maximumDebt = int64_t(4096) * 21'440'000;
  const auto maximumLead =
      int64_t(phase == Phase::StartupClock ? 65536 : 1) * Timing::kMasterClockHz - 1;
  archive.Bounded(balance_, -maximumDebt, maximumLead);
  archive.Bounded(timer_, 0u, 0xffffffu);
  archive.Bounded(timerLatch_, 0u, 0xffffffu);
  archive.Number(input_);
  archive.Number(output_);
  archive.Number(inputReady_);
  archive.Number(outputReady_);
  const auto ready = archive.Number(ready_);
  archive.Number(signal_);
  const auto reset = archive.Number(reset_);
  archive.Validate(ready == (phase == Phase::Instruction) &&
                       (!reset || phase == Phase::Begin || phase == Phase::ResetClock),
                   "Invalid ARM startup handshake");
  archive.BeforeRestore([this]() noexcept {
    execution_.reset();
    replaying_ = false;
    recording_ = true;
    replayIndex_ = 0;
  });
}
} // namespace snes::core
