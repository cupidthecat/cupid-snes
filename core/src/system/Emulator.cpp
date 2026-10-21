// Top-level subsystem wiring, cartridge loading, and frame execution.

#include "snes/core/Emulator.hpp"
#include "snes/core/Logging.hpp"
#include "snes/core/SnesCpu.hpp"
#include "snes/core/detail/Compatibility.hpp"
#include "snes/core/detail/ExecutionContext.hpp"
#include "snes/core/gameboy/StartupState.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace snes::core {

namespace {
std::optional<std::vector<uint8_t>> ReadImage(const std::string &path, std::string *error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error)
      *error = "Unable to open ROM file: " + path;
    return std::nullopt;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
  if (file.bad() || data.empty()) {
    if (error)
      *error = "ROM file is empty or unreadable: " + path;
    return std::nullopt;
  }
  return data;
}

std::optional<const char *> UnsupportedChipName(EnhancementChip chip) {
  switch (chip) {
  case EnhancementChip::None:
  case EnhancementChip::Dsp1:
  case EnhancementChip::Dsp2:
  case EnhancementChip::Obc1:
  case EnhancementChip::Srtc:
  case EnhancementChip::Sdd1:
  case EnhancementChip::St010:
  case EnhancementChip::Spc7110:
  case EnhancementChip::Spc7110Rtc:
  case EnhancementChip::Sa1:
  case EnhancementChip::SuperFx:
  case EnhancementChip::Cx4:
  case EnhancementChip::St011:
  case EnhancementChip::St018:
  case EnhancementChip::Dsp3:
  case EnhancementChip::Dsp4:
  case EnhancementChip::Competition:
  case EnhancementChip::SuperGameBoy:
    return std::nullopt;
  default:
    return ChipName(chip);
  }
}
} // namespace

// Construction / destruction

Emulator::Emulator() : Emulator(Config{}) {}

Emulator::Emulator(Config config)
    : config_(std::move(config)), ppu_(config_.ppu), timing_(Region::NTSC, config_.cpuRevision) {
  const auto seed = config_.entropySeed
                        ? (uint64_t(config_.entropySequence) << 32) | *config_.entropySeed
                        : gameboy::StartupState::SeedFromTime(uint64_t(std::time(nullptr)));
  handheldStartup_ = std::make_shared<gameboy::StartupState>();
  handheldStartup_->Configure(config_.entropyMode != EntropyMode::None, seed);
  broadcastReceiver_.SetTimeSource(CurrentBroadcastTime);
  Logger::Instance().Write(LogLevel::Info, "Emulator initialized");
}

Emulator::~Emulator() {
  StopFrameExecution();
}

// Platform attachment

void Emulator::SetExpansionDevice(ExpansionDevice device) {
  config_.expansionDevice = device;
  broadcastReceiver_.Reset();
}

void Emulator::AttachVideoOutput(IVideoOutput *output) {
  videoOutput_ = output;
}

void Emulator::AttachAudioOutput(IAudioOutput *output) {
  audioOutput_ = output;
}

void Emulator::AttachInputProvider(IInputProvider *input) {
  inputProvider_ = input;
}

void Emulator::AddTraceSink(ITraceSink *sink) {
  if (sink == nullptr) {
    return;
  }
  traceSinks_.push_back(sink);
}

// Cartridge loading

void Emulator::ReplaceCartridge(Cartridge &&cartridge) {
  StopFrameExecution();
  if (cartridge_)
    if (auto storage = cartridge_->CaptureCx4Storage())
      cx4Storage_ = std::move(*storage);
  if (cx4Storage_)
    cartridge.InitializeCx4Storage(*cx4Storage_);
  // Cartridge page storage samples the stream left by the previous power cycle.
  if (startupEntropy_)
    cartridge.InitializeFlashPages(*startupEntropy_);
  cartridge_ = std::move(cartridge);
}

bool Emulator::LoadCartridge(std::span<const uint8_t> romData, std::string *error) {
  return LoadCartridgeWithFirmware(romData, config_.cartridgeFirmware.get(), error);
}

bool Emulator::LoadCartridgeWithFirmware(std::span<const uint8_t> romData,
                                         const CartridgeFirmware *firmware, std::string *error) {
  RomNormalizationInfo normalization;
  auto cartridge =
      Cartridge::FromRomImage(romData, &normalization, &cartridgeDatabase_, error, firmware);
  if (!cartridge.has_value()) {
    Logger::Instance().Write(LogLevel::Error, "Failed to load cartridge image");
    return false;
  }

  if (auto unsupported = UnsupportedChipName(cartridge->Header().chip); unsupported.has_value()) {
    if (error != nullptr) {
      *error = std::string("Unsupported enhancement chip: ") + *unsupported + " (cartridge type $" +
               "0123456789ABCDEF"[(cartridge->Header().cartridgeType >> 4) & 0x0F] +
               "0123456789ABCDEF"[(cartridge->Header().cartridgeType >> 0) & 0x0F] + ")";
    }
    Logger::Instance().Write(LogLevel::Error, "Unsupported cartridge enhancement chip detected");
    return false;
  }

  ReplaceCartridge(std::move(*cartridge));
  Logger::Instance().Write(
      LogLevel::Info, "Cartridge loaded: " + cartridge_->Header().title +
                          (normalization.hadCopierHeader ? " [copier-header-normalized]" : "") +
                          (normalization.hadInterleave ? " [deinterleaved]" : ""));

  // Wire all subsystems now that we have a cartridge
  InitSubsystems();

  return true;
}

bool Emulator::LoadSufamiTurbo(std::span<const uint8_t> bios, std::span<const uint8_t> slotA,
                               std::span<const uint8_t> slotB, std::string *error) {
  auto cartridge = Cartridge::FromSufamiTurbo(bios, slotA, slotB, error);
  if (!cartridge)
    return false;
  ReplaceCartridge(std::move(*cartridge));
  InitSubsystems();
  return true;
}

bool Emulator::LoadCartridgeFromFile(const std::string &path, std::string *error) {
  const auto data = ReadImage(path, error);
  if (!data)
    return false;
  CartridgeFirmware adjacentFirmware;
  auto *firmware = config_.cartridgeFirmware.get();
  const auto directory = std::filesystem::path(path).parent_path() / "firmware";
  std::error_code fileError;
  if (!firmware && std::filesystem::is_directory(directory, fileError)) {
    if (!adjacentFirmware.LoadDirectory(directory, error))
      return false;
    firmware = &adjacentFirmware;
  }
  if (!LoadCartridgeWithFirmware(*data, firmware, error))
    return false;
  AttachMsu1(path);
  return true;
}

void Emulator::AttachMsu1(const std::string &path) {
  auto msu1 = std::make_unique<Msu1>();
  if (msu1->Open(path)) {
    msu1_ = std::move(msu1);
    // Complete pending cartridge work before accessing the shared audio timeline.
    const auto slot = bus_.RegisterHandler(
        [this](uint32_t address, uint8_t openBus) {
          SynchronizeCoprocessors(true);
          SynchronizeExpansionAudio(timing_.MasterClocksElapsed());
          return msu1_->Read(address, openBus);
        },
        [this](uint32_t address, uint8_t value) {
          SynchronizeCoprocessors(true);
          SynchronizeExpansionAudio(timing_.MasterClocksElapsed());
          msu1_->Write(address, value);
        });
    bus_.MapRange(0x00, 0x3f, 0x2000, 0x2007, slot);
    bus_.MapRange(0x80, 0xbf, 0x2000, 0x2007, slot);
    ConfigureAudioStreams();
    Logger::Instance().Write(LogLevel::Info, "MSU-1 data and PCM playback enabled");
  }
}

bool Emulator::LoadSuperGameBoy(std::span<const uint8_t> bios, std::span<const uint8_t> game,
                                std::span<const uint8_t> boot, std::string *error) {
  auto cartridge = Cartridge::FromSuperGameBoy(bios, game, boot, error);
  if (!cartridge)
    return false;
  ReplaceCartridge(std::move(*cartridge));
  InitSubsystems();
  return true;
}

bool Emulator::LoadSuperGameBoyFromFiles(const std::string &biosPath, const std::string &gamePath,
                                         const std::string &bootPath, std::string *error) {
  const auto bios = ReadImage(biosPath, error);
  if (!bios)
    return false;
  const auto game = ReadImage(gamePath, error);
  if (!game)
    return false;
  const auto boot = ReadImage(bootPath, error);
  if (!boot || !LoadSuperGameBoy(*bios, *game, *boot, error))
    return false;
  AttachMsu1(biosPath);
  return true;
}

bool Emulator::LoadBroadcastCartridge(std::span<const uint8_t> base, std::span<const uint8_t> pack,
                                      std::string *error) {
  auto cartridge = Cartridge::FromBroadcastCartridge(base, pack, error);
  if (!cartridge)
    return false;
  ReplaceCartridge(std::move(*cartridge));
  InitSubsystems();
  return true;
}

bool Emulator::LoadBroadcastCartridgeFromFiles(const std::string &basePath,
                                               const std::string &packPath, std::string *error) {
  const auto base = ReadImage(basePath, error);
  if (!base)
    return false;
  if (packPath.empty() || packPath == "-")
    return LoadBroadcastCartridge(*base, {}, error);
  const auto pack = ReadImage(packPath, error);
  return pack && LoadBroadcastCartridge(*base, *pack, error);
}

bool Emulator::LoadSufamiTurboFromFiles(const std::string &biosPath, const std::string &slotAPath,
                                        const std::string &slotBPath, std::string *error) {
  const auto bios = ReadImage(biosPath, error);
  if (!bios)
    return false;
  std::array<std::vector<uint8_t>, 2> slots;
  const std::array paths{slotAPath, slotBPath};
  for (unsigned i = 0; i < 2; ++i) {
    if (paths[i].empty() || paths[i] == "-")
      continue;
    auto image = ReadImage(paths[i], error);
    if (!image)
      return false;
    slots[i] = std::move(*image);
  }
  return LoadSufamiTurbo(*bios, slots[0], slots[1], error);
}

bool Emulator::LoadCompetitionCartridge(CompetitionBoard::Settings settings,
                                        const std::array<std::span<const uint8_t>, 4> &images,
                                        std::string *error) {
  auto cartridge =
      Cartridge::FromCompetitionCartridge(settings, images, error, config_.cartridgeFirmware.get());
  if (!cartridge)
    return false;
  ReplaceCartridge(std::move(*cartridge));
  InitSubsystems();
  return true;
}

bool Emulator::LoadCompetitionCartridgeFromFiles(CompetitionBoard::Settings settings,
                                                 const std::array<std::string, 4> &paths,
                                                 std::string *error) {
  std::array<std::vector<uint8_t>, 4> images;
  std::array<std::span<const uint8_t>, 4> views;
  for (unsigned i = 0; i < paths.size(); ++i) {
    auto image = ReadImage(paths[i], error);
    if (!image)
      return false;
    images[i] = std::move(*image);
    views[i] = images[i];
  }
  CartridgeFirmware adjacentFirmware;
  auto *firmware = config_.cartridgeFirmware.get();
  const auto directory = std::filesystem::path(paths[0]).parent_path() / "firmware";
  std::error_code fileError;
  if (!firmware && std::filesystem::is_directory(directory, fileError)) {
    if (!adjacentFirmware.LoadDirectory(directory, error))
      return false;
    firmware = &adjacentFirmware;
  }
  auto cartridge = Cartridge::FromCompetitionCartridge(settings, views, error, firmware);
  if (!cartridge)
    return false;
  ReplaceCartridge(std::move(*cartridge));
  InitSubsystems();
  return true;
}

const Cartridge *Emulator::LoadedCartridge() const noexcept {
  return cartridge_ ? &*cartridge_ : nullptr;
}

// InitSubsystems - wire callbacks, map bus, reset everything

void Emulator::InitSubsystems() {
  msu1_.reset();
  if (cartridge_->Header().chip == EnhancementChip::SuperGameBoy)
    cartridge_->InitializeGameBoyStartup(handheldStartup_);
  // 1. Inter-subsystem connections
  dsp_.SetRam(smp_.Ram());
  smp_.SetDsp(dsp_);
  smp_.SetCpuSynchronizationCallback([this] { SynchronizeDevices(true); });
  dma_.SetBus(&bus_);

  // 2. Map the 24-bit address space
  bus_.Reset();
  bus_.MapWram();
  bus_.MapPpu(ppu_);
  bus_.MapApu(smp_);
  bus_.MapCpuIo(cpuIo_);
  bus_.MapDma(dma_);
  bus_.MapCartridge(*cartridge_);
  const auto expansion = bus_.RegisterHandler(
      [this](uint32_t address, uint8_t openBus) {
        return config_.expansionDevice == ExpansionDevice::Satellaview
                   ? broadcastReceiver_.ReadCpu(address, openBus)
                   : openBus;
      },
      [this](uint32_t address, uint8_t value) {
        if (config_.expansionDevice == ExpansionDevice::Satellaview)
          broadcastReceiver_.WriteCpu(address, value);
      });
  bus_.MapRegions({{0x00, 0x3f, 0x2188, 0x219f}, {0x80, 0xbf, 0x2188, 0x219f}}, expansion);
  dma_.SetCartridge(&*cartridge_);

  // Map the DSP-1 data and status windows for the cartridge board.
  dsp1_.reset();
  dsp2_.reset();
  srtc_.reset();
  const auto chip = cartridge_->BoardConnected() && !cartridge_->HasSignalProcessor()
                        ? cartridge_->Header().chip
                        : EnhancementChip::None;
  const auto *signalBoard = cartridge_->SignalBoard();
  if (chip == EnhancementChip::Dsp1) {
    dsp1_ = std::make_unique<Dsp1>();
    auto *device = dsp1_.get();
    const auto slot = bus_.RegisterHandler(
        [device, signalBoard](uint32_t address, uint8_t) {
          return signalBoard->StatusPort(address) ? device->GetSr() : device->GetDr();
        },
        [device, signalBoard](uint32_t address, uint8_t value) {
          if (!signalBoard->StatusPort(address))
            device->SetDr(value);
        });
    signalBoard->MapDevice(bus_, slot);
  }
  if (chip == EnhancementChip::Dsp2) {
    dsp2_ = std::make_unique<Dsp2>();
    const auto slot = bus_.RegisterHandler(
        [this, signalBoard](uint32_t address, uint8_t) -> uint8_t {
          return signalBoard->StatusPort(address) ? 0 : dsp2_->Read();
        },
        [this, signalBoard](uint32_t address, uint8_t value) {
          if (!signalBoard->StatusPort(address))
            dsp2_->Write(value);
        });
    signalBoard->MapDevice(bus_, slot);
  }
  if (chip == EnhancementChip::Srtc) {
    srtc_ = std::make_unique<Srtc>();
    const auto slot = bus_.RegisterHandler(
        [this](uint32_t address, uint8_t openBus) {
          return (address & 1) ? openBus : srtc_->Read();
        },
        [this](uint32_t address, uint8_t data) {
          if (address & 1)
            srtc_->Write(data);
        });
    bus_.MapRange(0x00, 0x3f, 0x2800, 0x2801, slot);
    bus_.MapRange(0x80, 0xbf, 0x2800, 0x2801, slot);
  }

  // 3. Create the 65816 CPU (needs bus_ reference)
  cpu_ = std::make_unique<SnesCpu>(bus_);
  cpu_->SetInterruptPollCallback([this]() {
    if (irq_.IrqLocked())
      return false;
    if (irq_.NmiTest())
      cpu_->RequestNmi();
    // Keep the physical IRQ request visible while I is set so WAI wakes.
    const bool timerIrq = irq_.IrqTest(true);
    cpu_->SetIrqLevel(timerIrq || cartridge_->CpuIrqPending());
    return true;
  });

  // 4. Wire CpuIoRegisters Ã¢â€ â€™ subsystem callbacks

  // $4200 NMITIMEN Ã¢â€ â€™ IrqController + AutoJoypad
  cpuIo_.SetNmitimenCallback([this](uint8_t data) {
    irq_.NmitimenUpdate(data);
    autoJoypad_.SetAutoJoypadPoll((data & 0x01) != 0);
    cpuIo_.setAutoJoypadCounter(autoJoypad_.Counter());
  });

  // $420D MEMSEL Ã¢â€ â€™ MemoryBus fast-ROM + Cartridge access timing
  cpuIo_.SetMemselCallback([this](bool fast) {
    bus_.SetFastRom(fast);
    if (cartridge_) {
      cartridge_->SetMemselFast(fast);
    }
  });

  // Queue DMA for arbitration at a subsequent CPU bus-cycle boundary.
  cpuIo_.SetDmaEnableCallback([this](uint8_t channels) { dma_.EnableDma(channels); });
  dma_.SetClockCallback([this](uint32_t clocks) { AdvanceClocks(clocks); });
  dma_.SetCoprocessorSyncCallback([this]() { SynchronizeCoprocessors(); });
  dma_.SetClockQuery([this]() { return timing_.MasterClocksElapsed(); });
  dma_.SetCpuBusLatches(cpu_->regs().mar, cpu_->regs().mdr);
  dma_.SetBoundaryCallback([this]() { ServicePendingHdma(); });

  // $420C HDMAEN Ã¢â€ â€™ DMA controller
  cpuIo_.SetHdmaEnableCallback([this](uint8_t channels) { dma_.EnableHdma(channels); });

  // PPU counter latch ($4201 WRIO bit-7 falling edge)
  cpuIo_.SetPpuLatchCallback([this]() { ppu_.LatchCounters(timing_.HDot(), timing_.VCounter()); });
  cpuIo_.SetPioCallback([this](uint8_t pio) {
    ppu_.SetCpuPio(pio);
    autoJoypad_.Ports().SetPio(pio);
  });
  cpuIo_.SetPioInputCallback([this]() { return autoJoypad_.Ports().IoBits(); });
  autoJoypad_.Ports().SetGunLatchCallback(
      [this](uint16_t h, uint16_t v) { ppu_.LatchCounters(h, v); });

  // PPU counter latch ($2137 SLHV read when PIO bit 7 is high)
  ppu_.SetCounterLatchCallback(
      [this]() { ppu_.LatchCounters(timing_.HDot(), timing_.VCounter()); });

  // SETINI changes the VBlank boundary used by rendering, DMA and CPU I/O.
  ppu_.SetVDispCallback([this](uint16_t vdisp) {
    timing_.SetVDisp(vdisp);
    autoJoypad_.Ports().SetVDisp(vdisp);
  });

  // HVBJOY ($4212) timing query
  cpuIo_.SetTimingQueryCallback([this]() -> TimingQuery {
    return TimingQuery{
        timing_.HCounter(),
        timing_.VCounter(),
        timing_.VDisp(),
    };
  });

  // $4210 RDNMI Ã¢â€ â€™ IrqController
  cpuIo_.SetRdnmiCallback([this]() -> bool { return irq_.Rdnmi(); });

  // $4211 TIMEUP Ã¢â€ â€™ IrqController
  cpuIo_.SetTimeupCallback([this]() -> bool { return irq_.Timeup(); });

  // $4207-$420A H/V timer target writes Ã¢â€ â€™ IrqController
  cpuIo_.SetHVTimeChangeCallback([this](uint16_t htime, uint16_t vtime) {
    irq_.SetHTime(htime);
    irq_.SetVTime(vtime);
    irq_.PollTimer(timing_.HCounter(), timing_.VCounter(), timing_.PreviousHPeriod(),
                   timing_.PreviousVPeriod());
  });

  // 5. Wire Timing Ã¢â€ â€™ subsystem callbacks

  // V=0, H=0 - start of frame
  timing_.onFrameBegin = [this]() {
    ppu_.SetCurrentLine(0);
    ppu_.SetCurrentHClock(0);
    ppu_.FrameBegin();
    autoJoypad_.FrameBegin();
    autoJoypad_.Ports().FrameBegin();
  };
  timing_.onHdmaSetup = [this]() {
    dma_.HdmaReset();
    pendingHdmaSetup_ = dma_.AnyHdmaEnabled();
  };

  // New scanline
  timing_.onScanline = [this](uint16_t v) {
    SynchronizeDevices(true);
    SynchronizeExpansionAudio(coprocessorCompletedClock_);
    SynchronizeCoprocessors(true);
    ppu_.SetCurrentLine(v);
    ppu_.SetCurrentHClock(0);
  };

  // Cache visible scanlines at the render sampling point rather than H=0.
  timing_.onRenderCycle = [this](uint16_t v) { ppu_.ScanlineBegin(v); };

  // VBlank start (V = VDisp)
  timing_.onVBlankBegin = [this]() {
    autoJoypad_.Ports().LatchLightGuns();
    ppu_.VBlankBegin();
    SynchronizeDevices();
    if (execution_ && execution_->Running() && !singleInstruction_)
      SuspendFrame();
  };

  // NMI assertion point - handled indirectly by IrqController::Poll
  timing_.onNmiPoint = nullptr;

  // DRAM refresh - steal 40 master clocks from the CPU
  timing_.onDramRefresh = [this]() {
    cpu_->ApplyDramRefreshPenalty();
    pendingExtraClocks_ += Timing::kDramRefreshClocks;
  };

  // HDMA per visible scanline
  timing_.onHdmaTransfer = [this](uint16_t /*v*/) { pendingHdmaRun_ = dma_.AnyHdmaActive(); };

  // IRQ/NMI condition polling (every ~4 master clocks)
  timing_.onIrqPoll = [this](uint16_t h, uint16_t v, uint16_t vdisp, uint16_t) {
    irq_.Poll(h, v, vdisp, timing_.PreviousHPeriod(), timing_.PreviousVPeriod());
    autoJoypad_.Ports().BeamPosition(timing_.HDot(), v, vdisp - 1);
  };

  // Auto-joypad polling (every 128 master clocks)
  timing_.onJoypadPoll = [this](uint16_t h, uint16_t v, uint16_t vdisp) {
    autoJoypad_.Tick128(h, v, vdisp, (timing_.MasterClocksElapsed() & 255) == 0);
    // Synchronize joy registers into CpuIoRegisters
    cpuIo_.setJoy1(autoJoypad_.Joy1());
    cpuIo_.setJoy2(autoJoypad_.Joy2());
    cpuIo_.setJoy3(autoJoypad_.Joy3());
    cpuIo_.setJoy4(autoJoypad_.Joy4());
    cpuIo_.setAutoJoypadCounter(autoJoypad_.Counter());
  };

  // HBlank callback (not currently needed, but available)
  timing_.onHBlank = nullptr;

  // 6. Multiply/divide clocking during CPU cycles and DRAM refresh.
  cpu_->SetAluStepCallback([this](bool writeCycle) { cpuIo_.AluStep(writeCycle); });

  // 7. AutoJoypad input source
  autoJoypad_.SetInputCallback([this](int player) -> InputState {
    if (inputProvider_) {
      return inputProvider_->PollController(player, frameIndex_);
    }
    return {};
  });

  autoJoypad_.Ports().SetMouseCallback([this](int port) {
    return inputProvider_ ? inputProvider_->PollMouse(port, frameIndex_) : MouseState{};
  });
  autoJoypad_.Ports().SetGunCallback([this](int port, int gun) {
    return inputProvider_ ? inputProvider_->PollLightGun(port, gun, frameIndex_) : LightGunState{};
  });

  // Manual reads and automatic polling share the physical latch and clocks.
  cpuIo_.SetJoypadLatchCallback([this](bool latch) { autoJoypad_.SetManualLatch(latch); });

  cpuIo_.SetJoypadDataCallback(
      [this](int port) -> uint8_t { return autoJoypad_.Ports().Read(port); });

  ResetHardware(false);
  cpu_->resumeStep_ = [this]() { return ResumeCpuInstruction(); };
  cpu_->discardStep_ = [this]() { StopFrameExecution(); };
  cpu_->SetClockCallback([this](uint32_t clocks) { AdvanceClocks(clocks); });
  cpu_->SetCoprocessorSyncCallback([this]() { SynchronizeCoprocessors(); });
  cpu_->SetBeforeCycleCallback([this](uint32_t clocks) { BeginCpuCycle(clocks); });
  cpu_->SetBusPhaseCallback([this]() { irq_.SetIrqLock(false); });

  initialized_ = true;

  Logger::Instance().Write(LogLevel::Info, "Subsystems initialized");
}

bool Emulator::Reset() {
  if (!initialized_)
    return false;
  StopFrameExecution();
  ResetHardware(true);
  return true;
}

void Emulator::ResetHardware(bool warm) {
  broadcastReceiver_.Reset();
  cartridge_->SetSa1BusContention(config_.sa1BusContention);
  cartridge_->SetCx4DelayedSync(config_.cx4DelayedSync);
  const auto compatibility = detail::SelectCompatibility(
      cartridge_->Header(), config_.ppu, config_.entropyMode, config_.gameCompatibilityFixes);
  smp_.SetSampleRate(compatibility.sampleRate);
  const uint32_t seed = config_.entropySeed ? *config_.entropySeed : uint32_t(std::clock());
  startupEntropy_.emplace(compatibility.entropy, seed, config_.entropySequence);
  auto &startup = *startupEntropy_;
  if (!warm)
    startup.Fill({bus_.WramData(), MemoryBus::WramSize});
  if (compatibility.initializeWram)
    std::fill_n(bus_.WramData(), MemoryBus::WramSize, uint8_t{0xff});
  if (warm) {
    bus_.ResetRegisters();
    cartridge_->Reset();
    if (dsp1_)
      dsp1_->Reset();
    if (dsp2_)
      dsp2_->Reset();
    if (srtc_)
      srtc_->Reset();
    if (msu1_)
      msu1_->Reset();
    dsp_.SoftReset();
  } else {
    dsp_.Power();
  }
  if (compatibility.initializeDsp)
    for (unsigned reg = 0; reg < Dsp::RegisterCount; ++reg)
      dsp_.Write(uint8_t(reg), 0xff);
  smp_.SetDspMode(compatibility.perCycleDsp ? Smp::DspMode::PerCycle : config_.dspMode);
  // Audio RAM belongs to the console and survives cartridge replacement.
  smp_.Power(true);
  ppu_.Power(startup, compatibility.video, warm);
  cartridge_->InitializePowerOnMemory(startup);
  cpuIo_.Reset();
  cpuIo_.setCpuVersion(timing_.CpuRevision());
  dma_.Reset();
  irq_.Reset();
  autoJoypad_.Reset();
  timing_.Reset();

  timing_.SetRenderCycle(ppu_.Configuration().model == Ppu::RenderModel::Scanline
                             ? ppu_.Configuration().renderCycle
                             : Timing::kRenderCycle);
  const auto region = config_.regionOverride.value_or(
      detail::PalCountry(cartridge_->Header().country) ? Region::PAL : Region::NTSC);
  const bool pal = region == Region::PAL;
  timing_.SetRegion(region);
  cartridge_->SetPal(pal);
  cartridge_->SetGameBoyMixedAudioOutput([this](float left, float right) {
    if (collectingAudio_) {
      audioOutputBuffer_.interleavedStereo.push_back(left);
      audioOutputBuffer_.interleavedStereo.push_back(right);
    }
  });
  ConfigureAudioStreams();
  if (srtc_)
    srtc_->SetPal(pal);
  ppu_.SetPal(pal);
  frameIndex_ = 0;
  masterCycles_ = 0;
  pendingExtraClocks_ = 0;
  pendingHdmaSetup_ = false;
  pendingHdmaRun_ = false;
  servicingHdma_ = false;
  dmaArmed_ = false;
  collectingAudio_ = false;
  audioOutputBuffer_.interleavedStereo.clear();
  coprocessorsSyncedClock_ = 0;
  coprocessorClockTarget_ = 0;
  coprocessorCompletedClock_ = 0;

  // Set up DSP audio output buffer
  dsp_.SetOutput(audioBuf_.data(), kAudioBufSamples);

  // Queue reset after wiring; execution begins only when the console is stepped.
  cpu_->Reset();
}

// StepFrame - run one complete video frame

FrameStepResult Emulator::StepFrame(const FrameStepOptions &options) {
  // Stub path - no cartridge loaded yet
  if (!initialized_) {
    const auto input =
        inputProvider_ ? inputProvider_->PollController(0, frameIndex_) : InputState{};

    if (options.emitCpuBusTrace) {
      EmitTrace(CpuBusTraceEvent{
          .cycle = masterCycles_,
          .address = 0x00FFFC,
          .value = 0,
          .isWrite = false,
      });
    }
    if (options.emitPpuTrace) {
      EmitTrace(PpuTraceEvent{
          .cycle = masterCycles_,
          .registerAddress = 0x2100,
          .value = 0,
          .isWrite = false,
      });
    }
    if (options.emitDmaTrace) {
      EmitTrace(DmaTraceEvent{
          .cycle = masterCycles_,
          .channel = 0,
          .sourceAddress = 0x7E0000,
          .destinationAddress = 0x2118,
          .length = 0,
      });
    }

    if (videoOutput_) {
      auto &frame = videoOutputFrame_;
      frame.width = config_.visibleWidth;
      frame.height = config_.visibleHeight;
      frame.pixels.assign(static_cast<size_t>(frame.width) * frame.height, 0xFF000000);
      videoOutput_->Present(frame);
    }
    if (audioOutput_) {
      auto &audio = audioOutputBuffer_;
      audio.sampleRate = AudioTiming::SampleRate;
      audio.interleavedStereo.clear();
      audioOutput_->Submit(audio);
    }

    masterCycles_ += config_.masterCyclesPerFrame;
    ++frameIndex_;

    return FrameStepResult{
        .frameIndex = frameIndex_,
        .masterCycles = masterCycles_,
        .latchedInput = input,
    };
  }

  // Real frame loop
  autoJoypad_.Ports().PollLightGuns();
  const auto input = inputProvider_ ? inputProvider_->PollController(0, frameIndex_) : InputState{};

  // Prepare DSP audio buffer for this frame
  audioOutputBuffer_.interleavedStereo.clear();
  collectingAudio_ = audioOutput_ != nullptr;
  dsp_.ResetSamplesWritten();
  dsp_.SetOutput(audioBuf_.data(), kAudioBufSamples);

  if (!execution_) {
    execution_ = std::make_unique<detail::ExecutionContext>([this]() {
      try {
        for (;;) {
          cpu_->Step();
          if (singleInstruction_)
            execution_->Suspend();
        }
      } catch (...) {
        cpu_->stepSuspended_ = false;
        throw;
      }
    });
  }
  singleInstruction_ = false;
  execution_->Resume();
  collectingAudio_ = false;
  // End-of-frame processing

  // RenderFrame is now called from VBlankBegin() - no separate call here.
  // The previous duplicate call consumed V=1 from the next frame, causing
  // alternating 1-line / 223-line renders and incorrect scroll values.

  // Submit the aligned stereo stream.
  if (audioOutput_) {
    auto &audio = audioOutputBuffer_;
    audio.sampleRate = AudioTiming::SampleRate;
    audioOutput_->Submit(audio);
  }

  // Present video - extract only the visible region from the PPU's
  // internal buffer. Visible rows are written starting at output row 0,
  // so the frontend can copy them directly without preserving the hidden
  // 240-line border area.
  if (videoOutput_) {
    auto &frame = videoOutputFrame_;
    frame.width = ppu_.FrameWidth();
    frame.height = ppu_.FrameHeight();
    const uint32_t *src = ppu_.OutputData();
    if (src) {
      frame.pixels.resize(static_cast<size_t>(frame.width) * frame.height);
      for (uint32_t y = 0; y < frame.height; y++) {
        const uint32_t *row = src + static_cast<size_t>(y) * Ppu::OutputWidth;
        std::copy_n(row, frame.width, frame.pixels.data() + y * frame.width);
      }
    } else {
      frame.pixels.assign(static_cast<size_t>(frame.width) * frame.height, 0xFF000000);
    }
    videoOutput_->Present(frame);
  }

  // Update counters
  masterCycles_ = timing_.MasterClocksElapsed();
  ++frameIndex_;

  return FrameStepResult{
      .frameIndex = frameIndex_,
      .masterCycles = masterCycles_,
      .latchedInput = input,
  };
}

void Emulator::AdvanceClocks(uint32_t clocks) {
  // Cartridge clocks include the whole bus interval when a scanline event
  // interrupts the console partway through that interval.
  coprocessorClockTarget_ = timing_.MasterClocksElapsed() + clocks;
  timing_.SetInterlace(ppu_.Interlace());
  timing_.Tick(clocks);
  coprocessorCompletedClock_ = timing_.MasterClocksElapsed();
  // A penalty can trigger another refresh or HDMA event. Drain all of them
  // before the CPU resumes, including events near the frame boundary.
  while (pendingExtraClocks_ != 0) {
    const auto extra = pendingExtraClocks_;
    pendingExtraClocks_ = 0;
    coprocessorClockTarget_ = timing_.MasterClocksElapsed() + extra;
    timing_.Tick(extra);
    coprocessorCompletedClock_ = timing_.MasterClocksElapsed();
  }
  ppu_.SetCurrentHClock(timing_.HCounter());
  SynchronizeDevices();
}

void Emulator::SynchronizeCoprocessors(bool forceExecution) {
  // Retain elapsed debt until a register access or scanline handoff.
  if (config_.superFxDelayedSync && cartridge_->Header().chip == EnhancementChip::SuperFx &&
      !forceExecution && !SuperFx::SelectsRegisters(cpu_->regs().mar))
    return;
  // Charge the handheld oscillator after each completed CPU bus interval.
  // Its pending execution resumes at scanline handoffs.
  const auto elapsed = cartridge_->Header().chip == EnhancementChip::SuperGameBoy
                           ? coprocessorCompletedClock_
                           : std::max(timing_.MasterClocksElapsed(), coprocessorClockTarget_);
  auto delta = elapsed > coprocessorsSyncedClock_ ? elapsed - coprocessorsSyncedClock_ : 0;
  coprocessorsSyncedClock_ = elapsed;
  while (delta > std::numeric_limits<uint32_t>::max()) {
    cartridge_->AdvanceHardware(std::numeric_limits<uint32_t>::max(), cpu_->regs().mar,
                                cpu_->DramRefreshActive(), forceExecution);
    if (srtc_)
      srtc_->Advance(std::numeric_limits<uint32_t>::max());
    delta -= std::numeric_limits<uint32_t>::max();
  }
  cartridge_->AdvanceHardware(static_cast<uint32_t>(delta), cpu_->regs().mar,
                              cpu_->DramRefreshActive(), forceExecution);
  if (srtc_)
    srtc_->Advance(static_cast<uint32_t>(delta));
}

void Emulator::ConfigureAudioStreams() {
  msuSamplesClocked_ = 0;
  audioMixer_.SetOutput([this](float left, float right) {
    if (collectingAudio_) {
      audioOutputBuffer_.interleavedStereo.push_back(left);
      audioOutputBuffer_.interleavedStereo.push_back(right);
    }
  });
  audioMixer_.Reset();
  audioMixer_.SetRate(audio::StreamMixer::Input::Console, smp_.SampleRate());
  audioMixer_.SetRate(audio::StreamMixer::Input::Expansion, msu1_ ? Msu1::SampleRate : 0);
  cartridge_->SetGameBoyConsoleAudioRate(smp_.SampleRate());
  cartridge_->SetGameBoyExpansionAudioRate(msu1_ ? Msu1::SampleRate : 0);
}

void Emulator::SynchronizeExpansionAudio(uint64_t elapsed) {
  if (!msu1_)
    return;
  const uint64_t masterHz = timing_.MasterClockHz();
  const uint64_t target = (elapsed / masterHz) * Msu1::SampleRate +
                          ((elapsed % masterHz) * Msu1::SampleRate + masterHz - 1) / masterHz;
  const bool muted = (dsp_.Read(Dsp::kFlg) & 0x40) != 0;
  const bool adapter = cartridge_->Header().chip == EnhancementChip::SuperGameBoy;
  while (msuSamplesClocked_ < target) {
    const auto frame = msu1_->Clock(muted);
    ++msuSamplesClocked_;
    if (adapter)
      cartridge_->WriteGameBoyExpansionAudio(frame[0], frame[1]);
    else
      audioMixer_.Write(audio::StreamMixer::Input::Expansion, frame[0], frame[1]);
  }
}

void Emulator::SynchronizeDevices(bool communication) {
  if (!communication)
    return;
  const bool adapter = cartridge_->Header().chip == EnhancementChip::SuperGameBoy;
  const int firstSample = dsp_.SamplesWritten();
  // Exchange with the sound processor at scanlines and communication accesses.
  // A handoff can finish the current sound bus phase beyond the console deadline.
  smp_.SynchronizeCpu(coprocessorCompletedClock_, timing_.MasterClockHz());
  for (int sample = firstSample; sample < dsp_.SamplesWritten(); ++sample) {
    const auto left = audioBuf_[sample * 2];
    const auto right = audioBuf_[sample * 2 + 1];
    if (adapter)
      cartridge_->WriteGameBoyConsoleAudio(left, right);
    else
      audioMixer_.Write(audio::StreamMixer::Input::Console, double(left) / 32768,
                        double(right) / 32768);
  }
  // The mixed stream already owns every published sample, including headless
  // execution and debugger operations spanning several video fields.
  if (dsp_.SamplesWritten() == kAudioBufSamples)
    dsp_.ResetSamplesWritten();
}

void Emulator::SuspendFrame() {
  cpu_->stepSuspended_ = true;
  execution_->Suspend();
  cpu_->stepSuspended_ = false;
}

uint32_t Emulator::ResumeCpuInstruction() {
  const auto before = cpu_->Cycles();
  singleInstruction_ = true;
  cpu_->stepSuspended_ = false;
  execution_->Resume();
  singleInstruction_ = false;
  return static_cast<uint32_t>(cpu_->Cycles() - before);
}

void Emulator::StopFrameExecution() {
  execution_.reset();
  if (cpu_)
    cpu_->stepSuspended_ = false;
  singleInstruction_ = false;
  // Cancellation unwinds any DMA guards before a reset or cartridge reload.
  servicingHdma_ = false;
  dmaArmed_ = false;
}

void Emulator::BeginCpuCycle(uint32_t clocks) {
  dma_.SetCpuCycleClocks(clocks);
  // A request first arms the controller. One complete CPU cycle runs before
  // the controller takes the bus at the following cycle boundary.
  if (dmaArmed_) {
    ServicePendingHdma();
    if (dma_.AnyDmaEnabled()) {
      dma_.RunDma();
      irq_.SetIrqLock(true);
    }
    dmaArmed_ = false;
  }
  if (pendingHdmaSetup_ || pendingHdmaRun_ || dma_.AnyDmaEnabled())
    dmaArmed_ = true;
}

void Emulator::ServicePendingHdma() {
  if (servicingHdma_ || dma_.InHdma())
    return;
  servicingHdma_ = true;
  while (pendingHdmaSetup_ || pendingHdmaRun_) {
    if (pendingHdmaSetup_) {
      pendingHdmaSetup_ = false;
      if (dma_.AnyHdmaEnabled()) {
        dma_.HdmaSetup();
        irq_.SetIrqLock(true);
      }
    } else {
      pendingHdmaRun_ = false;
      if (dma_.AnyHdmaEnabled()) {
        dma_.HdmaRun();
        irq_.SetIrqLock(true);
      }
    }
  }
  servicingHdma_ = false;
}

// Accessors

uint64_t Emulator::CurrentFrame() const noexcept {
  return frameIndex_;
}

uint64_t Emulator::CurrentMasterCycles() const noexcept {
  return initialized_ ? timing_.MasterClocksElapsed() : masterCycles_;
}

void Emulator::EmitTrace(const TraceEvent &event) const {
  for (auto *sink : traceSinks_) {
    sink->OnTrace(event);
  }
}

} // namespace snes::core
