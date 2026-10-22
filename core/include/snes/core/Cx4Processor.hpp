#pragma once

#include "snes/core/Hg51b.hpp"

#include <cstddef>
#include <memory>
#include <span>

namespace snes::core {
class MemoryBus;
namespace detail {
class ExecutionContext;
}

class Cx4Processor : private Hg51b {
public:
  enum class Layout { LoRom, HiRom };
  struct RetainedStorage {
    std::array<std::array<uint16_t, 256>, 2> program{};
    std::array<uint32_t, 8> stack{};
  };
  Cx4Processor(std::span<const uint8_t> rom, std::span<uint8_t> save,
               std::span<const uint8_t> firmware, Layout layout = Layout::LoRom);
  ~Cx4Processor();
  void Reset();
  RetainedStorage CaptureStorage() const noexcept;
  void InitializeStorage(const RetainedStorage &storage);
  void SetPal(bool pal);
  void SetDelayedSync(bool enabled) noexcept {
    delayedSync_ = enabled;
  }
  void Advance(uint32_t masterClocks, bool synchronize = true);
  uint8_t Read(uint32_t address, uint8_t openBus);
  void Write(uint32_t address, uint8_t value);
  void MapCpu(MemoryBus &bus, uint8_t slot) const;
  bool CpuIrqPending() const noexcept {
    return irqLine_;
  }

private:
  void Synchronize();
  bool IsRom(uint32_t address) const override;
  bool IsRam(uint32_t address) const override;
  bool IsData(uint32_t address) const;
  bool IsIo(uint32_t address) const;
  uint32_t RomOffset(uint32_t address) const;
  uint32_t RamOffset(uint32_t address) const;
  static std::size_t Mirror(uint32_t address, std::size_t size);
  uint8_t ReadRom(uint32_t address, uint8_t openBus) const;
  uint8_t ReadRam(uint32_t address) const;
  uint8_t ReadExternal(uint32_t address) override;
  void WriteExternal(uint32_t address, uint8_t value) override;
  void StepClocks(unsigned clocks) override;
  void Halt() override;
  void ClearIrq() override;
  std::span<const uint8_t> rom_;
  std::span<uint8_t> save_;
  Layout layout_;
  std::unique_ptr<detail::ExecutionContext> execution_;
  uint32_t masterFrequency_ = 21'477'272;
  int64_t balance_ = 0;
  bool irqLine_ = false;
  bool delayedSync_ = false;
};

} // namespace snes::core
