// Cartridge metadata, address mapping, enhancement devices, and saves.

#pragma once

#include "snes/core/BsMemory.hpp"
#include "snes/core/Bsx.hpp"
#include "snes/core/CompetitionBoard.hpp"
#include "snes/core/Cx4.hpp"
#include "snes/core/Cx4Processor.hpp"
#include "snes/core/Dsp3.hpp"
#include "snes/core/Dsp4.hpp"
#include "snes/core/NecDsp.hpp"
#include "snes/core/NecDspBoard.hpp"
#include "snes/core/Obc1.hpp"
#include "snes/core/Sa1.hpp"
#include "snes/core/Sdd1.hpp"
#include "snes/core/SetaProtocol.hpp"
#include "snes/core/Spc7110.hpp"
#include "snes/core/St010.hpp"
#include "snes/core/St018Processor.hpp"
#include "snes/core/SuperFx.hpp"
#include <memory>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace snes::core {
class PowerOnState;
namespace gameboy {
class Adapter;
class StartupState;
} // namespace gameboy

enum class MappingType {
  Unknown,
  LoRom,
  HiRom,
  ExHiRom,
  ExLoRom,
  LoRomNoMad1,
  LoRom24Mbit,
  LoRomLargeSram,
  SufamiTurbo,
  BroadcastLoRom,
  BroadcastHiRom,
  Sdd1,
  St010,
  Spc7110,
  Sa1,
  SuperFx,
  St011,
  BroadcastSa1,
  Bsx,
  Competition92,
  Competition94
};

const char *MappingName(MappingType mapping) noexcept;

enum class EnhancementChip {
  None,
  Dsp1,
  Dsp2,
  Dsp3,
  Dsp4,
  Obc1,
  Srtc,
  SuperFx,
  Sa1,
  Sdd1,
  Spc7110,
  Spc7110Rtc,
  Cx4,
  St010,
  St011,
  St018,
  Competition,
  SuperGameBoy
};

const char *ChipName(EnhancementChip chip) noexcept;

enum class RomSpeed { Slow, Fast };

struct RomHeader {
  size_t headerOffset = 0;
  std::string title;
  MappingType mapping = MappingType::Unknown;
  RomSpeed speed = RomSpeed::Slow;
  uint8_t mapMode = 0;
  uint8_t maker = 0;
  EnhancementChip chip = EnhancementChip::None;
  uint8_t cartridgeType = 0;
  uint8_t romSizeShift = 0;
  uint8_t sramSizeShift = 0;
  uint8_t expansionRamSizeShift = 0;
  uint8_t country = 0;
  uint8_t version = 0;
  uint16_t resetVector = 0;
  uint16_t checksum = 0;
  uint16_t checksumComplement = 0;
  uint32_t score = 0;
};

struct RomNormalizationInfo {
  bool hadCopierHeader = false;
  bool hadInterleave = false;
};

struct DatabaseOverride {
  std::optional<MappingType> forceMapping;
  std::optional<size_t> forceSramSize;
  std::optional<RomSpeed> forceSpeed;
  std::optional<std::string> forceTitle;
};

class CartridgeDatabase {
public:
  CartridgeDatabase();

  void AddOverride(uint32_t crc32, DatabaseOverride entry);
  std::optional<DatabaseOverride> Find(uint32_t crc32) const;

private:
  std::unordered_map<uint32_t, DatabaseOverride> overrides_;
};

class Cartridge {
public:
  using GameBoyAudioSink = std::function<void(int16_t left, int16_t right, uint32_t sampleRate)>;
  Cartridge();
  ~Cartridge();
  Cartridge(Cartridge &&) noexcept;
  Cartridge &operator=(Cartridge &&) noexcept;
  static std::optional<Cartridge> FromRomImage(std::span<const uint8_t> romImage,
                                               RomNormalizationInfo *normalization,
                                               const CartridgeDatabase *database,
                                               std::string *error,
                                               const CartridgeFirmware *firmware = nullptr);

  static std::optional<Cartridge> FromSufamiTurbo(std::span<const uint8_t> bios,
                                                  std::span<const uint8_t> slotA,
                                                  std::span<const uint8_t> slotB,
                                                  std::string *error = nullptr);
  static std::optional<Cartridge> FromSuperGameBoy(std::span<const uint8_t> bios,
                                                   std::span<const uint8_t> game,
                                                   std::span<const uint8_t> boot,
                                                   std::string *error = nullptr);
  static std::optional<Cartridge> FromBroadcastCartridge(std::span<const uint8_t> base,
                                                         std::span<const uint8_t> pack,
                                                         std::string *error = nullptr);
  static std::optional<Cartridge> FromCompetitionCartridge(
      CompetitionBoard::Settings settings, const std::array<std::span<const uint8_t>, 4> &images,
      std::string *error = nullptr, const CartridgeFirmware *firmware = nullptr);

  const RomHeader &Header() const noexcept;
  uint32_t RomCrc32() const noexcept;

  // Reset device controls and internal work RAM as required. Retain battery RAM
  // and clock data.
  void Reset();
  void InitializePowerOnMemory(PowerOnState &state);
  void InitializeGameBoyStartup(std::shared_ptr<gameboy::StartupState> state);

  void SetMemselFast(bool enabled) noexcept;
  bool MemselFast() const noexcept;

  uint8_t Read(uint32_t cpuAddress, uint8_t openBus = 0xff) const;
  void Write(uint32_t cpuAddress, uint8_t value);

  void ObserveDmaWrite(uint32_t address, uint8_t value);

  uint32_t AccessCycles(uint32_t cpuAddress) const noexcept;
  void AdvanceHardware(uint32_t masterClocks, uint32_t cpuAddress = 0, bool cpuRefreshing = false,
                       bool synchronizeHandheld = true);
  void SetSa1BusContention(bool enabled);
  void SetPal(bool pal);
  bool CpuIrqPending() const noexcept;
  bool HasSignalProcessor() const noexcept {
    return bool(necDsp_) || (competition_ && competition_->HasSignalProcessor());
  }
  bool HasArmProcessor() const noexcept {
    return bool(armDsp_);
  }
  bool HasCx4Processor() const noexcept {
    return bool(cx4Processor_);
  }
  const NecDspBoard *SignalBoard() const noexcept {
    return signalBoard_ ? &*signalBoard_ : nullptr;
  }
  bool MapSignalProcessor(MemoryBus &bus, uint8_t slot) const;
  bool MapCx4Processor(MemoryBus &bus, uint8_t slot) const;
  bool MapSpc7110(MemoryBus &bus, uint8_t slot) const;
  bool MapCompetitionCpu(MemoryBus &bus, uint8_t slot) const;
  bool MapGameBoy(MemoryBus &bus, uint8_t slot) const;
  void SetGameBoyAudioOutput(GameBoyAudioSink output);
  void SetGameBoyMixedAudioOutput(std::function<void(float left, float right)> output);
  void WriteGameBoyConsoleAudio(int16_t left, int16_t right);
  void SetGameBoyConsoleAudioRate(uint32_t rate);
  void SetGameBoyExpansionAudioRate(uint32_t rate);
  void WriteGameBoyExpansionAudio(float left, float right);

  bool BoardConnected() const noexcept {
    return boardConnected_;
  }
  std::span<const uint8_t> RomData() const noexcept;
  std::span<const uint8_t> SramData() const noexcept;
  void LoadSram(std::span<const uint8_t> data);
  std::span<const uint8_t> SlotSramData(unsigned slot) const noexcept;
  void LoadSlotSram(unsigned slot, std::span<const uint8_t> data);
  std::vector<uint8_t> SaveSlotBattery(unsigned slot) const;
  bool LoadSlotBattery(unsigned slot, std::span<const uint8_t> data);
  std::span<const uint8_t> MemoryPackData() const noexcept;
  bool LoadMemoryPack(std::span<const uint8_t> data);
  std::vector<uint8_t> MemoryPackMetadata() const;
  bool LoadMemoryPackMetadata(std::span<const uint8_t> bytes);
  bool LoadBroadcastStream(uint16_t channel, uint8_t sequence, std::span<const uint8_t> data);
  bool SetBroadcastTimeSource(std::function<BroadcastTime()> source);
  std::vector<uint8_t> SaveRtc() {
    return spc7110_ ? spc7110_->SaveRtc() : std::vector<uint8_t>{};
  }
  bool LoadRtc(std::span<const uint8_t> data) {
    return spc7110_ && spc7110_->LoadRtc(data);
  }

private:
  void ApplyBiosCompatibility();
  void InitializeBroadcast(std::span<const uint8_t> pack = {});

  static std::vector<uint8_t> RemoveCopierHeader(std::span<const uint8_t> rom, bool *removed);
  std::optional<size_t> ResolveRomOffset(uint32_t cpuAddress) const;
  std::optional<size_t> ResolveSramOffset(uint32_t cpuAddress) const;
  bool SelectsLoRomSram(uint32_t cpuAddress) const;
  std::optional<size_t> ResolveMemoryPackOffset(uint32_t cpuAddress) const;
  uint8_t ReadMemoryPack(uint32_t cpuAddress, size_t offset) const;
  void WriteMemoryPack(uint32_t cpuAddress, size_t offset, uint8_t value);

  static uint32_t ComputeCrc32(std::span<const uint8_t> data);

  std::vector<uint8_t> rom_;
  std::vector<uint8_t> sram_;
  RomHeader header_{};
  uint32_t romCrc32_ = 0;
  bool memselFast_ = false;
  bool boardConnected_ = true;
  size_t slotSize_[2]{};
  size_t slotOffset_[2]{};
  std::vector<uint8_t> memoryPack_;
  bool memoryPackReadOnly_ = false;
  std::unique_ptr<BsMemory> memoryPackDevice_;
  std::unique_ptr<Sdd1> sdd1_;
  St010 st010_;
  std::unique_ptr<Spc7110> spc7110_;
  std::unique_ptr<Sa1> sa1_;
  std::unique_ptr<SuperFx> superFx_;
  std::unique_ptr<Cx4> cx4_;
  std::unique_ptr<Cx4Processor> cx4Processor_;
  std::unique_ptr<St011> st011_;
  std::unique_ptr<St018> st018_;
  std::unique_ptr<St018Processor> armDsp_;
  std::unique_ptr<Bsx> bsx_;
  std::unique_ptr<Dsp3> dsp3_;
  std::unique_ptr<Dsp4> dsp4_;
  std::unique_ptr<Obc1> obc1_;
  std::optional<NecDspBoard> signalBoard_;
  std::unique_ptr<NecDsp> necDsp_;
  std::unique_ptr<CompetitionBoard> competition_;
  std::unique_ptr<gameboy::Adapter> gameBoy_;
};

} // namespace snes::core
