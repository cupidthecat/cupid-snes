// Divider-driven timer, serial transfer, and audio sequencer edges.
#pragma once

#include <cstdint>

namespace snes::core::detail {
class StateArchive;
}

namespace snes::core::gameboy {

class ClockedIo {
  public:
    void SerializeState(detail::StateArchive& archive);

    struct Bus {
        virtual ~Bus() = default;
        virtual void RequestInterrupt(uint8_t mask) = 0;
        virtual void AudioDividerEdge(bool rising) = 0;
    };

    explicit ClockedIo(Bus& bus) : bus_(bus) {
    }

    void Reset() noexcept;
    void Advance(unsigned clocks);
    uint8_t Read(uint16_t address) const noexcept;
    void Write(uint16_t address, uint8_t value);
    void EnterStop(bool interruptMasterEnabled);
    void LeaveStop() noexcept;

  private:
    enum class Reload { None, Pending, Complete };
    Bus& bus_;
    uint16_t divider_ = 8;
    unsigned untilTick_ = 4;
    uint8_t timer_ = 0, modulo_ = 0, control_ = 0;
    uint8_t serialData_ = 0, serialControl_ = 0x7e, serialBits_ = 0;
    Reload reload_ = Reload::None;
    bool serialClock_ = false, stopped_ = false;

    bool TimerSignal(uint16_t divider, uint8_t control) const noexcept;
    void SetDivider(uint16_t value);
    void IncrementTimer() noexcept;
    void SerialEdge();
};

} // namespace snes::core::gameboy
