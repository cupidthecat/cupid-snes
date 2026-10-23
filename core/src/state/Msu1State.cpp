#include "../cartridge/msu1/Resource.hpp"
#include "snes/core/Msu1.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <limits>

namespace snes::core {
bool Msu1::Stream::SerializeState(detail::StateArchive &archive, const std::filesystem::path &path,
                                  bool reuse) {
  if (!archive.Reading()) {
    archive.Validate(state_.opened == IsOpen(), "Invalid MSU-1 stream attachment");
    if (resource_) {
      const auto &key = resource_->Fingerprint(archive.Progress());
      state_.identity = key.digest;
      state_.mediaSize = key.size;
    }
  }
  State next;
  archive.Object(state_, [&](auto &fields, auto &state) {
    fields.Number(state.opened);
    fields.Bounded(state.size, uint64_t{0}, uint64_t(std::numeric_limits<int64_t>::max()));
    fields.Bounded(state.mediaSize, uint64_t{0}, uint64_t(std::numeric_limits<int64_t>::max()));
    fields.Bounded(state.cacheOffset, uint64_t{0}, state.size);
    fields.Bounded(state.cacheSize, 0u, uint32_t(state.cache.size()));
    fields.Validate(state.cacheSize <= state.size - state.cacheOffset,
                    "Invalid MSU-1 cache extent");
    fields.Array(state.identity);
    fields.Array(state.cache);
    fields.Validate(state.opened || (state.size == 0 && state.mediaSize == 0 &&
                                     state.cacheOffset == 0 && state.cacheSize == 0 &&
                                     std::all_of(state.identity.begin(), state.identity.end(),
                                                 [](uint8_t value) { return value == 0; })),
                    "Closed MSU-1 stream retains an active extent");
    next = state;
  });
  if (!archive.Reading())
    return next.opened;
  std::shared_ptr<Resource> prepared;
  if (next.opened) {
    archive.Validate(!path.empty(), "MSU-1 checkpoint needs attached media");
    if (reuse && resource_) {
      try {
        const auto &key = resource_->Fingerprint(archive.Progress());
        if (key.size == next.mediaSize && key.digest == next.identity)
          prepared = resource_;
      } catch (const std::runtime_error &) {
        // A fresh handle can recover a resource that the current stream lost.
      }
    }
    if (!prepared) {
      prepared = Resource::Open(path);
      archive.Validate(prepared && prepared->Size() == next.mediaSize,
                       "MSU-1 checkpoint media is missing or has a different size");
      const auto &key = prepared->Fingerprint(archive.Progress());
      archive.Validate(key.size == next.mediaSize && key.digest == next.identity,
                       "MSU-1 checkpoint media content differs");
    }
  }
  if (prepared)
    archive.RecheckBeforeRestore([prepared, size = next.mediaSize, identity = next.identity,
                                  operation = archive.Progress()]() {
      const auto &key = prepared->Fingerprint(operation);
      if (key.size != size || key.digest != identity)
        throw std::runtime_error("MSU-1 checkpoint media changed before restore");
    });
  archive.BeforeRestore(
      [this, prepared = std::move(prepared)]() noexcept { resource_ = prepared; });
  return next.opened;
}

void Msu1::SerializeState(detail::StateArchive &archive) {
  // The section also invalidates a caught resource-preparation failure.
  archive.Section(0x3155534d, [&](auto &fields) {
    fields.Number(dataSeek_);
    fields.Bounded(dataOffset_, uint64_t{0}, uint64_t(std::numeric_limits<int64_t>::max()));
    fields.Number(track_);
    const auto opened = fields.Number(openedTrack_);
    const auto status = fields.Number(status_);
    fields.Validate((status & ~0x38u) == 0 &&
                        (!(status & AudioError) || !(status & (Playing | Repeating))),
                    "Invalid MSU-1 playback flags");
    fields.Number(volume_);
    fields.Bounded(audioPosition_, uint64_t{0}, uint64_t(std::numeric_limits<int64_t>::max()));
    fields.Validate((fields.Number(playOffset_) & 3) == 0 && (fields.Number(loopOffset_) & 3) == 0,
                    "Invalid MSU-1 sample alignment");
    fields.Object(resume_, [](auto &values, auto &resume) {
      values.Number(resume.valid);
      values.Number(resume.track);
      values.Validate((values.Number(resume.offset) & 3) == 0, "Invalid MSU-1 resume alignment");
    });
    auto dataPath = basePath_, pcmPath = basePath_;
    if (!basePath_.empty()) {
      dataPath += ".msu";
      pcmPath += "-" + std::to_string(opened) + ".pcm";
    }
    const auto data = data_.SerializeState(fields, dataPath, true);
    const auto pcm = pcm_.SerializeState(fields, pcmPath, opened == openedTrack_);
    fields.Validate((!pcm || data) && (!data || pcm == !(status & AudioError)),
                    "Invalid MSU-1 stream and error flags");
  });
}
} // namespace snes::core
