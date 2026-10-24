#include "snes/core/CpuIoRegisters.hpp"
#include "snes/core/Dma.hpp"
#include "snes/core/MemoryBus.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void MemoryBus::SerializeState(detail::StateArchive &archive) {
  // Handler slots and mappings belong to the attached machine.
  archive.Array(wram_);
  archive.Bounded(wmdataAddr_, 0u, 0x1ffffu);
  archive.Number(mdr_);
  archive.Number(fastRom_);
}

void CpuIoRegisters::SerializeState(detail::StateArchive &archive) {
  archive.Expect(cpuVersion_);
  archive.Number(nmiEnable_);
  const auto horizontal = archive.Number(hirqEnable_);
  const auto vertical = archive.Number(virqEnable_);
  const auto irq = archive.Number(irqEnable_);
  archive.Validate(irq == (horizontal || vertical), "Invalid CPU I/O interrupt enables");
  archive.Number(autoJoypadPoll_);
  archive.Number(pio_);
  archive.Number(wrmpya_);
  archive.Number(wrmpyb_);
  archive.Number(wrdiva_);
  archive.Number(wrdivb_);
  archive.Bounded(htime_, uint16_t{0}, uint16_t{511});
  archive.Bounded(vtime_, uint16_t{0}, uint16_t{511});
  archive.Number(fastRom_);
  archive.Number(rddiv_);
  archive.Number(rdmpy_);
  archive.Number(joy1_);
  archive.Number(joy2_);
  archive.Number(joy3_);
  archive.Number(joy4_);
  archive.Number(nmiFlag_);
  archive.Number(irqFlag_);
  archive.Bounded(autoJoypadCounter_, uint8_t{0}, uint8_t{33});
  archive.Object(alu_, [](auto &fields, auto &alu) {
    fields.Bounded(alu.mpyctr, 0u, 8u);
    fields.Bounded(alu.divctr, 0u, 16u);
    fields.Validate(!alu.mpyctr || !alu.divctr, "Invalid CPU arithmetic operation");
    fields.Number(alu.shift);
    fields.Number(alu.busyOnWrite);
  });
  archive.Number(joypadLatch_);
}

void DmaController::SerializeState(detail::StateArchive &archive) {
  for (auto &channel : channels_)
    archive.Object(channel, [](auto &fields, auto &value) {
      fields.Bounded(value.transferMode, uint8_t{0}, uint8_t{7});
      fields.Number(value.fixedTransfer);
      fields.Number(value.reverseTransfer);
      fields.Number(value.unused);
      fields.Number(value.indirect);
      fields.Number(value.direction);
      fields.Number(value.targetAddress);
      fields.Number(value.sourceAddress);
      fields.Number(value.sourceBank);
      fields.Number(value.transferSize);
      fields.Number(value.indirectBank);
      fields.Number(value.hdmaAddress);
      fields.Number(value.lineCounter);
      fields.Number(value.unknown);
      fields.Number(value.dmaEnable);
      fields.Number(value.hdmaEnable);
      fields.Number(value.hdmaCompleted);
      fields.Number(value.hdmaDoTransfer);
    });
  archive.Number(dmaClocks_);
  archive.Number(cpuCycleClocks_);
  const auto dma = archive.Number(inDma_);
  const auto hdma = archive.Number(inHdma_);
  archive.Validate(!dma && !hdma, "DMA checkpoint requires a completed transfer boundary");
}
} // namespace snes::core
