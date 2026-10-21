#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace snes::core {

class PowerOnState;

class BsMemory {
public:
  // The cartridge owns the image and keeps its allocation stable.
  BsMemory(std::span<uint8_t> image, bool readOnly);
  void Reset();
  void InitializePageBuffers(PowerOnState &state);
  void SetWritable(bool writable);
  void SetPal(bool pal);
  // Accrue bus clocks while execution waits for a cartridge processor handoff.
  void Advance(uint32_t masterClocks, bool synchronizeProcessor = true);
  uint8_t Read(uint32_t address, uint8_t openBus = 0xff) const;
  void Write(uint32_t address, uint8_t value);
  std::vector<uint8_t> Metadata() const;
  bool LoadMetadata(std::span<const uint8_t> bytes);

private:
  enum class Mode { Array, Identifier, Page, Status, ExtendedStatus };
  struct Block {
    uint32_t erasures = 1;
    bool locked = true;
    bool erasing = false;
    uint8_t status = 0x80;
  };
  struct Entry {
    uint32_t address = 0;
    uint8_t value = 0;
  };
  size_t Mirror(uint32_t address) const;
  unsigned BlockIndex(uint32_t address) const;
  void Program(uint32_t address, uint8_t value);
  void BeginErase(unsigned block);
  void FinishErase(unsigned block);
  void Fail();
  void RunClock();
  void Execute();

  std::span<uint8_t> image_;
  bool readOnly_ = false, writable_ = false;
  unsigned blockCount_ = 0;
  uint16_t vendor_ = 0xb0, device_ = 0x66a8;
  uint64_t serial_ = 0x000123456789;
  std::array<Block, 64> blocks_{};
  std::array<std::array<uint8_t, 256>, 2> pages_{};
  std::array<Entry, 4> queue_{};
  unsigned queued_ = 0, readyMode_ = 3;
  Mode mode_ = Mode::Array;
  uint8_t status_ = 0x80, global_ = 0x86;
  int waitingErase_ = -1;
  uint32_t masterFrequency_ = 0;
  int64_t clock_ = 0;
};

} // namespace snes::core
