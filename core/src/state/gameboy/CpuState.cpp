#include "snes/core/detail/StateArchive.hpp"
#include "snes/core/gameboy/ClockedIo.hpp"
#include "snes/core/gameboy/Cpu.hpp"
#include "snes/core/gameboy/StartupState.hpp"

namespace snes::core::gameboy {
void Cpu::SerializeState(detail::StateArchive &archive) {
  archive.Object(state_, [](auto &fields, auto &state) {
    fields.Number(state.a);
    fields.Validate((fields.Number(state.f) & 15) == 0, "Invalid handheld CPU flags");
    fields.Number(state.b);
    fields.Number(state.c);
    fields.Number(state.d);
    fields.Number(state.e);
    fields.Number(state.h);
    fields.Number(state.l);
    fields.Number(state.sp);
    fields.Number(state.pc);
    fields.Number(state.ime);
    fields.Number(state.enablePending);
    fields.Number(state.halted);
    fields.Number(state.firstHaltWait);
    fields.Number(state.haltBug);
    fields.Number(state.stopped);
  });
}
void StartupState::SerializeState(detail::StateArchive &archive) {
  archive.Number(state_);
  archive.Number(enabled_);
}
void ClockedIo::SerializeState(detail::StateArchive &archive) {
  archive.Validate((archive.Number(divider_) & 3) == 0, "Invalid handheld divider alignment");
  archive.Bounded(untilTick_, 1u, 8u);
  archive.Number(timer_);
  archive.Number(modulo_);
  archive.Number(control_);
  archive.Number(serialData_);
  archive.Validate((archive.Number(serialControl_) & 0x7e) == 0x7e,
                   "Invalid handheld serial control bits");
  archive.Bounded(serialBits_, uint8_t{0}, uint8_t{7});
  archive.Enumeration(reload_, Reload::None, Reload::Complete);
  archive.Number(serialClock_);
  archive.Number(stopped_);
}
} // namespace snes::core::gameboy
