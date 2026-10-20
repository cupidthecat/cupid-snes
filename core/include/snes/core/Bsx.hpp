// BS-X BIOS cartridge controller and Satellaview register interface.
#pragma once

#include "snes/core/BroadcastReceiver.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace snes::core {

class Bsx {
public:
  // BIOS, memory pack and battery RAM remain owned by the cartridge. Their
  // allocations must stay stable for this device's lifetime. PSRAM is owned here.
  Bsx(std::span<const uint8_t> bios, std::span<uint8_t> memoryPack, std::span<uint8_t> sram);
  ~Bsx();
  Bsx(Bsx &&) noexcept;
  Bsx &operator=(Bsx &&) noexcept;
  Bsx(const Bsx &) = delete;
  Bsx &operator=(const Bsx &) = delete;

  // Reset the controller/protocol without erasing any RAM or flash. Injected
  // broadcast assets and the time provider remain installed.
  void Reset();
  void SetPal(bool pal);
  void Advance(uint32_t masterClocks);
  static bool Selects(uint32_t cpuAddress) noexcept;
  uint8_t ReadCpu(uint32_t cpuAddress, uint8_t openBus = 0xff);
  void WriteCpu(uint32_t cpuAddress, uint8_t value);
  bool CpuIrqPending() const noexcept;
  std::span<uint8_t> Psram() noexcept;
  std::vector<uint8_t> FlashMetadata() const;
  bool LoadFlashMetadata(std::span<const uint8_t> bytes);

  // Installing a payload opts into recorded reception; ClearStreams restores
  // the default register interface. Both receivers select channels. Payloads are
  // copied; each has ceil(size/22) packets. At the end of a sequence, a missing
  // following asset restarts sequence zero. An empty payload removes that asset.
  // Channel zero is reserved for the clock. False means invalid channel/size.
  bool LoadStream(uint16_t channel, uint8_t sequence, std::span<const uint8_t> payload);
  void ClearStreams();

  // No filesystem, network or wall clock is consulted by the device. With no
  // provider the clock stays at 00:00:00. Each 18-byte clock record captures one
  // provider result. Recorded reception uses a shared 23-byte calendar record.
  void SetTimeSource(std::function<BroadcastTime()> source);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace snes::core
