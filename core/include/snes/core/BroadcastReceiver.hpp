// Console broadcast receiver registers, serial clock and recorded reception.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace snes::core {
namespace detail {
class StateArchive;
}

struct BroadcastTime {
    uint16_t year = 2000;
    uint8_t month = 1, day = 1, weekday = 7; // Sunday=1 through Saturday=7.
    uint8_t hour = 0, minute = 0, second = 0;
};

BroadcastTime CurrentBroadcastTime();

class BroadcastReceiver {
  public:
    BroadcastReceiver();
    ~BroadcastReceiver();
    BroadcastReceiver(const BroadcastReceiver&) = delete;
    BroadcastReceiver& operator=(const BroadcastReceiver&) = delete;

    // Reset port state while retaining supplied assets and the clock provider.
    void Reset();
    void SerializeState(detail::StateArchive& archive);
    static bool Selects(uint32_t address) noexcept;
    uint8_t ReadCpu(uint32_t address, uint8_t openBus);
    void WriteCpu(uint32_t address, uint8_t value);

    bool LoadStream(uint16_t channel, uint8_t sequence, std::span<const uint8_t> payload);
    void ClearStreams();
    void SetTimeSource(std::function<BroadcastTime()> source);
    std::function<BroadcastTime()> GetTimeSource() const;
    std::function<BroadcastTime()>
    ExchangeTimeSource(std::function<BroadcastTime()> source) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snes::core
