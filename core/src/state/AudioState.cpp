#include "snes/core/Smp.hpp"
#include "snes/core/Spc700.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <limits>

namespace snes::core {

void Spc700::SerializeState(detail::StateArchive &archive) {
  archive.Validate(archive.Reading() || !InstructionInProgress(),
                   "SPC700 state requires an instruction boundary");
  archive.BeforeRestore([this]() noexcept { DiscardExecution(); });
  archive.Object(r, [](auto &fields, auto &next) {
    fields.Number(next.pc);
    fields.Number(next.a);
    fields.Number(next.y);
    fields.Number(next.x);
    fields.Number(next.s);
    fields.Number(next.p.c);
    fields.Number(next.p.z);
    fields.Number(next.p.i);
    fields.Number(next.p.h);
    fields.Number(next.p.b);
    fields.Number(next.p.p);
    fields.Number(next.p.v);
    fields.Number(next.p.n);
    fields.Number(next.wait);
    fields.Number(next.stop);
  });
  archive.Number(cycles_);
}

void Smp::SerializeState(detail::StateArchive &archive) {
  archive.Validate(archive.Reading() || (!synchronizingCpu_ && !busWait_.duration),
                   "SMP state requires a completed bus operation");
  archive.BeforeRestore([this]() noexcept {
    busWait_ = {};
    synchronizingCpu_ = yieldedCpu_ = false;
  });
  archive.Expect(uint32_t(dspMode_));
  archive.Expect(sampleRate_);
  for (auto byte : iplRom_)
    archive.Expect(byte);
  Spc700::SerializeState(archive);
  archive.Array(ram_);
  archive.Object(io_, [](auto &fields, auto &next) {
    fields.Number(next.timersDisable);
    fields.Number(next.ramWritable);
    fields.Number(next.ramDisable);
    fields.Number(next.timersEnable);
    fields.Bounded(next.externalWaitStates, uint8_t{0}, uint8_t{3});
    fields.Bounded(next.internalWaitStates, uint8_t{0}, uint8_t{3});
    fields.Number(next.iplRomEnable);
    fields.Number(next.dspAddr);
    fields.Number(next.aux4);
    fields.Number(next.aux5);
  });
  archive.Array(apuInput_);
  archive.Array(cpuOutput_);
  const auto timer = [](auto &fields, auto &next, uint8_t divider) {
    fields.Bounded(next.stage0, uint8_t{0}, uint8_t(divider - 1));
    fields.Bounded(next.stage1, uint8_t{0}, uint8_t{1});
    fields.Number(next.stage2);
    fields.Bounded(next.stage3, uint8_t{0}, uint8_t{15});
    fields.Number(next.line);
    fields.Number(next.enable);
    fields.Number(next.target);
  };
  archive.Object(timer0_, [&](auto &fields, auto &next) { timer(fields, next, 128); });
  archive.Object(timer1_, [&](auto &fields, auto &next) { timer(fields, next, 128); });
  archive.Object(timer2_, [&](auto &fields, auto &next) { timer(fields, next, 16); });
  const auto hardware = archive.Bounded(hardwareClocks_, uint64_t{0}, uint64_t(INT64_MAX));
  archive.Bounded(dspClockBalance_, int64_t{-65536}, int64_t{63});
  const auto target = archive.Bounded(synchronizationTarget_, uint64_t{0}, uint64_t(INT64_MAX));
  const auto phase = archive.Number(synchronizationPhase_);
  const auto frequency =
      archive.Bounded(synchronizationFrequency_, uint32_t{1}, uint32_t{100000000});
  const auto distance = hardware >= target ? hardware - target : target - hardware;
  archive.Validate(phase < frequency && distance <= uint64_t(INT64_MAX) / frequency,
                   "Invalid SMP clock relationship");
}
} // namespace snes::core
