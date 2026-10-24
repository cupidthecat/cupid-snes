// Handheld processor, address bus, timer, and cartridge execution.
#pragma once

#include "snes/core/gameboy/Cartridge.hpp"
#include "snes/core/gameboy/ClockedIo.hpp"
#include "snes/core/gameboy/Cpu.hpp"
#include "snes/core/gameboy/Host.hpp"
#include "snes/core/gameboy/StartupState.hpp"

#include <array>
#include <functional>
#include <memory>
#include <utility>

namespace snes::core::detail {
class StateArchive;
}

namespace snes::core::gameboy {

class Machine final : private Cpu::Bus, private ClockedIo::Bus {
public:
  void SerializeState(detail::StateArchive &archive);
  enum class Access { Cpu, Dma };
  struct Devices {
    virtual ~Devices() = default;
    virtual void Reset(bool secondModel) = 0;
    virtual void Reset(bool secondModel, bool) {
      Reset(secondModel);
    }
    virtual void InitializePowerOnMemory(std::span<const uint8_t, 160>,
                                         std::span<const uint8_t, 16>) {}
    virtual uint8_t Read(uint16_t address, Access access) = 0;
    // Writes and elapsed clocks return newly requested interrupt bits.
    virtual uint8_t Write(uint16_t address, uint8_t value, Access access) = 0;
    virtual uint8_t WriteCpu(uint16_t address, uint8_t value, uint16_t) {
      return Write(address, value, Access::Cpu);
    }
    virtual uint8_t Advance(unsigned clocks) = 0;
    virtual void AudioDividerEdge(bool rising, uint64_t clock) = 0;
    // Complete a CPU bus timing batch after its individual hardware clocks.
    virtual void FinishAdvance() {}
    virtual void AddressBus(uint16_t, bool) {}
    virtual void BlockedOamAccess(uint16_t, bool) {}
    virtual void SetDmaSynchronizer(std::function<void()>) {}
    virtual void SetCpuSleeping(bool) {}
    virtual void Stop(bool) {}
    virtual void WindowWrite(bool) {}
    virtual uint8_t DmaStatus(uint16_t, uint8_t) {
      return 0;
    }
  };

  Machine(Host &host, Devices &devices);
  bool Load(std::span<const uint8_t> image, std::span<const uint8_t> boot,
            std::string *error = nullptr);
  void Reset(bool secondModel = false, bool preserveAudio = false);
  void SetStartupEntropy(bool enabled, uint64_t seed) noexcept {
    startup_->Configure(enabled, seed);
    cartridge_.SetCameraEntropy(seed, enabled);
  }
  void SetStartupState(std::shared_ptr<StartupState> state) noexcept {
    if (state)
      startup_ = std::move(state);
    cartridge_.SetCameraEntropy(0, startup_->Enabled());
  }
  unsigned RunInstruction();
  void AdvanceHardware(unsigned clocks);
  uint8_t ReadMemory(uint16_t address);
  void WriteMemory(uint16_t address, uint8_t value);
  const Cpu::State &CpuState() const noexcept {
    return cpu_.GetState();
  }
  void SetCpuState(const Cpu::State &state);
  Cartridge &GameCartridge() noexcept {
    return cartridge_;
  }
  const Cartridge &GameCartridge() const noexcept {
    return cartridge_;
  }
  uint64_t Clocks() const noexcept {
    return cpuClock_;
  }

private:
  Host &host_;
  Devices &devices_;
  Cpu cpu_{*this};
  ClockedIo timer_{*this};
  Cartridge cartridge_;
  std::shared_ptr<StartupState> startup_ = std::make_shared<StartupState>();
  std::array<uint8_t, 8192> ram_{};
  std::array<uint8_t, 127> highRam_{};
  std::array<uint8_t, 256> boot_{};
  uint64_t cpuClock_ = 0, deviceClock_ = 0, dmaClock_ = 0;
  uint16_t dmaSource_ = 0;
  uint8_t dmaPosition_ = 0xa1, dmaClocks_ = 0, dmaRegister_ = 0xff;
  uint8_t flags_ = 0, enable_ = 0, joypad_ = 0xcf, dataBus_ = 0, decay_ = 0;
  bool bootDisabled_ = false, dmaRestarting_ = false;

  uint8_t Read(uint16_t address) override;
  void Write(uint16_t address, uint8_t value) override;
  void Advance(unsigned clocks) override;
  void Synchronize() override;
  uint8_t InterruptEnable() const override {
    return enable_;
  }
  uint8_t InterruptFlags() const override {
    return flags_;
  }
  void AcknowledgeInterrupt(uint8_t mask) override;
  uint8_t WriteInterruptLow(uint16_t address, uint8_t value) override;
  void DriveAddress(uint16_t address) override;
  void AddressIncremented(uint16_t address) override;
  void SetStopped(bool stopped) override;
  void Resume() override;
  void RequestInterrupt(uint8_t mask) override;
  void AudioDividerEdge(bool rising) override;

  void Flush(uint64_t target);
  uint8_t ReadData(uint16_t address, Access access);
  void WriteData(uint16_t address, uint8_t value, Access access);
  void DriveData(uint16_t address, uint8_t value);
  bool DmaConflict(uint16_t address) const noexcept;
  void RunDma(unsigned clocks, bool force = false);
  void SynchronizeDma(bool beforeRead = false);
};

} // namespace snes::core::gameboy
