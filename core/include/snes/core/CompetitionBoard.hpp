#pragma once

#include "snes/core/Dsp1.hpp"
#include "snes/core/UpdDsp.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace snes::core {
class MemoryBus;

class CompetitionBoard {
public:
  enum class Board { CampusChallenge92, PowerFest94 };
  struct Settings {
    Board board = Board::CampusChallenge92;
    uint8_t dipSwitches = 0;
    size_t saveRamSize = 0x2000;
  };
  using Images = std::array<std::vector<uint8_t>, 4>;

  CompetitionBoard(Settings settings, Images images);
  bool LoadFirmware(std::span<const uint8_t> bytes);
  void Reset();
  void SerializeState(detail::StateArchive &archive);
  void SetPal(bool pal);
  void Advance(uint32_t masterClocks);
  void MapCpu(MemoryBus &bus, uint8_t slot) const;
  uint8_t Read(uint32_t address, uint8_t openBus);
  void Write(uint32_t address, uint8_t value);
  std::span<const uint8_t> BootRom() const noexcept;
  std::span<const uint8_t> SaveRam() const noexcept;
  void LoadRam(std::span<const uint8_t> bytes);
  bool HasSignalProcessor() const noexcept {
    return bool(processor_);
  }

private:
  static size_t Mirror(size_t address, size_t size);
  bool SelectsDsp(uint32_t address) const;
  bool SelectsDspStatus(uint32_t address) const;
  std::optional<size_t> SaveAddress(uint32_t address) const;
  std::optional<std::pair<unsigned, size_t>> RomAddress(uint32_t address) const;
  unsigned SelectedRom() const;
  void TickSeconds(uint64_t seconds);

  Settings settings_;
  Images images_;
  std::array<std::array<uint8_t, 32>, 4> imageDigests_{};
  std::vector<uint8_t> save_;
  std::unique_ptr<UpdDsp> processor_;
  std::unique_ptr<Dsp1> commandDsp_;
  uint32_t masterFrequency_;
  int64_t timerBalance_ = 0, dspBalance_ = 0;
  unsigned gameSeconds_ = 0, scoreSeconds_ = 0;
  uint8_t status_ = 0, select_ = 0;
};
} // namespace snes::core
