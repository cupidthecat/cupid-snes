#include "snes/core/audio/StreamMixer.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core::audio {
void StreamMixer::SerializeState(detail::StateArchive &archive) {
  for (auto &stream : streams_) {
    const auto rate = archive.Number(stream.rate);
    archive.Array(stream.history, [](auto &fields, auto &frame) { fields.Array(frame); });
    const auto count = archive.Vector(stream.ready, Stream::kQueueFrames,
                                      [](auto &fields, auto &frame) { fields.Array(frame); });
    archive.Validate(count == (rate ? Stream::kQueueFrames : 0),
                     "Invalid audio stream queue capacity");
    const auto read = archive.Bounded(stream.readIndex, 0u, count ? unsigned(count) - 1 : 0u);
    const auto write = archive.Bounded(stream.writeIndex, 0u, count ? unsigned(count) - 1 : 0u);
    const auto queued = archive.Number(stream.queued);
    archive.Validate(rate ? (read + queued % count) % count == write : queued == 0,
                     "Invalid audio stream queue position");
    archive.Bounded(stream.cursor, 0u, 3u);
    const auto interval = archive.Number(stream.interval);
    const auto next = archive.Number(stream.next);
    const auto expected = rate ? double(rate) / AudioTiming::SampleRate : 1;
    const auto tolerance = std::max(1.0, expected) * 8 * std::numeric_limits<double>::epsilon();
    archive.Validate(interval == expected && next >= 0 && next <= interval + tolerance,
                     "Invalid audio resampling phase");
  }
}
} // namespace snes::core::audio
