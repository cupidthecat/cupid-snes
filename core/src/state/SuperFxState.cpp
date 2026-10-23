#include "../cartridge/superfx/Impl.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void SuperFx::SerializeState(detail::StateArchive &archive) {
  auto &s = *impl_;
  archive.Expect(uint64_t(s.rom.size()));
  archive.Expect(uint64_t(s.ram.size()));
  archive.Expect(s.masterFrequency);
  archive.Array(s.r);
  archive.Array(s.cache);
  archive.Number(s.validLines);
  std::array<uint8_t, 2> pending{};
  size_t rowIndex = 0;
  archive.Array(s.pixels, [&](auto &fields, auto &row) {
    fields.Number(row.position);
    fields.Number(row.pending);
    fields.Array(row.colors);
    fields.Validate(row.position < 8192 || (row.position == 0xffff && !row.pending),
                    "Invalid Super FX pixel row");
    pending[rowIndex++] = row.pending;
  });
  const auto masked = [&]<class T>(T &value, T mask) {
    const auto next = archive.Number(value);
    archive.Validate((next & ~mask) == 0, "Invalid Super FX register bits");
  };
  masked(s.status, Impl::StatusMask);
  masked(s.cacheBase, uint16_t{0xfff0});
  archive.Number(s.lastRam);
  archive.Bounded(s.programBank, uint8_t{0}, uint8_t{0x7f});
  archive.Bounded(s.romBank, uint8_t{0}, uint8_t{0x7f});
  archive.Bounded(s.ramBank, uint8_t{0}, uint8_t{1});
  masked(s.config, uint8_t{0xa0});
  archive.Number(s.screenBase);
  masked(s.screenMode, uint8_t{0x3f});
  archive.Bounded(s.clockSelect, uint8_t{0}, uint8_t{1});
  archive.Bounded(s.backupEnable, uint8_t{0}, uint8_t{1});
  archive.Number(s.color);
  archive.Bounded(s.plotOptions, uint8_t{0}, uint8_t{31});
  archive.Number(s.pipeline);
  archive.Bounded(s.source, 0u, 15u);
  archive.Bounded(s.destination, 0u, 15u);
  archive.Number(s.pcWritten);
  archive.Number(s.r14Written);
  archive.Number(s.irqOutput);
  archive.Bounded(s.romRemaining, 0u, 6u);
  archive.Bounded(s.romAddress, 0u, 0x7fffffu);
  archive.Number(s.romBuffer);
  archive.Bounded(s.ramRemaining, 0u, 6u);
  archive.Bounded(s.ramAddress, 0u, 0x1ffffu);
  archive.Number(s.ramBuffer);
  archive.Validate(archive.Number(s.waitRemaining) == 0, "Super FX state requires a clock return");
  const auto balance =
      archive.Bounded(s.masterBalance, int64_t{0}, int64_t{14} * Timing::kMasterClockHz);
  const auto port = archive.Enumeration(s.waitPort, Impl::Port::None, Impl::Port::Ram);
  const auto cacheFill = archive.Number(s.cacheFill);
  const auto pixelFlush = archive.Number(s.pixelFlush);
  const auto idle = archive.Number(s.idleStep);
  const auto waiting = archive.Number(s.waitingForGo);

  Impl::Continuation work;
  archive.Object(s.work, [&](auto &fields, auto &next) {
    fields.Enumeration(next.phase, Impl::Phase::Begin, Impl::Phase::RomByte);
    fields.Validate(next.phase != Impl::Phase::Execute && next.phase != Impl::Phase::PlotWrite,
                    "Invalid Super FX instruction return");
    fields.Number(next.opcode);
    fields.Bounded(next.mode, 0u, 3u);
    fields.Number(next.input);
    fields.Number(next.immediate);
    fields.Number(next.operand);
    fields.Number(next.word);
    fields.Number(next.takeBranch);
    fields.Number(next.clearPrefix);
    fields.Validate((fields.Number(next.base) & ~0x10000u) == 0,
                    "Invalid Super FX instruction RAM bank");
    fields.Bounded(next.address, 0u, 0x4fbceu);
    fields.Number(next.data);
    fields.Number(next.x);
    fields.Number(next.y);
    fields.Bounded(next.row, uint16_t{0}, uint16_t{8191});
    fields.Number(next.depth);
    fields.Validate(next.depth == 0 || next.depth == 2 || next.depth == 4 || next.depth == 8,
                    "Invalid Super FX pixel depth");
    fields.Bounded(next.plane, 0u, next.depth);
    work = next;
  });
  Impl::FetchState fetch;
  archive.Object(s.fetch, [&](auto &fields, auto &next) {
    fields.Number(next.address);
    fields.Number(next.offset);
    fields.Bounded(next.bank, uint8_t{0}, uint8_t{0x7f});
    fields.Number(next.cached);
    fields.Number(next.hit);
    fields.Bounded(next.fillByte, 0u, 16u);
    fields.Enumeration(next.phase, Impl::FetchState::Phase::Synchronize,
                       Impl::FetchState::Phase::Access);
    fields.Validate((!next.cached || next.offset < 512) && (!next.hit || next.cached),
                    "Invalid Super FX opcode cache selection");
    fetch = next;
  });
  Impl::FlushState flush;
  archive.Object(s.flush, [&](auto &fields, auto &next) {
    fields.Bounded(next.index, 0u, 1u);
    fields.Bounded(next.address, 0u, 0x4fbceu);
    fields.Number(next.depth);
    fields.Validate(next.depth == 0 || next.depth == 2 || next.depth == 4 || next.depth == 8,
                    "Invalid Super FX flush depth");
    fields.Bounded(next.plane, 0u, next.depth);
    fields.Number(next.data);
    fields.Number(next.reading);
    flush = next;
  });

  Impl::StepPhase previous = Impl::StepPhase::Clock;
  size_t visited = 0;
  const auto count = archive.Vector(
      s.steps, detail::StateArchive::MaximumBytes / 10, [&](auto &fields, auto &step) {
        if (visited++)
          fields.Validate(previous == Impl::StepPhase::RomAccess ||
                              previous == Impl::StepPhase::RamAccess,
                          "Invalid Super FX interrupted memory step");
        fields.Bounded(step.clocks, 1u, 14u);
        fields.Enumeration(step.phase, Impl::StepPhase::RomCountdown, Impl::StepPhase::Clock);
        fields.Bounded(step.address, 0u,
                       step.phase == Impl::StepPhase::RamAccess ? 0x1ffffu : 0x7fffffu);
        fields.Number(step.data);
        previous = step.phase;
      });
  archive.Validate(!count || previous == Impl::StepPhase::Clock,
                   "Invalid Super FX pending clock step");
  archive.Validate(waiting == (work.phase == Impl::Phase::Begin) && idle == (waiting && count),
                   "Invalid Super FX stopped continuation");
  archive.Validate(count || (waiting && !cacheFill && !pixelFlush),
                   "Missing Super FX pending clock step");
  archive.Validate(count || balance == 0, "Invalid Super FX initial clock balance");
  const bool fetching = work.phase == Impl::Phase::Opcode ||
                        work.phase == Impl::Phase::ImmediateLow ||
                        work.phase == Impl::Phase::ImmediateHigh;
  archive.Validate(!fetching || fetch.cached == (fetch.offset < 512),
                   "Invalid Super FX pending opcode selection");
  archive.Validate(cacheFill == (fetching && !fetch.hit), "Invalid Super FX cache-fill return");
  if (cacheFill) {
    archive.Validate(!fetch.cached || fetch.fillByte < 16, "Invalid Super FX cache-fill byte");
    archive.Validate(port == (fetch.cached ? Impl::BankPort(fetch.bank) : Impl::Port::None),
                     "Invalid Super FX opcode memory port");
  }
  const bool flushing = work.phase == Impl::Phase::RpixFlushFirst ||
                        work.phase == Impl::Phase::RpixFlushSecond ||
                        work.phase == Impl::Phase::PlotEvict || work.phase == Impl::Phase::PlotFull;
  archive.Validate(pixelFlush == flushing, "Invalid Super FX pixel-flush return");
  if (flushing) {
    archive.Validate(work.opcode == 0x4c && flush.depth && flush.plane < flush.depth &&
                         port == Impl::Port::Ram && pending[flush.index] &&
                         flush.index == (work.phase == Impl::Phase::RpixFlushSecond ? 0u : 1u),
                     "Invalid Super FX pixel-flush position");
  }
  const bool immediate = (work.opcode & 0xf0) == 0xa0 || work.opcode >= 0xf0;
  const bool store = work.opcode >= 0x30 && work.opcode <= 0x3b;
  const bool load = work.opcode >= 0x40 && work.opcode <= 0x4b;
  const auto expectedPort =
      fetching && cacheFill && fetch.cached ? Impl::BankPort(fetch.bank)
      : flushing || work.phase == Impl::Phase::RpixPlane || work.phase == Impl::Phase::LoadLow ||
              work.phase == Impl::Phase::LoadWordLow || work.phase == Impl::Phase::LoadHigh
          ? Impl::Port::Ram
          : Impl::Port::None;
  archive.Validate(port == expectedPort, "Invalid Super FX continuation memory port");
  switch (work.phase) {
  case Impl::Phase::ImmediateLow:
    archive.Validate(immediate || (work.opcode >= 5 && work.opcode <= 15),
                     "Invalid Super FX immediate byte");
    break;
  case Impl::Phase::ImmediateHigh:
    archive.Validate(work.opcode >= 0xf0, "Invalid Super FX immediate high byte");
    break;
  case Impl::Phase::StoreLow:
    archive.Validate(store, "Invalid Super FX register store");
    break;
  case Impl::Phase::StoreWordLow:
    archive.Validate(work.opcode == 0x90 || (immediate && work.mode == 2),
                     "Invalid Super FX word store");
    break;
  case Impl::Phase::StoreHigh:
    archive.Validate(store || work.opcode == 0x90 || (immediate && work.mode == 2),
                     "Invalid Super FX high-byte store");
    break;
  case Impl::Phase::LoadLow:
    archive.Validate(load, "Invalid Super FX register load");
    break;
  case Impl::Phase::LoadWordLow:
    archive.Validate(immediate && (work.mode & 1), "Invalid Super FX word load");
    break;
  case Impl::Phase::LoadHigh:
    archive.Validate(load || (immediate && (work.mode & 1)), "Invalid Super FX high-byte load");
    break;
  case Impl::Phase::RpixFlushFirst:
  case Impl::Phase::RpixFlushSecond:
  case Impl::Phase::RpixPlane:
    archive.Validate(work.opcode == 0x4c && (work.mode & 1), "Invalid Super FX pixel read");
    if (work.phase == Impl::Phase::RpixPlane)
      archive.Validate(work.depth && work.plane < work.depth && port == Impl::Port::Ram,
                       "Invalid Super FX pixel-read plane");
    break;
  case Impl::Phase::PlotEvict:
  case Impl::Phase::PlotFull:
    archive.Validate(work.opcode == 0x4c && !(work.mode & 1), "Invalid Super FX pixel plot");
    break;
  case Impl::Phase::RomColor:
    archive.Validate(!(work.mode & 2), "Invalid Super FX buffered color read");
    [[fallthrough]];
  case Impl::Phase::RamBank:
    if (work.phase == Impl::Phase::RamBank)
      archive.Validate(work.mode == 2, "Invalid Super FX RAM bank return");
    [[fallthrough]];
  case Impl::Phase::RomBank:
    if (work.phase == Impl::Phase::RomBank)
      archive.Validate(work.mode == 3, "Invalid Super FX ROM bank return");
    archive.Validate(work.opcode == 0xdf, "Invalid Super FX buffer instruction");
    break;
  case Impl::Phase::RomByte:
    archive.Validate(work.opcode == 0xef, "Invalid Super FX buffered byte read");
    break;
  case Impl::Phase::Finish:
    archive.Validate(((work.opcode >= 0x80 && work.opcode <= 0x8f) || work.opcode == 0x9f) &&
                         !work.clearPrefix,
                     "Invalid Super FX multiply return");
    break;
  default:
    break;
  }
}
} // namespace snes::core
