#include "snes/core/detail/StateArchive.hpp"
#include "snes/core/gameboy/Camera.hpp"
#include "snes/core/gameboy/Huc3Clock.hpp"
#include "snes/core/gameboy/Infrared.hpp"
#include "snes/core/gameboy/MotionEeprom.hpp"
#include "snes/core/gameboy/Tpp1.hpp"

#include <charconv>
#include <locale>
#include <sstream>

namespace snes::core::gameboy {
void MotionEeprom::SerializeState(detail::StateArchive &archive) {
  archive.Number(motionX_);
  archive.Number(motionY_);
  archive.Number(x_);
  archive.Number(y_);
  archive.Bounded(command_, uint16_t{0}, uint16_t{0x7ff});
  archive.Number(output_);
  archive.Bounded(dataBits_, uint8_t{0}, uint8_t{16});
  archive.Number(selected_);
  archive.Number(clock_);
  archive.Number(input_);
  archive.Number(dataOut_);
  archive.Number(writeEnabled_);
}
void Infrared::SerializeState(detail::StateArchive &archive) {
  archive.Bounded(charge_, uint64_t{0}, uint64_t{19900} + 2 * uint64_t{UINT32_MAX} - 1);
  archive.Number(input_);
  archive.Number(output_);
  archive.Number(detected_);
}
void Huc3Clock::SerializeState(detail::StateArchive &archive) {
  archive.Array(words_);
  archive.Number(timestamp_);
  archive.Number(cursor_);
  archive.Bounded(read_, uint8_t{0}, uint8_t{15});
  archive.Bounded(flags_, uint8_t{0}, uint8_t{15});
  archive.Number(alarmEnabled_);
}
void Tpp1::SerializeState(detail::StateArchive &archive) {
  archive.Array(clock_);
  archive.Array(latched_);
  archive.Number(timestamp_);
  archive.Bounded(phase_, uint64_t{0}, uint64_t{UINT32_MAX / 16 - 1});
  archive.Number(romBank_);
  archive.Number(ramBank_);
  const auto mode = archive.Number(mode_);
  archive.Validate(mode == 0 || mode == 2 || mode == 3 || mode == 5, "Invalid TPP1 access mode");
  archive.Validate((archive.Number(flags_) & ~uint8_t{12}) == 0, "Invalid TPP1 status bits");
  archive.Bounded(rumble_, uint8_t{0}, uint8_t{3});
  archive.Number(bankWritten_);
}
void Camera::SerializeState(detail::StateArchive &archive) {
  archive.Object(random_, [](auto &fields, auto &random) {
    // The engine's standard text form exposes its numeric state without native padding.
    std::ostringstream encoded;
    encoded.imbue(std::locale::classic());
    encoded << random;
    const auto text = encoded.str();
    uint64_t state = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), state);
    fields.Validate(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
                    "Invalid camera noise-generator state");
    fields.Number(state);
    if (fields.Reading())
      random.seed(state);
  });
  uint8_t control = 0;
  archive.Object(registers_, [&](auto &fields, auto &registers) {
    fields.Array(registers);
    control = registers[0];
    fields.Validate((control & ~7u) == 0, "Invalid camera control flags");
  });
  const auto frame = archive.Vector(frame_, 128 * 112);
  archive.Validate(frame == 0 || frame == 128 * 112, "Invalid camera sensor frame");
  const auto colors = archive.Vector(colors_, 128 * 112);
  archive.Validate(colors == 0 || colors == 128 * 112, "Invalid camera color cache");
  const auto remaining = archive.Bounded(remaining_, 0u, 129792u + 2048 + 65535 * 64 + 4);
  archive.Validate(bool(control & 1) == (remaining != 0), "Invalid camera capture phase");
  archive.Number(alignment_);
  archive.Number(noise_);
  archive.Number(randomEnabled_);
  const auto dirty = archive.Number(dirty_);
  archive.Validate(dirty || colors == 128 * 112, "Missing camera color cache");
}
} // namespace snes::core::gameboy
