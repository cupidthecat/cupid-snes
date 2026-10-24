#include "snes/core/Processor65816.hpp"
#include "snes/core/Sa1.hpp"
#include "snes/core/SnesCpu.hpp"
#include "snes/core/detail/ExecutionContext.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {

void Processor65816::SerializeState(detail::StateArchive &archive) {
  archive.Object(r, [](auto &fields, auto &next) {
    fields.Number(next.a);
    fields.Number(next.x);
    fields.Number(next.y);
    fields.Number(next.s);
    fields.Number(next.d);
    fields.Number(next.pc);
    fields.Number(next.db);
    fields.Number(next.pb);
    fields.Number(next.p);
    fields.Number(next.e);
    fields.Number(next.irq);
    fields.Number(next.wai);
    fields.Number(next.stp);
    fields.Number(next.vector);
    fields.Bounded(next.mar, uint32_t{0}, uint32_t{0xffffff});
    fields.Number(next.mdr);
    fields.Number(next.u);
    fields.Number(next.v);
    fields.Number(next.w);
  });
}

void SnesCpu::SerializeState(detail::StateArchive &archive) {
  archive.Validate(archive.Reading() || !stepSuspended_,
                   "Console CPU state requires an instruction boundary");
  archive.BeforeRestore([this]() noexcept {
    if (discardStep_)
      discardStep_();
    stepSuspended_ = false;
  });
  Processor65816::SerializeState(archive);
  archive.Number(cycles_);
  archive.Number(instructionClocks_);
  archive.Number(nmiLine_);
  archive.Number(nmiEdge_);
  archive.Number(nmiPending_);
  archive.Number(irqLine_);
  archive.Number(irqPending_);
  archive.Number(resetPending_);
  const auto refresh = archive.Number(dramRefreshState_);
  archive.Validate(refresh == 0, "Console CPU state retains an unfinished refresh operation");
}

void Sa1::SerializeState(detail::StateArchive &archive) {
  archive.Validate(archive.Reading() || (atStateBoundary_ && !synchronizingState_),
                   "SA-1 state requires an operation boundary");
  archive.BeforeRestore([this]() noexcept {
    execution_.reset();
    synchronizingState_ = false;
    atStateBoundary_ = true;
  });
  archive.Expect(uint32_t(bwram_.size()));
  archive.Expect(busContention_);
  archive.Expect(videoLines_);
  Processor65816::SerializeState(archive);
  archive.Array(iram_);
  archive.Fixed(bwram_);
  archive.Array(registers_);
  archive.Number(cpuFlags_);
  archive.Number(sa1Flags_);
  archive.Number(cpuIrqLevel_);
  archive.Number(interruptPending_);
  const auto requested = archive.Number(requestedClocks_);
  const auto executed = archive.Number(executedClocks_);
  archive.Validate(executed >= requested && executed - requested <= 16 * 1024 * 1024,
                   "Invalid SA-1 operation clock lead");
  archive.Bounded(cpuAddress_, uint32_t{0}, uint32_t{0xffffff});
  archive.Number(cpuRefreshing_);
  archive.Bounded(hClocks_, uint16_t{0}, uint16_t{2047});
  archive.Bounded(vCounter_, uint16_t{0}, uint16_t{511});
  archive.Bounded(latchedH_, uint16_t{0}, uint16_t{511});
  archive.Bounded(latchedV_, uint16_t{0}, uint16_t{511});
  archive.Bounded(arithmeticResult_, uint64_t{0}, AccumulatorMask);
  archive.Number(arithmeticOverflow_);
  archive.Bounded(bitOffset_, uint8_t{0}, uint8_t{7});
  archive.Number(dmaActive_);
  archive.Number(conversionActive_);
  archive.Bounded(conversionRows_, uint8_t{0}, uint8_t{7});
  archive.Bounded(conversionBuffer_, uint8_t{0}, uint8_t{1});
}
} // namespace snes::core
