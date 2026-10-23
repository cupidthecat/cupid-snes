#include "snes/core/Hg51b.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void Hg51b::SerializeState(detail::StateArchive &archive) {
  detail::StateArchive identity;
  identity.Array(state_.dataRom);
  for (auto byte : detail::Sha256(identity.Bytes()))
    archive.Expect(byte);
  archive.Object(state_, [](auto &fields, auto &state) {
    const auto word = [](auto &values, uint32_t &value) { values.Bounded(value, 0u, Mask); };
    auto &r = state.registers;
    for (auto &value : r.general)
      word(fields, value);
    fields.Bounded(r.product, uint64_t{0}, uint64_t{0xffffffffffff});
    word(fields, r.accumulator);
    word(fields, r.memoryData);
    word(fields, r.romData);
    word(fields, r.ramData);
    word(fields, r.memoryAddress);
    word(fields, r.dataPointer);
    fields.Bounded(r.bank, uint16_t{0}, uint16_t{0x7fff});
    fields.Bounded(r.page, uint16_t{0}, uint16_t{0x7fff});
    fields.Number(r.counter);
    fields.Bounded(r.flags, uint8_t{0}, uint8_t{31});
    auto &c = state.control;
    fields.Number(c.locked);
    fields.Number(c.halted);
    fields.Number(c.irqDisabled);
    fields.Number(c.singleRom);
    fields.Bounded(c.romWait, uint8_t{0}, uint8_t{7});
    fields.Bounded(c.ramWait, uint8_t{0}, uint8_t{7});
    fields.Array(c.vectors);
    fields.Number(c.suspended);
    fields.Number(c.suspendClocks);
    auto &cache = c.cache;
    fields.Number(cache.loading);
    fields.Number(cache.preload);
    fields.Bounded(cache.page, uint8_t{0}, uint8_t{1});
    fields.Array(cache.locked);
    for (auto &address : cache.address)
      word(fields, address);
    word(fields, cache.base);
    fields.Bounded(cache.bank, uint16_t{0}, uint16_t{0x7fff});
    fields.Number(cache.counter);
    fields.Number(c.dma.active);
    word(fields, c.dma.source);
    word(fields, c.dma.target);
    fields.Number(c.dma.length);
    fields.Number(c.bus.active);
    fields.Number(c.bus.reading);
    fields.Number(c.bus.writing);
    fields.Bounded(c.bus.pending, uint8_t{0}, uint8_t{15});
    word(fields, c.bus.address);
    for (auto &address : state.stack)
      fields.Bounded(address, 0u, 0x7fffffu);
    for (auto &page : state.program)
      fields.Array(page);
    fields.Array(state.dataRam);
  });
  Phase phase = continuation_.phase;
  archive.Object(continuation_, [&](auto &fields, auto &work) {
    phase = fields.Enumeration(work.phase, Phase::Begin, Phase::SkipClock);
    fields.Validate(phase != Phase::DmaNext && phase != Phase::LatchOpcode &&
                        phase != Phase::InstructionClock && phase != Phase::SkipClock,
                    "HG51B state requires a clock return or completed step");
    fields.Enumeration(work.cacheReturn, CacheReturn::Finish, CacheReturn::Skip);
    fields.Bounded(work.cacheAddress, 0u, Mask);
    fields.Bounded(work.cacheWord, uint16_t{0}, uint16_t{256});
    fields.Number(work.opcode);
    fields.Bounded(work.dmaLength, 0u, 65535u);
    fields.Bounded(work.dmaOffset, 0u, work.dmaLength);
    fields.Bounded(work.dmaSource, 0u, Mask);
    fields.Bounded(work.dmaTarget, 0u, Mask);
    fields.Number(work.dmaValue);
    if (phase == Phase::CacheLow || phase == Phase::CacheHigh)
      fields.Validate(work.cacheWord < 256, "Invalid HG51B cache-fill position");
    if (phase == Phase::DmaRead || phase == Phase::DmaWrite)
      fields.Validate(work.dmaOffset < work.dmaLength, "Invalid HG51B DMA position");
  });
  const auto idle = archive.Number(idleCycle_);
  archive.Validate(!idle || phase == Phase::ClearIdle, "Invalid HG51B idle return");
}
} // namespace snes::core
