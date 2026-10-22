#include "snes/core/gameboy/ClockedIo.hpp"

#include <algorithm>

namespace snes::core::gameboy {

void ClockedIo::Reset() noexcept {
  divider_ = 8;
  untilTick_ = 4;
  timer_ = modulo_ = control_ = serialData_ = serialBits_ = 0;
  serialControl_ = 0x7e;
  reload_ = Reload::None;
  serialClock_ = stopped_ = false;
}

bool ClockedIo::TimerSignal(uint16_t divider, uint8_t control) const noexcept {
  constexpr unsigned bits[] = {9, 3, 5, 7};
  return (control & 4) && (divider & (1u << bits[control & 3]));
}

void ClockedIo::IncrementTimer() noexcept {
  if (++timer_ == 0) {
    timer_ = modulo_;
    reload_ = Reload::Pending;
  }
}

void ClockedIo::SerialEdge() {
  serialClock_ = !serialClock_;
  if (serialClock_ || (serialControl_ & 0x81) != 0x81)
    return;
  serialData_ = uint8_t((serialData_ << 1) | 1);
  if (++serialBits_ == 8) {
    serialBits_ = 0;
    serialControl_ &= 0x7f;
    bus_.RequestInterrupt(8);
  }
}

void ClockedIo::SetDivider(uint16_t value) {
  const uint16_t previous = divider_;
  divider_ = value;
  if (TimerSignal(previous, control_) && !TimerSignal(value, control_))
    IncrementTimer();
  if ((previous & 0x80) && !(value & 0x80))
    SerialEdge();
  if ((previous ^ value) & 0x1000)
    bus_.AudioDividerEdge(value & 0x1000);
}

void ClockedIo::Advance(unsigned clocks) {
  if (stopped_)
    return;
  while (clocks) {
    const auto count = std::min(clocks, untilTick_);
    clocks -= count;
    untilTick_ -= count;
    if (untilTick_ != 0)
      continue;
    untilTick_ = 4;
    if (reload_ == Reload::Pending) {
      reload_ = Reload::Complete;
      bus_.RequestInterrupt(4);
    } else if (reload_ == Reload::Complete) {
      reload_ = Reload::None;
    }
    SetDivider(uint16_t(divider_ + 4));
  }
}

uint8_t ClockedIo::Read(uint16_t address) const noexcept {
  switch (address) {
  case 0xff01:
    return serialData_;
  case 0xff02:
    return serialControl_;
  case 0xff04:
    return uint8_t(divider_ >> 8);
  case 0xff05:
    return reload_ == Reload::Pending ? 0 : timer_;
  case 0xff06:
    return modulo_;
  case 0xff07:
    return control_ | 0xf8;
  default:
    return 0xff;
  }
}

void ClockedIo::Write(uint16_t address, uint8_t value) {
  switch (address) {
  case 0xff01:
    serialData_ = value;
    break;
  case 0xff02:
    serialBits_ = 0;
    if (serialClock_)
      SerialEdge();
    serialControl_ = value | 0x7e;
    break;
  case 0xff04:
    SetDivider(0);
    untilTick_ = 4;
    break;
  case 0xff05:
    if (reload_ != Reload::Complete)
      timer_ = value;
    break;
  case 0xff06:
    modulo_ = value;
    if (reload_ != Reload::None)
      timer_ = value;
    break;
  case 0xff07:
    if (TimerSignal(divider_, control_) && !TimerSignal(divider_, value))
      IncrementTimer();
    control_ = value;
    break;
  }
}

void ClockedIo::EnterStop(bool interruptMasterEnabled) {
  Write(0xff04, 0);
  if (!interruptMasterEnabled)
    untilTick_ += 4;
  stopped_ = true;
}

void ClockedIo::LeaveStop() noexcept {
  stopped_ = false;
}

} // namespace snes::core::gameboy
