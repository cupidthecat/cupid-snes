// Top-level emulator lifecycle and subsystem interface.

#pragma once

#include "snes/core/StateOperation.hpp"
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
#include "snes/core/audio/StreamMixer.hpp"

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

struct InstructionStepResult {
  bool instructionComplete = false;
  std::optional<FrameStepResult> field;
};

class Emulator {
  friend class MemoryInspector;

public:
  enum class ExpansionDevice { None, Satellaview };
  struct Config {
    uint32_t frameRate = 60;
    uint32_t visibleWidth = 256;
    uint32_t visibleHeight = 224;
    uint64_t masterCyclesPerFrame = 357368;
    std::shared_ptr<const CartridgeFirmware> cartridgeFirmware;
    EntropyMode entropyMode = EntropyMode::Low;
    std::optional<uint32_t> entropySeed;
    uint32_t entropySequence = 0;
    uint8_t cpuRevision = 2;
    bool sa1BusContention = false;
    bool superFxDelayedSync = true;
    bool cx4DelayedSync = true;
    Smp::DspMode dspMode = Smp::DspMode::Batch32;
    Ppu::Config ppu{.model = Ppu::RenderModel::Scanline};
    bool gameCompatibilityFixes = true;
    ExpansionDevice expansionDevice = ExpansionDevice::None;
    // Unset selects the cartridge's declared region.
    std::optional<Region> regionOverride;
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
  // The host clock binding applies to current and future handheld loads.
  void SetGameBoyTimeSource(std::function<uint64_t()> source);
  void AddTraceSink(ITraceSink *sink);
  void RemoveTraceSink(ITraceSink *sink) noexcept;

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
    const bool loaded = cartridge_ && cartridge_->LoadMemoryPack(data);
    if (loaded)
      InvalidatePreparedRestores();
    return loaded;
  }
  bool LoadMemoryPackMetadata(std::span<const uint8_t> bytes) {
    const bool loaded = cartridge_ && cartridge_->LoadMemoryPackMetadata(bytes);
    if (loaded)
      InvalidatePreparedRestores();
    return loaded;
  }
  bool LoadBroadcastStream(uint16_t channel, uint8_t sequence, std::span<const uint8_t> data) {
    const bool loaded = config_.expansionDevice == ExpansionDevice::Satellaview &&
                        broadcastReceiver_.LoadStream(channel, sequence, data);
    if (loaded)
      InvalidatePreparedRestores();
    return loaded;
  }
  bool SetBroadcastTimeSource(std::function<BroadcastTime()> source) {
    if (config_.expansionDevice != ExpansionDevice::Satellaview)
      return false;
    broadcastReceiver_.SetTimeSource(std::move(source));
    return true;
  }
  void SetExpansionDevice(ExpansionDevice device);
  ExpansionDevice GetExpansionDevice() const noexcept {
    return config_.expansionDevice;
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
  bool PowerCycle();

  FrameStepResult StepFrame(const FrameStepOptions &options = {});
  // Returns at an instruction boundary or a crossed display event. A field
  // result can retain an unfinished instruction or DMA transfer for the next call.
  InstructionStepResult StepInstruction();
  bool AtInstructionBoundary() const noexcept {
    return atStateBoundary_;
  }

  class PreparedStateRestore {
  public:
    PreparedStateRestore(PreparedStateRestore &&) noexcept;
    PreparedStateRestore &operator=(PreparedStateRestore &&) noexcept;
    ~PreparedStateRestore();
    void Commit();

  private:
    friend class Emulator;
    struct Impl;
    explicit PreparedStateRestore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
  };

  // Call between emulation steps. Capture may complete pending hardware work
  // and deliver crossed fields. Restore retains the target's platform bindings.
  std::vector<uint8_t> CaptureState(const StateOperation &operation = {});
  PreparedStateRestore PrepareStateRestore(std::span<const uint8_t> bytes,
                                           const StateOperation &operation = {});
  void RestoreState(std::span<const uint8_t> bytes, const StateOperation &operation = {});
  const VideoFrame &LastFrame() const noexcept {
    return videoOutputFrame_;
  }

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
  CheatSystem CheatsSystem() const noexcept;
  void SetCheats(std::span<const CheatCode> codes);

private:
  /// Wire all subsystem callbacks and map the bus.
  /// Called from LoadCartridge() once the cartridge is ready.
  void InitSubsystems();
  void ReplaceCartridge(Cartridge &&cartridge);
  bool LoadCartridgeWithFirmware(std::span<const uint8_t> romData,
                                 const CartridgeFirmware *firmware, std::string *error);
  void ResetHardware(bool warm);
  bool Restart(bool warm);
  void AdvanceClocks(uint32_t clocks);
  void SynchronizeDevices(bool communication = false);
  void RouteDspSamples(int firstSample);
  void SynchronizeState(const StateOperation &operation);
  void SerializeState(detail::StateArchive &archive);
  void BeginFrame();
  FrameStepResult FinishFrame();
  void ResumeFrameExecution(bool instructionBoundary);
  void InvalidatePreparedRestores() noexcept;
  void ConfigureAudioStreams();
  void AttachMsu1(const std::string &path);
  void SynchronizeExpansionAudio(uint64_t elapsed);
  void SynchronizeCoprocessors(bool forceExecution = false);
  void StopFrameExecution();
  uint32_t ResumeCpuInstruction();
  void SuspendFrame();
  void ServicePendingHdma();
  void BeginCpuCycle(uint32_t clocks);

  void EmitTrace(const TraceEvent &event) const;

  Config config_;
  BroadcastReceiver broadcastReceiver_;
  uint64_t frameIndex_ = 0;
  uint64_t masterCycles_ = 0;
  bool initialized_ = false;

  IVideoOutput *videoOutput_ = nullptr;
  IAudioOutput *audioOutput_ = nullptr;
  IInputProvider *inputProvider_ = nullptr;
  std::vector<ITraceSink *> traceSinks_;

  CartridgeDatabase cartridgeDatabase_{};
  std::shared_ptr<gameboy::StartupState> handheldStartup_;
  std::optional<PowerOnState> startupEntropy_;
  std::optional<Cartridge> cartridge_{};
  // Cache words and return-stack entries belong to this console across cartridge changes.
  std::optional<Cx4Processor::RetainedStorage> cx4Storage_;

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
  CheatEngine cheats_;
  std::unique_ptr<SnesCpu> cpu_; // created in InitSubsystems()
  std::unique_ptr<detail::ExecutionContext> execution_;
  bool singleInstruction_ = false;
  bool instructionFields_ = false;
  std::unique_ptr<Dsp1> dsp1_; // created if cartridge uses DSP-1
  std::unique_ptr<Dsp2> dsp2_;
  std::unique_ptr<Srtc> srtc_;
  std::unique_ptr<Msu1> msu1_;
  audio::StreamMixer audioMixer_;
  uint64_t msuSamplesClocked_ = 0;

  // Audio output buffer (stereo interleaved int16_t, enough for 1+ frames)
  static constexpr int kAudioBufSamples = 2048;
  std::array<int16_t, kAudioBufSamples * 2> audioBuf_{};
  bool collectingAudio_ = false;
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

  enum class YieldReason { None, Frame, Instruction };
  YieldReason yieldReason_ = YieldReason::None;
  bool atStateBoundary_ = true;
  bool synchronizingState_ = false;
  bool frameOpen_ = false;
  InputState frameInput_{};
  std::shared_ptr<uint8_t> restoreLifetime_;
  std::function<uint64_t()> handheldTimeSource_;
};

} // namespace snes::core
