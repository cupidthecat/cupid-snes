// Video and sound devices on the handheld address and clock buses.
#pragma once

#include "snes/core/gameboy/Audio.hpp"
#include "snes/core/gameboy/Machine.hpp"
#include "snes/core/gameboy/Video.hpp"

namespace snes::core::detail {
class StateArchive;
}

namespace snes::core::gameboy {

class Peripherals final : public Machine::Devices {
public:
  void SerializeState(detail::StateArchive &archive);
  Peripherals(Video::Output &video, Audio::Output &audio) : video_(video), audio_(audio) {}
  void Reset(bool secondModel) override;
  void Reset(bool secondModel, bool preserveAudio) override;
  void InitializePowerOnMemory(std::span<const uint8_t, 160> objects,
                               std::span<const uint8_t, 16> wave) override;
  uint8_t Read(uint16_t address, Machine::Access access) override;
  uint8_t Write(uint16_t address, uint8_t value, Machine::Access access) override;
  uint8_t WriteCpu(uint16_t address, uint8_t value, uint16_t programCounter) override;
  uint8_t Advance(unsigned clocks) override;
  void FinishAdvance() override;
  void AudioDividerEdge(bool rising, uint64_t clock) override;
  void Stop(bool stopped) override;
  void WindowWrite(bool active) override;
  void AddressBus(uint16_t address, bool incremented) override;
  void BlockedOamAccess(uint16_t address, bool write) override;
  void SetDmaSynchronizer(std::function<void()> synchronize) override;
  void SetCpuSleeping(bool sleeping) override;
  uint8_t DmaStatus(uint16_t source, uint8_t position) override;

private:
  Video video_;
  Audio audio_;
  uint64_t clocks_ = 0;
};

} // namespace snes::core::gameboy
