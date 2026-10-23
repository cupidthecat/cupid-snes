#include "snes/core/Arm6.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
namespace {
void Registers(detail::StateArchive &archive, Arm6::State &state) {
  archive.Array(state.common);
  archive.Array(state.fiq);
  for (auto &bank : state.bank)
    archive.Array(bank);
  archive.Array(state.saved);
  archive.Number(state.status);
}
} // namespace

void Arm6::RestoreExecutionState(const ExecutionState &state) noexcept {
  state_ = state.registers;
  fetched_ = state.fetched;
  decoded_ = state.decoded;
  decodedAddress_ = state.decodedAddress;
  reload_ = state.reload;
  sequential_ = state.sequential;
  carry_ = state.carry;
}
void Arm6::SerializeExecutionState(detail::StateArchive &archive, ExecutionState &state) {
  archive.Object(state, [](auto &fields, auto &next) {
    Registers(fields, next.registers);
    fields.Number(next.fetched);
    fields.Number(next.decoded);
    fields.Number(next.decodedAddress);
    fields.Number(next.reload);
    fields.Number(next.sequential);
    fields.Number(next.carry);
  });
}
Arm6::ExecutionState Arm6::SerializeArmState(detail::StateArchive &archive) {
  ExecutionState decoded;
  archive.Object(state_, [&](auto &fields, auto &next) {
    Registers(fields, next);
    decoded.registers = next;
  });
  decoded.fetched = archive.Number(fetched_);
  decoded.decoded = archive.Number(decoded_);
  decoded.decodedAddress = archive.Number(decodedAddress_);
  decoded.reload = archive.Number(reload_);
  decoded.sequential = archive.Number(sequential_);
  decoded.carry = archive.Number(carry_);
  return decoded;
}
void Arm6::SerializeState(detail::StateArchive &archive) {
  SerializeArmState(archive);
}
} // namespace snes::core
