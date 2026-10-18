// Top-level emulator lifecycle and subsystem interface.

#pragma once
// Owns every hardware subsystem and wires them together:
//   CPU (65816)  ↔  MemoryBus  ↔  PPU / APU / DMA / Cartridge
//   Timing → DRAM refresh, HDMA, NMI/IRQ polling, auto-joypad
//   IrqController → NMI / IRQ dispatch to CPU
//   AutoJoypad → controller polling fed into CpuIoRegisters
//   Smp + Dsp → APU audio generation
//
// StepFrame() advances to the next display event, retaining an unfinished
// CPU or DMA operation for the following call.

#include "snes/core/AutoJoypad.hpp"
#include "snes/core/Cartridge.hpp"
#include "snes/core/CpuIoRegisters.hpp"
#include "snes/core/Dma.hpp"
#include "snes/core/Dsp.hpp"
#include "snes/core/Dsp1.hpp"
#include "snes/core/Dsp2.hpp"
#include "snes/core/IrqController.hpp"
#include "snes/core/MemoryBus.hpp"
#include "snes/core/Msu1.hpp"
#include "snes/core/Platform.hpp"
#include "snes/core/PowerOnState.hpp"
#include "snes/core/Ppu.hpp"
#include "snes/core/Smp.hpp"
#include "snes/core/Srtc.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/Trace.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace snes::core {

class SnesCpu; // forward - constructed after MemoryBus
namespace detail {
class ExecutionContext;
}

struct FrameStepOptions {
  bool emitCpuBusTrace = true;
  bool emitPpuTrace = true;
  bool emitDmaTrace = true;
};

struct FrameStepResult {
  uint64_t frameIndex = 0;
  uint64_t masterCycles = 0;
  InputState latchedInput{};
};

class Emulator {
public:
  struct Config {
    uint32_t frameRate = 60;
    uint32_t visibleWidth = 256;
    uint32_t visibleHeight = 224;
    uint64_t masterCyclesPerFrame = 357368;
    std::shared_ptr<const CartridgeFirmware> cartridgeFirmware;
    EntropyMode entropyMode = EntropyMode::Low;
    std::optional<uint32_t> entropySeed;
    uint32_t entropySequence = 0;
    Ppu::Config ppu;
  };

  explicit Emulator();
  explicit Emulator(Config config);
  ~Emulator();

  // Non-copyable, non-movable (owns large subsystems by value)
  Emulator(const Emulator &) = delete;
  Emulator &operator=(const Emulator &) = delete;

  void AttachVideoOutput(IVideoOutput *output);
  void AttachAudioOutput(IAudioOutput *output);
  void AttachInputProvider(IInputProvider *input);
  void AddTraceSink(ITraceSink *sink);

  bool LoadCartridge(std::span<const uint8_t> romData, std::string *error = nullptr);
  bool LoadCartridgeFromFile(const std::string &path, std::string *error = nullptr);
  bool LoadSufamiTurbo(std::span<const uint8_t> bios, std::span<const uint8_t> slotA,
                       std::span<const uint8_t> slotB, std::string *error = nullptr);
  bool LoadSufamiTurboFromFiles(const std::string &bios, const std::string &slotA,
                                const std::string &slotB, std::string *error = nullptr);
  bool LoadSuperGameBoy(std::span<const uint8_t> bios, std::span<const uint8_t> game,
                        std::span<const uint8_t> boot, std::string *error = nullptr);
  bool LoadSuperGameBoyFromFiles(const std::string &bios, const std::string &game,
                                 const std::string &boot, std::string *error = nullptr);
  bool LoadBroadcastCartridge(std::span<const uint8_t> base, std::span<const uint8_t> pack,
                              std::string *error = nullptr);
  bool LoadBroadcastCartridgeFromFiles(const std::string &base, const std::string &pack,
                                       std::string *error = nullptr);
  bool LoadCompetitionCartridge(CompetitionBoard::Settings settings,
                                const std::array<std::span<const uint8_t>, 4> &images,
                                std::string *error = nullptr);
  bool LoadCompetitionCartridgeFromFiles(CompetitionBoard::Settings settings,
                                         const std::array<std::string, 4> &paths,
                                         std::string *error = nullptr);
  bool LoadMemoryPack(std::span<const uint8_t> data) {
    return cartridge_ && cartridge_->LoadMemoryPack(data);
  }
  bool LoadMemoryPackMetadata(std::span<const uint8_t> bytes) {
    return cartridge_ && cartridge_->LoadMemoryPackMetadata(bytes);
  }
  bool LoadBroadcastStream(uint16_t channel, uint8_t sequence, std::span<const uint8_t> data) {
    return cartridge_ && cartridge_->LoadBroadcastStream(channel, sequence, data);
  }
  bool SetBroadcastTimeSource(std::function<BroadcastTime()> source) {
    return cartridge_ && cartridge_->SetBroadcastTimeSource(std::move(source));
  }

  const Cartridge *LoadedCartridge() const noexcept;
  void LoadSram(std::span<const uint8_t> data) {
    if (cartridge_)
      cartridge_->LoadSram(data);
  }
  void LoadSlotSram(unsigned slot, std::span<const uint8_t> data) {
    if (cartridge_)
      cartridge_->LoadSlotSram(slot, data);
  }
  std::vector<uint8_t> SaveSlotBattery(unsigned slot) const {
    return cartridge_ ? cartridge_->SaveSlotBattery(slot) : std::vector<uint8_t>{};
  }
  bool LoadSlotBattery(unsigned slot, std::span<const uint8_t> data) {
    return cartridge_ && cartridge_->LoadSlotBattery(slot, data);
  }
  std::vector<uint8_t> SaveRtc() {
    if (!srtc_)
      return cartridge_ ? cartridge_->SaveRtc() : std::vector<uint8_t>{};
    const auto data = srtc_->Save();
    return {data.begin(), data.end()};
  }
  bool LoadRtc(std::span<const uint8_t> data) {
    return srtc_ ? srtc_->Load(data) : cartridge_ && cartridge_->LoadRtc(data);
  }

  // Call between emulation steps. Retains RAM, media and platform attachments.
  bool Reset();

  FrameStepResult StepFrame(const FrameStepOptions &options = {});

  uint64_t CurrentFrame() const noexcept;
  uint64_t CurrentMasterCycles() const noexcept;

  // Subsystem accessors (for testing / debugging)
  Ppu &GetPpu() noexcept {
    return ppu_;
  }
  Smp &GetSmp() noexcept {
    return smp_;
  }
  Dsp &GetDsp() noexcept {
    return dsp_;
  }
  Timing &GetTiming() noexcept {
    return timing_;
  }
  IrqController &GetIrq() noexcept {
    return irq_;
  }
  AutoJoypad &GetAutoJoypad() noexcept {
    return autoJoypad_;
  }
  CpuIoRegisters &GetCpuIo() noexcept {
    return cpuIo_;
  }
  DmaController &GetDma() noexcept {
    return dma_;
  }
  MemoryBus &GetBus() noexcept {
    return bus_;
  }
  SnesCpu *GetCpu() noexcept {
    return cpu_.get();
  }

  bool Initialized() const noexcept {
    return initialized_;
  }

private:
  /// Wire all subsystem callbacks and map the bus.
  /// Called from LoadCartridge() once the cartridge is ready.
  void InitSubsystems();
  bool LoadCartridgeWithFirmware(std::span<const uint8_t> romData,
                                 const CartridgeFirmware *firmware, std::string *error);
  void ResetHardware(bool warm);
  void AdvanceClocks(uint32_t clocks);
  void SynchronizeDevices(bool communication = false);
  void SynchronizeCoprocessors();
  void StopFrameExecution();
  uint32_t ResumeCpuInstruction();
  void SuspendFrame();
  void ServicePendingHdma();
  void BeginCpuCycle(uint32_t clocks);

  void EmitTrace(const TraceEvent &event) const;

  Config config_;
  uint64_t frameIndex_ = 0;
  uint64_t masterCycles_ = 0;
  bool initialized_ = false;

  IVideoOutput *videoOutput_ = nullptr;
  IAudioOutput *audioOutput_ = nullptr;
  IInputProvider *inputProvider_ = nullptr;
  std::vector<ITraceSink *> traceSinks_;

  CartridgeDatabase cartridgeDatabase_{};
  std::optional<Cartridge> cartridge_{};

  // Hardware subsystems (construction order matters - bus_ before cpu_)
  Dsp dsp_;
  Smp smp_;
  Ppu ppu_;
  CpuIoRegisters cpuIo_;
  DmaController dma_;
  IrqController irq_;
  AutoJoypad autoJoypad_;
  Timing timing_;
  MemoryBus bus_;
  std::unique_ptr<SnesCpu> cpu_; // created in InitSubsystems()
  std::unique_ptr<detail::ExecutionContext> execution_;
  bool singleInstruction_ = false;
  std::unique_ptr<Dsp1> dsp1_; // created if cartridge uses DSP-1
  std::unique_ptr<Dsp2> dsp2_;
  std::unique_ptr<Srtc> srtc_;
  std::unique_ptr<Msu1> msu1_;

  // Audio output buffer (stereo interleaved int16_t, enough for 1+ frames)
  static constexpr int kAudioBufSamples = 2048;
  std::array<int16_t, kAudioBufSamples * 2> audioBuf_{};
  std::vector<int16_t> audioOverflow_;
  bool collectingAudio_ = false;
  uint64_t nextSmpMasterClock_ = 0;
  uint64_t coprocessorsSyncedClock_ = 0;
  uint64_t coprocessorClockTarget_ = 0;
  uint64_t coprocessorCompletedClock_ = 0;
  AudioBuffer audioOutputBuffer_;
  VideoFrame videoOutputFrame_;

  // Refresh stalls raised by Timing callbacks and drained after a bus interval.
  uint32_t pendingExtraClocks_ = 0;
  bool pendingHdmaSetup_ = false;
  bool pendingHdmaRun_ = false;
  bool servicingHdma_ = false;
  bool dmaArmed_ = false;
};

} // namespace snes::core
